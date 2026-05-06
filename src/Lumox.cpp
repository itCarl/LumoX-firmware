#include "Lumox.h"
#include "const.h"
#include <ElegantOTA.h>

void Lumox::begin() {
    LOG_PRINTLN("\n[Lumox] Booting...");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // _dmxLock is a portMUX (statically initialised in the header) — no
    // runtime construction. Just zero the manual array.
    for (auto& v : manualValue) v = -1;     // all channels AUTO at boot

    loadConfig();          // from NVS (or defaults on first boot)

    // Cache MAC once — efuse-backed, available before WiFi.begin(). Avoids
    // a syscall on every ArtPollReply (poll storms hit this hot).
    WiFi.macAddress(_macCached);

    // Ethernet is the PREFERRED interface — probe + IP acquire before WiFi
    // so beginNetwork() can shorten (or skip) the STA timeout when ETH is up.
    beginEthernet();
    beginNetwork();        // WiFi STA / AP fallback (informed by _ethUp)
    beginWebServer();      // AsyncWebServer binds to lwIP — reachable on WiFi + ETH
    beginArtNet();
    beginDmx();

    // DMX output on Core 1 — prio 5 preempts the Arduino loopTask (prio 1) and
    // AsyncTCP (prio 3) so HTTP/OTA traffic can't delay break/MAB. Stays well
    // below tcpip_thread (18) and WiFi tasks (23) so RX path is untouched.
    if (_dmxReady) {
        xTaskCreatePinnedToCore(_dmxTask, "DMX", 4096, this, /*prio*/ 5, nullptr, /*core*/ 1);
    }

    // Art-Net RX task on Core 1 — prio 4 preempts loopTask (1) + AsyncTCP (3),
    // yields to the DMX task (5). Decoupled drain so HTTP handlers / mDNS /
    // captive-portal DNS can't delay UDP pickup. UDP TX from this task is the
    // *only* TX path — cross-task announces queue via _pendingAnnounce.
    xTaskCreatePinnedToCore(_artNetTaskWrap, "ArtNet", 8192, this, /*prio*/ 4,
                            &_artNetTaskH, /*core*/ 1);

    digitalWrite(LED_PIN, HIGH);
    LOG_PRINTLN("[Lumox] Ready.\n");
}

void Lumox::loop() {
    // Art-Net RX (pollArtNet drain + loopArtNet housekeeping) runs on its own
    // task — see _artNetTaskWrap. loop() handles UI / LED / ETH-shim only.

    // Drive WebSocket + DNS + OTA. AsyncWebServer no longer needs a loop tick
    // — it runs on AsyncTCP's own task — so HTTP requests + OTA uploads no
    // longer stall ArtDMX intake.
    loopWebServer();

    // Status LED (stale-link indicator)
    _updateLed();

    // ETH plug/unplug + IP changes are event-driven via _onEthEvent() — kept
    // as a no-op shim so the call site stays stable.
    loopEthernet();

    const uint32_t now = millis();

    // Periodic state push to all WebSocket clients (~5 Hz)
    if (now - _lastWsPushMs >= 200) {
        _lastWsPushMs = now;
        if (_ws.count() > 0) pushStateToClients();
    }

    // Periodic ETH status dump — debug builds only (LUMOX_DEBUG=1). Helps
    // diagnose "W5500 not showing up on the router" on the bench. Production
    // builds skip the timer entirely so loop() is one branch lighter.
#if LUMOX_DEBUG
    const uint32_t dbgInterval = _ethUp ? 30000 : 5000;
    if (now - _ethLastDbgMs >= dbgInterval) {
        _ethLastDbgMs = now;
        debugEthStatus();
    }
#endif

    // DMX-health change detector. Logs once on transition + every 10 s while
    // unhealthy — avoids serial spam but makes flicker causes visible.
    static bool     lastDmxOk     = true;
    static uint32_t lastHealthMs  = 0;
    const DmxHealth hh = dmxHealth();
    if (hh.ok != lastDmxOk) {
        if (hh.ok) {
            LOG_PRINTF("[DMX] ✓ signal clean  frames=%u rate=%uHz\n",
                          (unsigned)statsDmxFramesSent, (unsigned)statsDmxRateHz);
        } else {
            LOG_PRINTF("[DMX] ⚠ UNCLEAN: %s  frames=%u rate=%uHz sendErr=%u waitTO=%u mutex=%uus\n",
                          hh.reason,
                          (unsigned)statsDmxFramesSent,
                          (unsigned)statsDmxRateHz,
                          (unsigned)statsDmxSendErrors,
                          (unsigned)statsDmxWaitTimeouts,
                          (unsigned)statsDmxMaxMutexUs);
        }
        lastDmxOk    = hh.ok;
        lastHealthMs = now;
    } else if (!hh.ok && now - lastHealthMs >= 10000) {
        LOG_PRINTF("[DMX] ⚠ still unclean: %s\n", hh.reason);
        lastHealthMs = now;
    }
}

