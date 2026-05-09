# Lumox Firmware — ESP32 DMX Controller

## Overview

ESP32-based wireless DMX node. Receives lighting protocol (Art-Net or sACN E1.31) over WiFi **or** Wiznet W5500 Ethernet, outputs DMX512 via UART + MAX485. Each ESP32 = one universe.

**Debug counterpart**: `../lumox-dmx-monitor/` — separate tiny ESP32 firmware that receives DMX on the same MAX3485 hardware (DE held LOW), prints frames to Serial. Used to verify DMX is actually on the line.

## Architecture

Singleton class `Lumox` — methods split thematically across `.cpp` files. Ingress protocol is pluggable via the `IDmxSource` interface — exactly one source is active at a time, picked by `cfgProtocol` and instantiated via `createDmxSource()`.

```
Lumox::instance()
├── begin() / loop()         → Lumox.cpp        (init, FreeRTOS DMX + protocol task, tick loop)
├── beginNetwork()           → network.cpp      (WiFi STA/AP, mDNS, NVS config)
├── beginEthernet()          → network.cpp      (W5500 via ETH.h, lwIP, event-driven)
├── beginProtocol()          → Lumox.cpp        (factory.createDmxSource → source.begin)
├── feedDmxFrame()           → Lumox.cpp        (single-sender lock, swap detect, mutex write)
├── beginDmx() / writeDmx()  → dmx.cpp          (esp_dmx, mutex-guarded)
├── snapshotDmx()            → dmx.cpp          (atomic read for web UI)
└── beginWebServer()         → webserver.cpp    (async HTTP, WS, DNS, OTA)

IDmxSource (interface)
├── ArtNetSource             → protocols/artnet.cpp  (UDP 6454, full Art-Net 4 node)
├── E131Source               → protocols/e131.cpp    (UDP 5568, sACN multicast + unicast)
└── ApiSource                → protocols/api.cpp     (stub — reserved for future direct API)
```

Interface priority: **AP fallback > Ethernet > WiFi STA**. WiFi + ETH share the lwIP stack — AsyncWebServer + the source's UDP socket work on both interfaces from a single bind. lwIP route table picks the egress netif per destination.

## File Layout

