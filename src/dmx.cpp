#include "Lumox.h"
#include "const.h"
#include <esp_dmx.h>

void Lumox::beginDmx() {
    // NOTE: esp_dmx 4.x crashes when called with personalities=nullptr, count=0.
    // Supply a single dummy personality with footprint=512 (we always send a
    // full universe). Library requires footprint in (0, DMX_PACKET_SIZE_MAX).
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    dmx_personality_t personalities[] = {
        { 512, "Lumox DMX Controller" },
    };
    const int personality_count = sizeof(personalities) / sizeof(personalities[0]);

    if (!dmx_driver_install(_dmxPort, &config, personalities, personality_count)) {
        LOG_PRINTLN("[DMX] driver install FAILED — DMX output disabled");
        _dmxReady = false;
        return;
    }
    dmx_set_pin(_dmxPort, DMX_TX_PIN, DMX_RX_PIN, DMX_DE_PIN);
    _dmxReady = true;

    LOG_PRINTF("[DMX] Output on UART%d  TX=%d  RX=%d  DE=%d\n",
                  DMX_UART_PORT, DMX_TX_PIN, DMX_RX_PIN, DMX_DE_PIN);
}

// Writes a full DMX frame and blocks until it has been sent.
// Called exclusively from the FreeRTOS DMX task. Caller owns `frame` (513 B,
// [0]=0x00, [1..512]=channels) — no extra zero/memcpy on the hot path.
//
// Return-value checks matter for flicker: dmx_send() returning 0 means the
// frame never hit the wire (driver queue full / misconfig). dmx_wait_sent()
// returning false means the UART didn't drain within the timeout — the next
// frame would start before the current one completes, corrupting break/MAB
// timing. Both increment error counters; a running streak drives dmxHealth().
void Lumox::writeDmx(uint8_t* frame) {
    if (!_dmxReady) {
        vTaskDelay(pdMS_TO_TICKS(20));      // DMX disabled — idle task
        return;
    }

    dmx_write(_dmxPort, frame, DMX_PACKET_SIZE);
    const size_t sent      = dmx_send(_dmxPort);
    const bool   waited_ok = dmx_wait_sent(_dmxPort, DMX_TIMEOUT_TICK);

    const bool frameOk = (sent > 0) && waited_ok;
    if (frameOk) {
        statsDmxFramesSent++;
        statsDmxLastFrameMs  = millis();
        statsDmxConsecErrors = 0;
    } else {
        if (sent == 0)      statsDmxSendErrors++;
        if (!waited_ok)     statsDmxWaitTimeouts++;
        statsDmxConsecErrors++;
        if (statsDmxConsecErrors > statsDmxMaxConsecErr) {
            statsDmxMaxConsecErr = statsDmxConsecErrors;
        }
        // Warn once per streak of 3 — one-off errors are common on boot.
        if (statsDmxConsecErrors == 3) {
            LOG_PRINTF("[DMX] ⚠ consecutive send failures  send=%u waitTO=%u\n",
                          (unsigned)statsDmxSendErrors,
                          (unsigned)statsDmxWaitTimeouts);
        }
    }
}

// ── Explicit DMX signal health check ──────────────────────────────────────
// Each criterion is ordered from fatal → noisy. Returns the FIRST failed one,
// so the user sees the actionable problem first.
Lumox::DmxHealth Lumox::dmxHealth() const {
    DmxHealth h = { true, "" };

    if (!_dmxReady) { h.ok = false; h.reason = "Driver install failed";          return h; }

    if (statsDmxConsecErrors > 3) {
        h.ok = false; h.reason = "UART send failing (consecutive errors)";       return h;
    }

    // 25 Hz = well under the 44 Hz Art-Net target. Anything lower = visible.
    // First few seconds after boot the counter is 0 — ignore until we have data.
    if (statsDmxFramesSent > 100 && statsDmxRateHz > 0 && statsDmxRateHz < 25) {
        h.ok = false; h.reason = "Frame rate too low";                           return h;
    }

    // 5 ms mutex wait = DMX task blocked on Art-Net writes. At 44 Hz one frame
    // is ~22 ms, so 5 ms is the line where jitter becomes visible on fades.
    if (statsDmxMaxMutexUs > 5000) {
        h.ok = false; h.reason = "Mutex contention (DMX blocked on Art-Net)";    return h;
    }

    // Sender swap within the last 2 s = controllers fighting. Bad for cues.
    if (statsArtnetLastSwapMs != 0 &&
        (millis() - statsArtnetLastSwapMs) < 2000) {
        h.ok = false; h.reason = "Multiple Art-Net senders";                     return h;
    }

    // Stale Art-Net (no packets recently) when something was flowing before.
    if (statsArtnetPackets > 0 &&
        (millis() - statsArtnetLastMs) > DMX_LINK_STALE_MS) {
        h.ok = false; h.reason = "Art-Net link stale (holding last frame)";      return h;
    }

    return h;
}
