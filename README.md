<div align="center">

# Lumox Firmware

**Wireless DMX node for ESP32 — Art-Net over WiFi or Ethernet → DMX512**

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange?logo=platformio)](https://platformio.org/)
[![Framework](https://img.shields.io/badge/Framework-Arduino--ESP32-00979D?logo=arduino)](https://github.com/espressif/arduino-esp32)
[![Art-Net](https://img.shields.io/badge/Art--Net-4-blueviolet)](https://art-net.org.uk/)
[![Version](https://img.shields.io/badge/version-0.6.11-success)](package.json)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](#license)

[Overview](#overview) ·
[Features](#features) ·
[Hardware](#hardware) ·
[Quickstart](#quickstart) ·
[Web UI](#web-interface) ·
[Art-Net](#art-net) ·
[FAQ](#faq)

</div>

---

## Overview

Lumox is a wireless DMX512 receiver based on the ESP32. It accepts Art-Net over
WiFi **or** Wiznet W5500 ethernet and emits the universe as DMX512 through a
MAX485 transceiver on a 5-pin XLR line.

Each ESP32 hosts **one** universe. Multiple nodes can be synchronized via
Art-Net 4 (`OpSync`) so Stage-Left, Stage-Right and Truss release frames at the
exact same instant — no visible tearing.

---

## Features

- **Dual interface, single stack** — WiFi (STA + AP fallback) **and** Wiznet W5500 ethernet on a unified lwIP TCP stack. AsyncWebServer + Art-Net UDP reach both interfaces from a single socket. Ethernet is preferred once link + DHCP are up.
- **AP-aux mode** — when STA is down but ETH is up, the access point still opens so a phone can reach `/config` without unplugging the cable. ETH continues to carry the Art-Net data path.
- **Art-Net 4** — `OpPoll`, `OpDmx`, `OpSync`, `OpAddress`, `OpCommand`, `OpDiagData`
- **mDNS discovery** — `_lumox._tcp` service, hostname `lumox-xxyy.local` (MAC-derived, always unique)
- **Captive portal** — automatic redirect in AP mode for fast first-time setup
- **Async web UI** — dashboard, config, health monitor, manual override (all gzipped in PROGMEM). Interface bar at the bottom of every page indicates which link the browser used: blue = ETHERNET, green = WIFI (STA), orange = WIFI (AP).
- **OTA updates** — browser upload via [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) at `/update`
- **WebSocket live push** — status + DMX slots @ ~5 Hz for live monitoring
- **Flicker hardening** — per-sender sequence check, single-sender lock, sender-swap detection, dedicated Art-Net RX FreeRTOS task (decoupled from HTTP / mDNS / DNS), portMUX-guarded DMX buffer (no priority-inversion stalls)
- **ArtSync mode** — shadow buffer + atomic flip for synchronous multi-node frames
- **Slot-1 pin workaround** — optional runtime toggle (`/config`) forces DMX channel 1 to value 1 on the wire, dodging cheap moving-head receivers that misdetect the start-code + slot-1 zero run as a fresh BREAK
- **Silent production builds** — `[env:esp32dev]` compiles all logging out and never opens the UART; `[env:debug]` keeps the full firehose

---

## Hardware

### DMX wiring (UART1 → MAX485 → XLR-5)

```
┌──────────────┐               ┌────────────┐                  ┌──────────────┐
│    ESP32     │               │   MAX485   │                  │  XLR 5-pin F │
├──────────────┤               ├────────────┤                  ├──────────────┤
│ GPIO 17  TX1 ├──────────────▶│ DI         │                  │ Pin 1  GND   │
│ GPIO 16  RX1 │◀──────────────┤ RO  (n/c)  │                  │ Pin 2  Data− │
│ GPIO  4   DE ├───────┬──────▶│ DE         │     A  ──────────┤ Pin 3  Data+ │
│              │       └──────▶│ RE         │     B  ──────────┤ Pin 4  n/c   │
│       3.3V   ├──────────────▶│ VCC        │    GND ──────────┤ Pin 5  n/c   │
│       GND    ├──────────────▶│ GND        │                  │              │
└──────────────┘               └────────────┘                  └──────────────┘
```

DMX-512 (ANSI E1.11) pin assignment: **1 = signal ground**, **2 = Data−**, **3 = Data+**, **4/5 = optional second link (leave open on a single-link node)**. MAX485 pin **A is non-inverting** → wires to XLR 3, **B is inverting** → wires to XLR 2.

> **Tips**
> - Add a 120 Ω termination resistor between A and B at the *last* fixture in the chain.
> - MAX485 also runs on 3.3 V `VCC`, but the line is more robust at 5 V — feed the chip 5 V and use a level shifter on `DI`, or fit a MAX3485 / ISO3088 in the 3.3 V variant directly. The companion [`lumox-dmx-monitor`](../lumox-dmx-monitor/) sketch on the same hardware (DE held LOW) is the easiest way to verify what's actually on the wire.

### Ethernet wiring (W5500, optional — VSPI / SPI3_HOST)

```
┌──────────────┐               ┌────────────┐
│    ESP32     │               │   W5500    │
├──────────────┤               ├────────────┤
│ GPIO  5   CS ├──────────────▶│ SCS        │
│ GPIO 18  SCK ├──────────────▶│ SCLK       │
│ GPIO 19 MISO │◀──────────────┤ MISO       │
│ GPIO 23 MOSI ├──────────────▶│ MOSI       │
│ GPIO 34  INT │◀──────────────┤ INT        │ (input-only — see ETH_USE_IRQ)
│ GPIO 33  RST ├──────────────▶│ RST        │
│       3.3V   ├──────────────▶│ VCC        │
│       GND    ├──────────────▶│ GND        │
└──────────────┘               └────────────┘
```

| ESP32 GPIO | W5500 | Notes |
|:-:|:-:|:-|
| 5  | CS   | SPI chip-select, low-active |
| 18 | SCK  | SPI clock (20 MHz) |
| 19 | MISO | SPI master-in |
| 23 | MOSI | SPI master-out |
| 34 | INT  | Input-only — needs an external 10 kΩ pull-up if you set `ETH_USE_IRQ=1`; default is polling mode (`ETH_USE_IRQ=0`) which is bulletproof |
| 33 | RST  | Hardware reset, low-active |

No W5500 connected? → `ETH.begin()` simply fails, firmware continues on WiFi.

### Customizing pin assignment

→ [`include/const.h`](include/const.h)

---

## Quickstart

### 1. Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- [Node.js](https://nodejs.org/) ≥ 18 (for the web-UI build)
- ESP32 dev board + MAX485 + XLR socket

### 2. Flash

```bash
git clone https://github.com/itCarl/LumoX-firmware.git
cd LumoX-firmware
pio run -t upload          # builds web UI, compiles, flashes
pio device monitor         # 115200 baud
```

The pre-build hook automatically calls `npm run build` (see [`tools/cdata.js`](tools/cdata.js))
and gzip-compresses `data/*.html` into PROGMEM headers.

### 3. First-time setup

1. ESP32 starts in **AP mode** with SSID `Lumox-XXYY` (default password: `lumox123`)
2. Connect a phone/laptop → captive portal opens automatically (or hit `http://192.168.4.1` manually)
3. On `/config`:
   - Enter **STA WiFi** SSID/password → `Save & Reboot`
   - Optional: adjust **universe** + **device name**
   - Optional: switch **ethernet** to static (DHCP by default)
4. After reboot the node joins the hotspot and is discoverable in the Lumox controller (or QLC+, MagicQ, Resolume, …)

---

## Web Interface

| Route | Content |
|---|---|
| `/` | Dashboard — network, Art-Net, DMX live slots |
| `/config` | WiFi · ethernet · universe · device name |
| `/health` | DMX/Art-Net live stats (frame rate, errors, sender swaps) |
| `/control` | Manual override — 512 channel sliders (session-only) |
| `/update` | ElegantOTA firmware upload |

Plus REST API: `/api/status`, `/api/dmx`, `/api/config`, `/api/manual`, `/api/manual/clear`, `/api/reboot`, `/api/reset`,
and WebSocket on `ws://<ip>/ws` (JSON status + binary DMX).

---

## Art-Net

### Discovery

ArtPoll on UDP 6454 (broadcast) → ArtPollReply with IP, MAC, universe, device name, firmware version. Periodic unsolicited re-announcements ensure recovery without a fresh discovery round.

### Synchronization (ArtSync, Spec §10)

```
Controller ──▶ ArtDMX (universe 0) ──▶ Node Left   ┐
           ──▶ ArtDMX (universe 1) ──▶ Node Right  │ all frames
           ──▶ ArtDMX (universe 2) ──▶ Node Truss  │ in shadow buffer
           ──▶ OpSync ──────────────▶ all nodes    ┘ ───▶ atomic flip
```

Frames are buffered in the shadow; OpSync flips all nodes simultaneously. Result: no tearing between universes on large rigs.

### Supported OpCodes

| OpCode | Name | Function |
|---|---|---|
| `0x2000` | OpPoll | discovery |
| `0x2100` | OpPollReply | reply (239 B Art-Net 4 layout) |
| `0x2300` | OpDiagData | node→controller diagnostic text |
| `0x2400` | OpCommand | `Clear`, `Reboot`, `Identify`, `ResetStats` |
| `0x5000` | OpDmx | DMX data |
| `0x5200` | OpSync | synchronous frame flip |
| `0x6000` | OpAddress | remote config (universe, ShortName, identify) |

---

## Flicker Protection

DMX output is hardened against typical failure modes:

- **Persistent buffer** — last valid values keep streaming (~44 Hz), regardless of network state
- **Per-sender sequence check** — out-of-order packets dropped (`statsArtnetStale`); duplicates are accepted, since several controllers re-emit static frames with an unchanging sequence byte
- **Single-sender lock** — while the primary is live (`DMX_LINK_STALE_MS = 2 s`), other IPs are rejected (`statsArtnetLocked`); primary stale → next sender takes over
- **Sender-swap detection** — controller flips mid-show are counted + visible on `/health`
- **portMUX-guarded DMX buffer** — spinlock instead of FreeRTOS semaphore; preemption disabled inside the critical section eliminates priority-inversion stalls (≥5 ms under WiFi RX bursts)
- **Dedicated Art-Net RX task** — UDP drain runs on its own FreeRTOS task (Core 1, prio 4), decoupled from HTTP / mDNS / DNS, so web traffic can't delay packet pickup
- **Slot-1 zero-run workaround** — optional `cfgCh1PinNonzero` forces slot 1 to `0x01` on the wire (see FAQ)
- **Stale-link LED** — steady on while receiving, slow pulse after 2 s of silence
- **DMX TX health** — `framesSent`, `sendErr`, `waitTO`, `consecErr`, `rateHz`, `maxMutexUs` + 60 s rolling timeline of mutex-contention spikes on `/health`

→ Details in [`CLAUDE.md`](CLAUDE.md#flicker-protection--hardening)

---

## Software Layout

```
lumox-firmware/
├── platformio.ini             ← board, libraries, pre-build hooks
├── package.json               ← web-UI build (cdata.js)
├── include/
│   ├── Lumox.h                ← singleton — all declarations
│   ├── config.h               ← defaults (NVS-overridable)
│   ├── const.h                ← hardware pins
│   └── version.h              ← [auto] from package.json
├── src/
│   ├── main.cpp               ← setup() / loop()
│   ├── Lumox.cpp              ← begin, loop, FreeRTOS DMX task
│   ├── network.cpp            ← WiFi + W5500 + mDNS + NVS
│   ├── artnet.cpp             ← Art-Net parser + sender
│   ├── dmx.cpp                ← esp_dmx wrapper, health stats
│   └── webserver.cpp          ← AsyncWebServer + WS + OTA + DNS
├── data/                      ← web UI (HTML, gzip-compressed at build time)
├── tools/cdata.js             ← web UI → PROGMEM headers
└── pio-scripts/
    └── build_ui.py            ← pre-build: calls npm run build
```

Architecture pattern: central singleton class `Lumox`, methods split thematically across `.cpp` files.

---

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [someweisguy/esp_dmx](https://github.com/someweisguy/esp_dmx) | ^4.1 | DMX512 output |
| [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) | ^7.0 | JSON serialization |
| [ayushsharma82/ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) | ^3.1 | OTA firmware update |
| [esp32async/AsyncTCP](https://github.com/ESP32Async/AsyncTCP) | ^3.3.5 | async TCP stack |
| [esp32async/ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | ^3.7.10 | async HTTP server + AsyncWebSocket |

W5500 driver is built into arduino-esp32 v3.x via `ETH.h` (`ETH.begin(ETH_PHY_W5500, ...)`). Platform pinned to [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32) v53.03.13 since stock `espressif32@6.x` ships arduino-esp32 v2.x (no SPI ETH support).

---

## FAQ

<details>
<summary><strong>WiFi outage — will the lights start flickering?</strong></summary>

No. The DMX-TX task keeps streaming the last valid buffer. On packet loss the
current frame holds until new ArtDMX packets arrive. Only after 2 s of silence
does the LED start to pulse slowly — the lights themselves hold their state.
</details>

<details>
<summary><strong>Can I use WiFi and ethernet simultaneously?</strong></summary>

Yes. Both interfaces are active in parallel. When ethernet link is up, ArtPoll
replies prefer ethernet (more reliable). Optionally WiFi can be turned off
automatically as soon as ETH is up (`/config` → "Disable WiFi when ETH up").
</details>

<details>
<summary><strong>Multiple senders at the same time — what happens?</strong></summary>

Single-sender lock: as long as the primary sender keeps delivering packets
within `DMX_LINK_STALE_MS` (2 s), other IPs are rejected (`statsArtnetLocked`).
If the primary goes stale, the next sender takes over automatically. The number
of takeovers is visible on `/health` (`Sender Swaps`).
</details>

<details>
<summary><strong>Why UART1 instead of UART2?</strong></summary>

`esp_dmx` 4.1 has crashed on UART2 across several arduino-esp32 core versions. UART1 has been stable in every combination tested. The `DMX_UART_PORT` constant in `include/const.h` lets you change it if you need to free GPIO 16/17 — at your own risk.
</details>

<details>
<summary><strong>My moving heads jitter when DMX channel 1 = 0. What gives?</strong></summary>

Some cheap moving-head receivers misdetect the start of frame when slot 1 is `0x00`. The DMX start code is also `0x00`, so the line stays low long enough that a weak break-detector falsely resyncs mid-frame — visible as jitter on slots far beyond ch 1, even though those slots are transmitted correctly. Open `/config` → **DMX Output** → tick **Pin Ch 1 ≥ 1**, save & reboot. Slot 1 will then always carry `0x01` on the wire, breaking the run of zero bytes. Only enable when channel 1 is unused (fixtures patched ≥ 2). Compile-time `LUMOX_CH1_PIN_NONZERO=0` removes the feature entirely.
</details>

<details>
<summary><strong>How do I check whether DMX is actually on the wire?</strong></summary>

Flash the companion [`lumox-dmx-monitor`](../lumox-dmx-monitor/) sketch onto a spare ESP32 + MAX3485 board (same hardware, DE held LOW = permanent RX). It prints start-code + first 32 channels on the serial monitor at 115200 baud, ~2 ×/s, and reports "no signal" after 2 s of silence. Lumox's `/health` page also surfaces TX-side counters: `framesSent`, `sendErr`, `waitTO`, `consecErr`, `rateHz`, `maxMutexUs`, sender swaps, and a 60 s rolling timeline of mutex-contention spikes.
</details>

<details>
<summary><strong>Manual override on `/control` — does it survive a reboot?</strong></summary>

No. Both the master switch (`manualEnabled`) and the per-channel forced values are session-only — Lumox always boots as a clean Art-Net node. The override is intended for live debugging or a quick stage-side rescue, not as a persistent patch.
</details>

<details>
<summary><strong>Rebuild the web UI after code changes?</strong></summary>

```bash
npm run build               # gzip → include/html_*.h, writes version.h
```

Happens automatically on `pio run` via `pio-scripts/build_ui.py`.
</details>

<details>
<summary><strong>Production vs debug build — what's the difference?</strong></summary>

```bash
pio run -e esp32dev -t upload    # production (default): silent UART, no IDF logs
pio run -e debug    -t upload    # debug: full boot + periodic dumps, IDF verbose
```

Production builds compile every `LOG_*` macro to `((void)0)` and never call `Serial.begin()`, so the UART stays closed during shows (no half-formatted strings going to a port nobody's reading). Saves ~50 KB flash too.
</details>

---

## License

MIT — see [`LICENSE`](LICENSE).

## Trademarks

Art-Net™ Designed by and Copyright Artistic Licence Holdings Ltd.