```
lumox-firmware/
├── platformio.ini
├── package.json                        ← npm run build → cdata.js (single source of truth for FW version)
│
├── include/
│   ├── Lumox.h                         ← singleton, all declarations
│   ├── config.h                        ← defaults: SSID, universe, ethernet, timeouts
│   ├── const.h                         ← hardware pins: DMX, LED, W5500 SPI
│   ├── version.h                       ← [auto-generated] FW_VERSION_* from package.json
│   ├── html.h                          ← includes auto-generated gzip headers
│   ├── html_*.h                        ← [auto-generated] dashboard / config / health / control
│   └── protocols/
│       ├── IDmxSource.h                ← interface + ProtocolType enum + factory decl
│       ├── ArtNetSource.h
│       ├── E131Source.h
│       └── ApiSource.h
│
├── src/
│   ├── main.cpp                        ← setup() / loop()
│   ├── Lumox.cpp                       ← begin, loop, feedDmxFrame, FreeRTOS _dmxTask + _protocolTaskWrap
│   ├── network.cpp                     ← WiFi + W5500 ethernet + mDNS + NVS
│   ├── dmx.cpp                         ← DMX512 via esp_dmx, mutex, health stats
│   ├── webserver.cpp                   ← AsyncWebServer routes, WebSocket, DNS, OTA
│   └── protocols/
│       ├── factory.cpp                 ← createDmxSource() + protocolTypeName/FromString
│       ├── artnet.cpp                  ← ArtDMX/Poll/Sync/Address/Command parser + reply
│       ├── e131.cpp                    ← sACN ACN/DMP parser, IGMP join, priority arbitration
│       └── api.cpp                     ← stub
│
├── data/
│   ├── index.html                      ← dashboard
│   ├── config.html                     ← config (WiFi, ethernet, protocol, universe)
│   ├── health.html                     ← health monitor (DMX/source live stats)
│   └── control.html                    ← manual override (per-channel sliders)
│
├── tools/cdata.js
└── pio-scripts/build_ui.py
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

| Parameter | NVS key | Default | Editable via |
|-----------|---------|---------|--------------|
| WiFi STA SSID/PW | `staSsid` / `staPass` | (config-default) | `/config` |
| AP SSID / PW | `apSsid` / `apPass` | `Lumox` / `lumox123` | `/config` |
| Universe | `universe` | 0 | `/config` + ArtAddress |
| Device Name | `devName` | `Lumox-XXYY` | `/config` + ArtAddress (ShortName) |
| **Protocol** | `proto` | 0 = Art-Net | `/config` (reboot on change) |
| ETH DHCP | `ethDhcp` | true | `/config` |
| ETH Static IP/GW/SUB/DNS | `ethIp`/`ethGw`/`ethSub`/`ethDns` | `2.0.0.10 / 2.0.0.1 / /8 / 8.8.8.8` | `/config` |
| WiFi off when ETH up | `wifiOffEth` | false | `/config` |

Manual override (`/control`) is **session-only** — always OFF at boot.

## Source-protocol Architecture

Lumox supports multiple ingress protocols through `IDmxSource`:

- `ProtocolType::ArtNet` (default) — UDP 6454, Art-Net 4 node (Poll/Sync/Address/Command/DMX).
- `ProtocolType::E131` — sACN E1.31 on UDP 5568, multicast 239.255.U.U + unicast.
- `ProtocolType::Api` — stub, reserved for future direct WebSocket API.

Switch via `/config` → reboot. Only one source is ever live; sockets/multicast joins are owned by the active source. Sources call `Lumox::feedDmxFrame()` after their own validation — that's where the universal **single-sender lock** + sender-swap detection live, so every protocol benefits.

Protocol-specific extras (e.g. ArtSync shadow, E1.31 priority arbitration) are encapsulated inside the respective source.

## Art-Net Protocol Support (ArtNetSource)

Implemented OpCodes:

| OpCode | Name | Function |
|---|---|---|
| `0x2000` | OpPoll       | discovery — triggers ArtPollReply, records `TalkToMe` + `DiagPriority` |
| `0x2100` | OpPollReply  | reply (239 B, Art-Net 4 layout) — IP, name, universe, MAC, status, GoodOutput, NodeReport |
| `0x2300` | OpDiagData   | node→controller diagnostic text (priority 0x10–0xE0, filtered by DiagPriority) |
| `0x2400` | OpCommand    | ASCII: `Clear`, `Reboot`, `Identify`, `ResetStats` |
| `0x5000` | OpDmx        | DMX data → `Lumox::feedDmxFrame` (or shadow buffer in sync mode) |
| `0x5200` | OpSync       | shadow flush via `feedDmxFrame` — multi-node simultaneous release |
| `0x6000` | OpAddress    | remote config: universe (Net/Sub/SwOut), ShortName, Identify/ClearOp0/ResetFlags |

ProtVer check: packets with ProtVer < 14 are dropped (counted as `badProto` in source `httpStatus()`).

### ArtSync Mode (Spec §10)

`ArtNetSource` tracks a 4 s sync window. While active, ArtDMX writes go to its private `_shadow[513]`, not directly to `dmxBuffer`. OpSync calls `Lumox::feedDmxFrame(shadow, len, sender)` which flips it through the unified mutex-guarded ingestion path — so the sender lock + swap detection still apply.

Timeout: no OpSync for 4 s → sync mode off, ArtDMX flows direct.

## sACN E1.31 Protocol Support (E131Source)

Listens on UDP 5568. On `begin()` and on every `onNetworkChange()`, joins the multicast group `239.255.<U high>.<U low>` for the configured universe on every active netif (ETH, STA, AP). Unicast on the same socket also accepted (gateways that direct-send).

Per-frame validation:
- ACN packet identifier `ASC-E1.17` at offset 4
- Root vector `0x00000004`, frame vector `0x00000002`, DMP vector `0x02`
- Universe filter against `cfgUniverse`
- Stream-Terminated option flag — releases priority hold, drops frame
- Start code at DMP-data must be `0x00` (null DMX, not RDM/proprietary)

**Priority arbitration**: byte 108 (0..200, default 100). Within `DMX_LINK_STALE_MS`, lower-priority frames are rejected (`lowerPrio` stat). After the stale window, any priority can take over.

## Discovery

### Art-Net
- Single UDP 6454 socket bound to lwIP — receives on WiFi **and** ETH netifs simultaneously
- ArtPollReply contains the IP of the active interface (ETH preferred over STA)
- Periodic unsolicited broadcast every 10 s + on every network change (`announceProtocolNode`)
- Each netif's directed broadcast is hit explicitly (limited broadcast goes only out the default netif)

### E1.31
- Multicast IGMP join per active netif (ETH/STA/AP)
- Re-join on `onNetworkChange()` so interface changes don't silently drop group membership

### mDNS
- Service `_lumox._tcp` on port 80
- TXT records: `name`, `universe`, `mac`
- Hostname: `lumox-xxyy.local`

## Web Interface (Async)

`ESPAsyncWebServer` runs on its own AsyncTCP task — HTTP requests + OTA uploads no longer block packet pickup.

| Route | Description |
|-------|-------------|
| `/` | dashboard |
| `/config` | configuration: WiFi, ethernet, **protocol**, universe, device name |
| `/health` | DMX + source live health (frame rate, errors, sender swaps, protocol-specific) |
| `/control` | per-channel manual override (512 sliders) |
| `/update` | ElegantOTA firmware upload |
| `/api/status` | JSON: network, source, DMX state, health |
| `/api/dmx` | JSON: 512 DMX values (mutex snapshot) |
| `/api/config` | GET/POST config (`protocol` field accepts `"artnet"`/`"e131"`/`"api"`) |
| `/api/manual` | GET (state) / POST (`{ch, val}` or `{enabled}`) |
| `/api/manual/clear` | POST: release all overrides |
| `/api/reboot` | POST |
| `/api/reset` | POST: factory reset |
| `/generate_204`, `/hotspot-detect.html`, `/ncsi.txt` … | captive-portal detection |
| `ws://ip/ws` | WebSocket: JSON status + binary DMX push (AsyncWebSocket on port 80) |

