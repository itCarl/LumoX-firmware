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

    // Cache MAC once — efuse-backed, available before WiFi.begin().
    WiFi.macAddress(_macCached);

    // Ethernet is the PREFERRED interface — probe + IP acquire before WiFi
    // so beginNetwork() can shorten (or skip) the STA timeout when ETH is up.
    beginEthernet();
    beginNetwork();        // WiFi STA / AP fallback (informed by _ethUp)
    beginWebServer();      // AsyncWebServer binds to lwIP — reachable on WiFi + ETH
    beginProtocol();       // Art-Net / E1.31 / API per cfgProtocol
    beginDmx();

    // DMX output on Core 1 — prio 5 preempts loopTask (1) and AsyncTCP (3).
    if (_dmxReady) {
        xTaskCreatePinnedToCore(_dmxTask, "DMX", 4096, this, /*prio*/ 5, nullptr, /*core*/ 1);
    }

    // Protocol RX task on Core 1 — prio 4. Decoupled drain so HTTP / mDNS /
    // captive-portal DNS can't delay UDP pickup. UDP TX from this task is the
    // *only* TX path — cross-task announces queue via _pendingNetEvent.
    xTaskCreatePinnedToCore(_protocolTaskWrap, "Proto", 8192, this, /*prio*/ 4,
                            &_protoTaskH, /*core*/ 1);

    digitalWrite(LED_PIN, HIGH);
    LOG_PRINTLN("[Lumox] Ready.\n");
}