// ── Status LED ─────────────────────────────────────────────────────────────
// Identify (OpAddress/OpCommand) → fast blink (200 ms period)
// Receiving Art-Net              → steady ON
// No ArtDMX > stale-ms           → slow pulse (60 ms on every 1200 ms)
// DMX output itself keeps transmitting the last buffer regardless.
void Lumox::_updateLed() {
    const uint32_t now = millis();

    if (_identifyUntilMs > now) {
        digitalWrite(LED_PIN, (now % 200) < 100 ? HIGH : LOW);
        return;
    }

    const uint32_t since = now - statsArtnetLastMs;
    if (statsArtnetLastMs == 0 || since > DMX_LINK_STALE_MS) {
        digitalWrite(LED_PIN, (now % 1200) < 60 ? HIGH : LOW);
    } else {
        digitalWrite(LED_PIN, HIGH);
    }
}

// ── Manual override API ───────────────────────────────────────────────────
// setManual: ch in [1..512], val in [0..255] = force; val = -1 = release.
// Maintains manualActiveBits + manualActiveCount in O(1) so batch POSTs of
// dozens of channels no longer trigger a 512-iteration rescan per call.
void Lumox::setManual(int ch, int val) {
    if (ch < 1 || ch > 512) return;
    if (val < -1 || val > 255) return;

    const bool wasActive = (manualValue[ch] >= 0);
    const bool nowActive = (val >= 0);
    manualValue[ch] = (int16_t)val;

    if (wasActive == nowActive) return;       // value-only change; bit unchanged

    const uint32_t bit  = 1u << ((ch - 1) & 31);
    const int      word = (ch - 1) >> 5;
    if (nowActive) {
        manualActiveBits[word] |= bit;
        manualActiveCount++;
    } else {
        manualActiveBits[word] &= ~bit;
        manualActiveCount--;
    }
    manualActive = (manualActiveCount > 0);
}

void Lumox::clearAllManual() {
    for (auto& v : manualValue)        v = -1;
    for (auto& w : manualActiveBits)   w =  0;
    manualActiveCount = 0;
    manualActive      = false;
}

void Lumox::setManualEnabled(bool on) {
    manualEnabled = on;
    LOG_PRINTF("[Manual] master switch %s\n", on ? "ON" : "OFF");
}

