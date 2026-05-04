# Lumox Firmware — ESP32 DMX Controller

## Overview

ESP32-based wireless DMX node. Receives Art-Net over WiFi **or** Wiznet W5500 Ethernet, outputs DMX512 via UART + MAX485. Each ESP32 = one universe.

**Debug counterpart**: `../lumox-dmx-monitor/` — separate tiny ESP32 firmware that receives DMX on the same MAX3485 hardware (DE held LOW), prints frames to Serial. Used to verify DMX is actually on the line.

## Architecture

Singleton class `Lumox` — methods split thematically across `.cpp` files (pattern from DartTool/DartLaser projects).

```
Lumox::instance()
├── begin() / loop()       → Lumox.cpp     (init, FreeRTOS DMX task, tick loop)
├── beginNetwork()         → network.cpp   (WiFi STA/AP, mDNS, NVS config)
├── beginEthernet()        → network.cpp   (W5500 via ETH.h, lwIP, event-driven)
├── pollArtNet()           → artnet.cpp    (single lwIP socket, both netifs)
├── sendArtPollReply()     → artnet.cpp    (per-netif directed broadcast)
├── beginDmx() / writeDmx()→ dmx.cpp       (esp_dmx, mutex-guarded)
├── snapshotDmx()          → dmx.cpp       (atomic read for web UI)
└── beginWebServer()       → webserver.cpp (async HTTP, WS, DNS, OTA)
```

Interface priority: **AP fallback > Ethernet > WiFi STA**. WiFi + ETH share the lwIP stack — AsyncWebServer + Art-Net UDP work on both interfaces from a single socket. lwIP route table picks the egress netif per destination.

## File Layout

```
lumox-firmware/
├── platformio.ini                ← board, libraries, pre-build hooks
├── package.json                  ← npm run build → cdata.js (version 0.3.0)
│
├── include/
│   ├── Lumox.h                   ← singleton, all declarations
│   ├── config.h                  ← defaults: SSID, universe, ethernet, timeouts
│   ├── const.h                   ← hardware pins: DMX, LED, W5500 SPI
│   ├── version.h                 ← [auto-generated] FW_VERSION_* from package.json
│   ├── html.h                    ← includes auto-generated gzip headers
│   ├── html_index.h              ← [auto-generated] dashboard
│   ├── html_config.h             ← [auto-generated] config page
│   ├── html_health.h             ← [auto-generated] health page
│   └── html_control.h            ← [auto-generated] manual-control page
│
├── src/
│   ├── main.cpp                  ← setup() / loop()
│   ├── Lumox.cpp                 ← begin, loop, FreeRTOS _dmxTask, _updateLed
│   ├── network.cpp               ← WiFi + W5500 ethernet + mDNS + NVS
│   ├── artnet.cpp                ← ArtDMX/Poll/Sync/Address/Command parser
│   ├── dmx.cpp                   ← DMX512 via esp_dmx, mutex, health stats
│   └── webserver.cpp             ← AsyncWebServer routes, WebSocket, DNS, OTA
│
├── data/
│   ├── index.html                ← dashboard
│   ├── config.html               ← config page (WiFi, ethernet, universe)
│   ├── health.html               ← health monitor (DMX/Art-Net live stats)
│   └── control.html              ← manual override (per-channel sliders)
│
├── tools/cdata.js                ← Node.js: data/*.html → include/html_*.h (gzip) + version.h
└── pio-scripts/
    └── build_ui.py               ← PIO pre-build: calls npm run build
```

## Hardware Pins (const.h)

### DMX (MAX485)
| Pin | Function | MAX485 |
|-----|----------|--------|
| GPIO 17 | DMX TX  | DI |
| GPIO 16 | DMX RX  | RO (unused) |
| GPIO 4  | DE+RE   | DE + RE (tied) |
| GPIO 2  | LED     | onboard |

### W5500 Ethernet (VSPI / SPI3_HOST)
| Pin | Function |
|-----|----------|
| GPIO 5  | CS   (SCS) |
| GPIO 18 | SCK  (SCLK) |
| GPIO 19 | MISO |
| GPIO 23 | MOSI |
| GPIO 34 | INT  (input-only) |
| GPIO 33 | RST  (low-active) |

UART1 is used for DMX (UART2 crashes in `esp_dmx` 4.1 with Arduino-ESP32 core 2.x).

## Configuration

Runtime config stored in NVS (Preferences). Defaults from `config.h`.

| Parameter | Default | Editable via |
|-----------|---------|--------------|
| WiFi STA SSID/PW | (config-default) | `/config` |
| AP SSID / PW | `Lumox` / `lumox123` | `/config` |
| Universe | 0 | `/config` + ArtAddress |
| Device Name | `Lumox-XXYY` | `/config` + ArtAddress (ShortName) |
| ETH DHCP | true | `/config` |
| ETH Static IP/GW/SUB/DNS | `2.0.0.10 / 2.0.0.1 / /8 / 8.8.8.8` | `/config` |
| WiFi off when ETH up | false | `/config` |

Manual override (`/control`) is **session-only** — always OFF at boot. Lumox comes up cleanly as an Art-Net node every time.