Status JSON: top-level `proto` field exposes the active protocol short name. Source-specific stats are merged into the `artnet` block via `IDmxSource::httpStatus()` — name is historical, the block content is protocol-neutral plus extras.

## Manual Override (control.html)

`Lumox::manualValue[1..512]` — `int16_t` per channel:
- `-1` = AUTO (source wins)
- `0..255` = forced value

`manualEnabled` is the master switch (session-only, defaults OFF). `_dmxTask` mixes in the TX path: if `manualEnabled && manualValue[i] >= 0` → manual value, else `dmxBuffer[i]`.

## Platform / Libraries (platformio.ini)

**Platform**: `pioarduino/platform-espressif32` v53.03.13 — community fork providing arduino-esp32 v3.x. Required for W5500 support in `ETH.h` (added in v3.0).

| Library | Purpose |
|---------|---------|
| someweisguy/esp_dmx ^4.1 | DMX512 output |
| bblanchon/ArduinoJson ^7.0 | JSON serialization |
| ayushsharma82/ElegantOTA ^3.1 | OTA firmware update (async) |
| esp32async/AsyncTCP ^3.3.5 | async TCP for ESPAsyncWebServer |
| esp32async/ESPAsyncWebServer ^3.7.10 | non-blocking HTTP server + AsyncWebSocket |