void Lumox::loop() {
    // Protocol RX (drain + housekeeping) runs on its own task — see
    // _protocolTaskWrap. loop() handles UI / LED / ETH-shim only.
    loopWebServer();
    _updateLed();
    loopEthernet();

    const uint32_t now = millis();

    if (now - _lastWsPushMs >= 200) {
        _lastWsPushMs = now;
        if (_ws.count() > 0) pushStateToClients();
    }

#if LUMOX_DEBUG
    const uint32_t dbgInterval = _ethUp ? 30000 : 5000;
    if (now - _ethLastDbgMs >= dbgInterval) {
        _ethLastDbgMs = now;
        debugEthStatus();
    }
#endif

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

// ── Protocol lifecycle ─────────────────────────────────────────────────────
// Single active source, picked by cfgProtocol. Today: created once in begin()
// — switching protocols requires a reboot (cleaner socket teardown).
void Lumox::beginProtocol() {
    if (_source) {
        // Defensive — should never happen at boot, but keep the path safe
        // if a future hot-swap path calls beginProtocol() twice.
        _source->end();
        delete _source;
        _source = nullptr;
    }
    _source = createDmxSource(cfgProtocol);
    if (!_source) {
        LOG_PRINTF("[Proto] Unknown protocol type %u — defaulting to Art-Net\n",
                      (unsigned)cfgProtocol);
        cfgProtocol = ProtocolType::ArtNet;
        _source = createDmxSource(cfgProtocol);
    }
    LOG_PRINTF("[Proto] Active source: %s\n", _source->name());
    if (!_source->begin()) {
        LOG_PRINTF("[Proto] Source %s failed to start\n", _source->name());
    }
}

void Lumox::endProtocol() {
    if (!_source) return;
    _source->end();
    delete _source;
    _source = nullptr;
}

// ── Unified DMX ingestion ─────────────────────────────────────────────────
// Sources call this after their own validation (header / sequence / priority).
// Single-sender lock + sender-swap detection live here so every protocol
// benefits without reimplementing the policy.
bool Lumox::feedDmxFrame(const uint8_t* slots, uint16_t len, IPAddress sender) {
    if (len > 512) len = 512;

    // Single-sender lock: while the current primary is live (< stale window),
    // reject packets from any other IP. Stale primary → next sender takes over.
    if (statsArtnetLastMs != 0 && sender != statsArtnetSender) {
        const uint32_t since = millis() - statsArtnetLastMs;
        if (since < DMX_LINK_STALE_MS) {
            statsArtnetLocked++;
            return false;
        }
    }

    // Sender-swap detection
    if (statsArtnetSender != sender) {
        if ((uint32_t)statsArtnetSender != 0) {
            statsArtnetSenderSwaps++;
            statsArtnetLastSwapMs = millis();
            LOG_PRINTF("[DMX] ⚠ sender swap %s → %s (total %u)\n",
                          statsArtnetSender.toString().c_str(),
                          sender.toString().c_str(),
                          (unsigned)statsArtnetSenderSwaps);
        }
        statsArtnetSender = sender;
    }

    portENTER_CRITICAL(&_dmxLock);
    memcpy(&dmxBuffer[1], slots, len);
    dmxChannelCount = len;
    portEXIT_CRITICAL(&_dmxLock);

    statsArtnetPackets++;
    statsArtnetLastMs = millis();
    return true;
}

void Lumox::setIdentify(uint32_t durationMs) {
    _identifyUntilMs = (durationMs > 0) ? (millis() + durationMs) : 0;
}

void Lumox::clearDmxBuffer() {
    portENTER_CRITICAL(&_dmxLock);
    memset(&dmxBuffer[1], 0, 512);
    portEXIT_CRITICAL(&_dmxLock);
}

// ── Status LED ─────────────────────────────────────────────────────────────
// Identify (any source's locate cmd) → fast blink (200 ms period)
// Receiving DMX                      → steady ON
// No DMX > stale-ms                  → slow pulse
void Lumox::_updateLed() {
    const uint32_t now = millis();

    const bool identify = (_identifyUntilMs > now) ||
                          (_source && _source->identifyActive());
    if (identify) {
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
void Lumox::setManual(int ch, int val) {
    if (ch < 1 || ch > 512) return;
    if (val < -1 || val > 255) return;

    const bool wasActive = (manualValue[ch] >= 0);
    const bool nowActive = (val >= 0);
    manualValue[ch] = (int16_t)val;

    if (wasActive == nowActive) return;

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
void Lumox::_dmxTask(void* param) {
    auto& ctrl = *static_cast<Lumox*>(param);

    uint8_t  frame[513];
    uint32_t rateSampleCount = 0;
    uint32_t rateSampleStart = millis();
    constexpr uint32_t RATE_SAMPLE_FRAMES = 50;

    for (;;) {
        const uint32_t lockStart = micros();
        portENTER_CRITICAL(&ctrl._dmxLock);
        const uint32_t lockUs = micros() - lockStart;
        memcpy(frame, ctrl.dmxBuffer, 513);
        portEXIT_CRITICAL(&ctrl._dmxLock);

        if (lockUs > ctrl.statsDmxMaxMutexUs) ctrl.statsDmxMaxMutexUs = lockUs;

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

        if (lockUs > Lumox::MUTEX_LOG_THRESHOLD_US) {
            ctrl._mutexLog[ctrl._mutexLogHead] = { millis(), lockUs };
            ctrl._mutexLogHead = (ctrl._mutexLogHead + 1) % Lumox::MUTEX_LOG_SIZE;
            if (ctrl._mutexLogCount < Lumox::MUTEX_LOG_SIZE) ctrl._mutexLogCount++;
        }

        // Hold-last-frame timeout. cfgHoldTimeoutMs == 0 → keep last frame
        // forever (default — flicker-resistant). >0 → blackout the wire after
        // N ms without input. Applied BEFORE the manual-override overlay so
        // user-forced channels still come through during a blackout window.
        if (ctrl.cfgHoldTimeoutMs > 0 && ctrl.statsArtnetLastMs != 0) {
            const uint32_t since = millis() - ctrl.statsArtnetLastMs;
            if (since > ctrl.cfgHoldTimeoutMs) {
                memset(&frame[1], 0, 512);
            }
        }

        uint16_t txLen = ctrl.dmxChannelCount;
        if (ctrl.manualEnabled && ctrl.manualActive) {
            txLen = 512;
            for (int w = 0; w < 16; w++) {
                uint32_t bits = ctrl.manualActiveBits[w];
                while (bits) {
                    const int b  = __builtin_ctz(bits);
                    bits        &= bits - 1;
                    const int ch = (w << 5) + b + 1;
                    const int16_t m = ctrl.manualValue[ch];
                    if (m >= 0) frame[ch] = (uint8_t)m;
                }
            }
        }

        if (txLen < 512) memset(&frame[txLen + 1], 0, 512 - txLen);
        frame[0] = 0;

#if LUMOX_CH1_PIN_NONZERO
        if (ctrl.cfgCh1PinNonzero) frame[1] = 0x01;
#endif

        ctrl.writeDmx(frame);

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

// ── Protocol task ──────────────────────────────────────────────────────────
// Pinned to Core 1, prio 4. Owns the active source's UDP socket(s) — every TX
// path (PollReply, ArtAddress reply, etc.) is reached from inside the source's
// loop() so there's no cross-task UDP access. The one exception is
// announceProtocolNode() which can fire from the WiFi/ETH event task — it
// flips _pendingNetEvent and we drain it here.
void Lumox::_protocolTaskWrap(void* param) {
    auto& ctrl = *static_cast<Lumox*>(param);
    for (;;) {
        if (ctrl._pendingNetEvent) {
            ctrl._pendingNetEvent = false;
            if (ctrl._source) ctrl._source->onNetworkChange();
        }
        if (ctrl._source) ctrl._source->service();
        vTaskDelay(1);
    }
}