## Art-Net Protocol Support

Implemented OpCodes:

| OpCode | Name | Function |
|---|---|---|
| `0x2000` | OpPoll       | discovery — triggers ArtPollReply, records `TalkToMe` + `DiagPriority` |
| `0x2100` | OpPollReply  | reply (239 B, Art-Net 4 layout) — IP, name, universe, MAC, status, GoodOutput, NodeReport |
| `0x2300` | OpDiagData   | node→controller diagnostic text (priority 0x10–0xE0, filtered by DiagPriority) |
| `0x2400` | OpCommand    | ASCII: `Clear`, `Reboot`, `Identify`, `ResetStats` |
| `0x5000` | OpDmx        | DMX data → `dmxBuffer` (or `_dmxShadow` in sync mode) |
| `0x5200` | OpSync       | shadow → live flip — all nodes release frame synchronously |
| `0x6000` | OpAddress    | remote config: universe (Net/Sub/SwOut), ShortName, Identify/ClearOp0/ResetFlags |

After OpAddress change: `saveConfig()` + automatic ArtPollReply.

ProtVer check: packets with ProtVer < 14 are dropped (`statsArtNetBadProto`).

### ArtSync Mode (Spec §10)

When the node receives OpSync → `statsInSyncMode = true` for 4 s. While active:
- Incoming ArtDMX writes to `_dmxShadow` (not directly to `dmxBuffer`)
- OpSync flips shadow → live atomically (mutex-guarded)
- Multi-universe nodes release frames **simultaneously** → no tearing between Stage-Left/Right

If no OpSync arrives for 4 s → sync mode off, ArtDMX writes directly.

## Discovery

### ArtPoll
- Single UDP 6454 socket bound to lwIP — receives on WiFi **and** ETH netifs simultaneously
- ArtPollReply contains the IP of the active interface (ETH preferred over STA)
- Periodic unsolicited broadcast (`announceArtNetNode`) sends to each netif's directed broadcast (`ETH.localIP() | ~ETH.subnetMask()`, same for WiFi) so both wired + wireless controllers see it

### mDNS
- Service `_lumox._tcp` on port 80
- TXT records: `name`, `universe`, `mac`
- Hostname: `lumox-xxyy.local`

## Web Interface (Async)

`ESPAsyncWebServer` runs on its own AsyncTCP task — HTTP requests + OTA uploads no longer block the Art-Net poll.

| Route | Description |
|-------|-------------|
| `/` | dashboard |
| `/config` | configuration: WiFi, ethernet, universe, device name |
| `/health` | DMX/Art-Net live health (frame rate, errors, sender swaps) |
| `/control` | per-channel manual override (512 sliders) |
| `/update` | ElegantOTA firmware upload |
| `/api/status` | JSON: network, Art-Net, DMX state, health |
| `/api/dmx` | JSON: 512 DMX values (mutex snapshot) |
| `/api/config` | GET/POST config |
| `/api/manual` | GET (state) / POST (`{ch, val}` or `{enabled}`) |
| `/api/manual/clear` | POST: release all overrides |
| `/api/reboot` | POST |
| `/api/reset` | POST: factory reset |
| `/generate_204`, `/hotspot-detect.html`, `/ncsi.txt` … | captive-portal detection |
| `ws://ip/ws` | WebSocket: JSON status + binary DMX push (AsyncWebSocket on port 80) |

WebSocket uses ESPAsyncWebServer's `AsyncWebSocket` mounted on `/ws` — same TCP stack as HTTP, single port (80).

## Manual Override (control.html)

`Lumox::manualValue[1..512]` — `int16_t` per channel:
- `-1` = AUTO (Art-Net wins)
- `0..255` = forced value

`manualEnabled` is the master switch (session-only, defaults OFF). `_dmxTask` mixes in the TX path: if `manualEnabled && manualValue[i] >= 0` → manual value, else `dmxBuffer[i]`.

Atomic int16 reads on ESP32 → no mutex needed. Occasional torn updates would be visually invisible.

## Platform / Libraries (platformio.ini)

**Platform**: `pioarduino/platform-espressif32` v53.03.13 — community fork providing arduino-esp32 v3.x. Required for W5500 support in `ETH.h` (added in v3.0). Stock `espressif32@6.x` ships v2.x, which only supports built-in EMAC PHYs.

| Library | Purpose |
|---------|---------|
| someweisguy/esp_dmx ^4.1 | DMX512 output |
| bblanchon/ArduinoJson ^7.0 | JSON serialization |
| ayushsharma82/ElegantOTA ^3.1 | OTA firmware update (async) |
| esp32async/AsyncTCP ^3.3.5 | async TCP for ESPAsyncWebServer |
| esp32async/ESPAsyncWebServer ^3.7.10 | non-blocking HTTP server + AsyncWebSocket |

W5500 driver is built into arduino-esp32 v3.x (`ETH.begin(ETH_PHY_W5500, ...)`) — no external library needed.

`build_flags`: `ELEGANTOTA_USE_ASYNC_WEBSERVER=1`, `WEBSOCKETS_USE_SSL=0`, `CORE_DEBUG_LEVEL=3`.