// ── FreeRTOS task: continuously transmits DMX (~44 Hz) ─────────────────────
// Also measures signal-quality metrics consumed by dmxHealth():
//   • mutex-take duration (contention with Art-Net parser)
//   • actual frame rate (drops when writeDmx fails or blocks too long)
void Lumox::_dmxTask(void* param) {
    auto& ctrl = *static_cast<Lumox*>(param);

    uint8_t  frame[513];
    uint32_t rateSampleCount = 0;
    uint32_t rateSampleStart = millis();
    constexpr uint32_t RATE_SAMPLE_FRAMES = 50;   // ~1 s at 44 Hz

    for (;;) {
        const uint32_t lockStart = micros();
        portENTER_CRITICAL(&ctrl._dmxLock);
        const uint32_t lockUs = micros() - lockStart;
        memcpy(frame, ctrl.dmxBuffer, 513);
        portEXIT_CRITICAL(&ctrl._dmxLock);

        if (lockUs > ctrl.statsDmxMaxMutexUs) ctrl.statsDmxMaxMutexUs = lockUs;

        // Rolling decay of the high-water mark — keep MUTEX_DECAY_MS of recent
        // history. Avoids /health staying pinned UNCLEAN forever after a single
        // boot-time spike (typical: WiFi assoc + DHCP all on first second).
        const uint32_t nowMs = millis();
        if (nowMs - ctrl._mutexDecayMs >= MUTEX_DECAY_MS) {
            uint32_t recentMax = 0;
            for (uint8_t i = 0; i < ctrl._mutexLogCount; i++) {
                if (nowMs - ctrl._mutexLog[i].timeMs <= MUTEX_DECAY_MS &&
                    ctrl._mutexLog[i].durationUs > recentMax) {
                    recentMax = ctrl._mutexLog[i].durationUs;
                }
            }
            ctrl.statsDmxMaxMutexUs = recentMax;
            ctrl._mutexDecayMs      = nowMs;
        }

        // Ring-buffer log of every spike above the threshold so /health can
        // show a timeline of contention events, not just the high-water mark.
        if (lockUs > Lumox::MUTEX_LOG_THRESHOLD_US) {
            ctrl._mutexLog[ctrl._mutexLogHead] = { millis(), lockUs };
            ctrl._mutexLogHead = (ctrl._mutexLogHead + 1) % Lumox::MUTEX_LOG_SIZE;
            if (ctrl._mutexLogCount < Lumox::MUTEX_LOG_SIZE) ctrl._mutexLogCount++;
        }

        // Manual override overlay — applied AFTER the Art-Net snapshot so
        // user-driven channels win over incoming ArtDMX. Iterates the active
        // bitmap (set bits only) — sparse overrides skip the 512-channel scan.
        uint16_t txLen = ctrl.dmxChannelCount;
        if (ctrl.manualEnabled && ctrl.manualActive) {
            txLen = 512;                    // ensure manual slots are sent
            for (int w = 0; w < 16; w++) {
                uint32_t bits = ctrl.manualActiveBits[w];
                while (bits) {
                    const int b  = __builtin_ctz(bits);
                    bits        &= bits - 1;
                    const int ch = (w << 5) + b + 1;     // [1..512]
                    const int16_t m = ctrl.manualValue[ch];
                    if (m >= 0) frame[ch] = (uint8_t)m;  // re-check vs torn write
                }
            }
        }

        // Zero the slots beyond txLen to preserve the prior partial-frame
        // semantics (controller sends 100 ch → channels 101..512 transmit 0).
        // dmxBuffer keeps last values, so without this we'd send stale data.
        if (txLen < 512) memset(&frame[txLen + 1], 0, 512 - txLen);
        frame[0] = 0;                       // start code (defensive)

#if LUMOX_CH1_PIN_NONZERO
        // Slot-1 pin: force frame[1] = 0x01 so the wire never carries
        // start-code + slot-1 both as 0x00. Some cheap moving-head receivers
        // misdetect that zero run as a fresh BREAK and resync mid-frame —
        // visible as jitter on slots far beyond ch 1. Last write before TX so
        // it overrides Art-Net + manual override on this slot.
        if (ctrl.cfgCh1PinNonzero) frame[1] = 0x01;
#endif

        ctrl.writeDmx(frame);
        // writeDmx blocks until the frame is fully sent (~22 ms)

        // Rolling frame-rate measurement — recomputed every RATE_SAMPLE_FRAMES.
        if (++rateSampleCount >= RATE_SAMPLE_FRAMES) {
            const uint32_t now   = millis();
            const uint32_t dt_ms = now - rateSampleStart;
            if (dt_ms > 0) {
                const uint32_t hz10     = (rateSampleCount * 10000) / dt_ms;
                ctrl.statsDmxRateHz     = hz10 / 10;
                ctrl.statsDmxRateTenths = hz10 % 10;
            }
            rateSampleCount = 0;
            rateSampleStart = now;
        }
    }
}

// ── FreeRTOS task: dedicated Art-Net RX + housekeeping ────────────────────
// Pinned to Core 1, prio 4. Owns the single _udp socket — every TX path
// (sendArtPollReply, ArtAddress reply, etc.) is reached from inside this task
// via the parser dispatcher, so there's no cross-task UDP access. The one
// exception is announceArtNetNode() which can fire from the WiFi/ETH event
// task — that path now flips _pendingAnnounce instead, drained here.
void Lumox::_artNetTaskWrap(void* param) {
    auto& ctrl = *static_cast<Lumox*>(param);
    for (;;) {
        if (ctrl._pendingAnnounce) {
            ctrl._pendingAnnounce = false;
            ctrl.sendArtPollReply(IPAddress(255, 255, 255, 255));
        }
        while (ctrl.pollArtNet()) { /* drain */ }
        ctrl.loopArtNet();
        vTaskDelay(1);          // yield 1 tick (~1 ms) — half a DMX frame
    }
}