W5500 driver is built into arduino-esp32 v3.x (`ETH.begin(ETH_PHY_W5500, ...)`) — no external library needed.

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

`package.json` `version` field is the **single source of truth**. SemVer `MAJOR.MINOR.PATCH`. `tools/cdata.js` derives `include/version.h` on every build. `FW_VERSION_MAJOR/MINOR` are also embedded in every ArtPollReply (bytes 16-17).

| Bump | When |
|---|---|
| **PATCH** | Default for every change — bug fixes, refactors, new features, new web routes, NVS additions. |
| **MINOR** | A coherent batch of features ships together (release milestone) or behaviour change worth flagging via the ArtPollReply minor byte. |
| **MAJOR** | Breaking change: NVS layout wipe, pin remap, protocol-incompatible Art-Net change, removal of an `/api/*` route. |

### How to bump

1. Edit `package.json` → increment `"version"`.
2. `pio run` regenerates `version.h` + UI HTML headers (PIO pre-build hook).
3. Commit `package.json` together with the code change. **Do not** commit `include/version.h` or `include/html_*.h`.
4. If NVS layout changed (new field, renamed key, removed key): note the migration in the commit message. Removed/repurposed keys → MAJOR.

## Flicker Protection / Hardening

DMX output is structurally robust against packet loss and corruption:

- **Persistent buffer**: `dmxBuffer` keeps last values until a new packet arrives
- **DMX task retransmits** continuously (~44 Hz) regardless of network state
- **Source-side validation**: each protocol checks header + length + ProtVer/Vector → invalid packets dropped
- **UDP checksum** filters corrupt packets at the OS level
- **Partial update**: packets with < 512 channels overwrite only the sent bytes
- **portMUX (`_dmxLock`)** guards concurrent access from the source + DMX task

**Hardening** (in `Lumox::feedDmxFrame` + per-source):
- **Per-sender sequence check** (source-side, protocol-specific encoding) — out-of-order/duplicate dropped (`statsArtnetStale`)
- **Single-sender lock** (`feedDmxFrame`): while primary is live (< `DMX_LINK_STALE_MS`), other IPs are rejected (`statsArtnetLocked`). Stale → next sender takes over
- **Sender-swap detection** (`feedDmxFrame`): primary flipping mid-show → `statsArtnetSenderSwaps++`, `statsArtnetLastSwapMs` (visible on `/health`)
- **Stale-link LED**: steady on while receiving, slow pulse after > 2 s without DMX
- **Queue drain**: source `service()` drains all pending UDP packets per tick (no backlog under WiFi bursts)
- **DMX-TX health**: `statsDmxFramesSent`, `statsDmxSendErrors`, `statsDmxConsecErrors`, `statsDmxMaxConsecErr`, `statsDmxRateHz`, `statsDmxMaxMutexUs` — `dmxHealth()` returns bool + reason

The `statsArtnet*` field names are historical — they apply to whichever source is active.

## Typical Boot Sequence

1. `loadConfig()` — read NVS (defaults on first boot, includes `proto` key)
2. `beginEthernet()` — W5500 via ETH.h, register `_onEthEvent`, optional static config, brief link wait
3. `beginNetwork()` — try STA (timeout shortened if ETH up) → on failure AP fallback + captive portal
4. `beginWebServer()` — async HTTP + WS + DNS + OTA on lwIP (reachable on both netifs)
5. `beginProtocol()` — factory creates source per `cfgProtocol`, source binds its socket(s)
6. `beginDmx()` — install esp_dmx driver, create mutex
7. `_dmxTask` on core 1 — DMX continuously at ~44 Hz
8. `_protocolTaskWrap` on core 1 — drains source RX queue + housekeeping
9. `loop()` on core 0:
   - `loopEthernet()` (no-op shim — ETH state runs on `_onEthEvent`)
   - `loopWebServer()` (DNS tick, WS push @ 5 Hz)
   - `_updateLed()` (status-LED pattern)