## Build

```bash
pio run                    # compile (auto-builds web UI + version.h)
pio run -t upload          # flash
pio device monitor         # serial monitor (115200 baud)
npm run build              # regenerate web UI + version.h only
```

`tools/cdata.js` runs as PIO pre-build hook (`pio-scripts/build_ui.py`) and:
1. gzips `data/*.html` → `include/html_*.h` as PROGMEM byte arrays
2. reads `package.json` version → writes `include/version.h` (FW_VERSION_*, FW_BUILD_DATE)

## Versioning

`package.json` `version` field is the **single source of truth**. SemVer `MAJOR.MINOR.PATCH`. `tools/cdata.js` derives `include/version.h` (`FW_VERSION_MAJOR/MINOR/PATCH`, `FW_VERSION_STRING` with git short hash) on every build. `FW_VERSION_MAJOR/MINOR` are also embedded in every ArtPollReply (bytes 16-17), so controllers see the bump.

### When to bump

**Default: PATCH.** Bump the patch number on every committed change unless the change explicitly meets the MINOR or MAJOR bar below.

| Bump | When |
|---|---|
| **PATCH** (`0.6.0` → `0.6.1`) | Default for every change — bug fixes, refactors, perf work, new features, new web routes, new Art-Net OpCodes, NVS additions. |
| **MINOR** (`0.6.x` → `0.7.0`) | A coherent batch of features ships together (release milestone), or behavior change worth flagging to controllers via the ArtPollReply minor byte. Author's call. |
| **MAJOR** (`0.x.y` → `1.0.0`) | Breaking change: NVS layout wipe, pin remap, protocol-incompatible Art-Net change, removal of an `/api/*` route. |

Don't agonise — just bump PATCH. MINOR is reserved for moments worth marking; MAJOR for breakage.

### How to bump

1. Edit `package.json` → increment `"version"` (usually the patch digit).
2. `npm run build` (or just `pio run` — pre-build hook regenerates `version.h` + UI HTML headers).
3. Commit `package.json` together with the code change. **Do not** commit `include/version.h` or `include/html_*.h` — generated artifacts, regenerated on every build.
4. If NVS layout changed (new field, renamed key, or removed key): note the migration in the commit message. Removed/repurposed keys → MAJOR.

Web UI uses `{{VERSION}}` placeholders, substituted at build time — bumping `package.json` is enough; no separate HTML edit.

## Flicker Protection / Hardening

DMX output is structurally robust against packet loss and corruption:

- **Persistent buffer**: `dmxBuffer` keeps last values until a new packet arrives
- **DMX task retransmits** continuously (~44 Hz) regardless of network state
- **`_parseArtNet` validates header + length + ProtVer** — invalid packets dropped
- **UDP checksum** filters corrupt packets at the OS level
- **Partial update**: packets with < 512 channels overwrite only the sent bytes
- **Mutex (`_dmxMutex`)** guards concurrent access from parser + task

**Hardening** (`artnet.cpp` + `Lumox.cpp`):
- **Per-sender sequence check**: byte 12 inspected, tracked via `_lastSeqSender` → out-of-order/duplicate dropped (`statsArtnetStale`)
- **Single-sender lock**: while primary is live (< `DMX_LINK_STALE_MS`), other IPs are rejected (`statsArtnetLocked`). Stale → next sender takes over
- **Sender-swap detection**: primary flipping mid-show → `statsArtnetSenderSwaps++`, `statsArtnetLastSwapMs` (visible on `/health`)
- **Stale-link LED**: steady on while receiving, slow pulse after > 2 s without ArtDMX
- **Queue drain**: `loop()` drains all pending UDP packets per tick (no backlog under WiFi bursts)
- **DMX-TX health**: `statsDmxFramesSent`, `statsDmxSendErrors`, `statsDmxConsecErrors`, `statsDmxMaxConsecErr`, `statsDmxRateHz`, `statsDmxMaxMutexUs` — `dmxHealth()` returns bool + reason

## Typical Boot Sequence

1. `loadConfig()` — read NVS (defaults on first boot)
2. `beginEthernet()` — W5500 via ETH.h, register `_onEthEvent`, optional static config, brief link wait
3. `beginNetwork()` — try STA (timeout shortened if ETH up) → on failure AP fallback + captive portal
4. `beginWebServer()` — async HTTP + WS + DNS + OTA on lwIP (reachable on both netifs)
5. `beginArtNet()` — open UDP 6454 (single lwIP socket → both netifs)
6. `beginDmx()` — install esp_dmx driver, create mutex
7. `_dmxTask` on core 1 — DMX continuously at ~44 Hz
8. `loop()` on core 0:
   - `pollArtNet()` (queue drain — both netifs)
   - `loopEthernet()` (no-op shim — ETH state runs on `_onEthEvent`)
   - `loopArtNet()` (periodic ArtPollReply, sync timeout)
   - `loopWebServer()` (DNS tick, WS push @ 5 Hz)
   - `_updateLed()` (status-LED pattern)
