<div align="center">

# Lumox Firmware

**Wireless DMX node for ESP32 — Art-Net over WiFi or Ethernet → DMX512**

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange?logo=platformio)](https://platformio.org/)
[![Framework](https://img.shields.io/badge/Framework-Arduino--ESP32-00979D?logo=arduino)](https://github.com/espressif/arduino-esp32)
[![Art-Net](https://img.shields.io/badge/Art--Net-4-blueviolet)](https://art-net.org.uk/)
[![Version](https://img.shields.io/badge/version-0.3.0-success)](package.json)
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

- **Dual interface** — WiFi (STA + AP fallback) **and** Wiznet W5500 ethernet in parallel.
  Wired link is automatically preferred once DHCP/link is up.
- **Art-Net 4** — `OpPoll`, `OpDmx`, `OpSync`, `OpAddress`, `OpCommand`, `OpDiagData`
- **mDNS discovery** — `_lumox._tcp` service, hostname `lumox-xxyy.local`
- **Captive portal** — automatic redirect in AP mode for fast first-time setup
- **Async web UI** — dashboard, config, health monitor, manual override (all gzipped in PROGMEM)
- **OTA updates** — browser upload via [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) at `/update`
- **WebSocket live push** — status + DMX slots @ ~5 Hz for live monitoring
- **Flicker hardening** — sequence check, single-sender lock, sender-swap detection, mutex-guarded buffer
- **ArtSync mode** — shadow buffer + atomic flip for synchronous multi-node frames

---

## Hardware

### DMX wiring

```
┌─────────────┐                ┌──────────┐                ┌──────────┐
│    ESP32    │                │  MAX485  │                │ XLR 5-pin│
├─────────────┤                ├──────────┤                ├──────────┤
│  GPIO 17 TX ├───────────────▶│  DI      │                │          │
│  GPIO 16 RX │◀───────────────┤  RO      │                │          │
│  GPIO  4 DE ├──────┬────────▶│  DE      │     A ─────────┤ Pin 3 (+)│
│             │      └────────▶│  RE      │     B ─────────┤ Pin 2 (−)│
│       3.3V  ├───────────────▶│  VCC     │    GND ────────┤ Pin 1    │
│       GND   ├───────────────▶│  GND     │                │          │
└─────────────┘                └──────────┘                └──────────┘
```

> **Tip:** MAX485 also runs on 3.3V VCC, but is more signal-stable at 5V.
> In that case use a level shifter on DI, or fit a MAX3485 / ISO3088 in the
> 3.3V variant directly.

### Ethernet (W5500, optional)

| ESP32 GPIO | W5500 |
|:-:|:-|
| 5  | CS  |
| 18 | SCK |
| 19 | MISO |
| 23 | MOSI |
| 34 | INT |
| 33 | RST |

No W5500 connected? → `Ethernet.begin()` simply fails, firmware continues on WiFi.

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
- **Per-sender sequence check** — out-of-order / duplicate packets are dropped
- **Single-sender lock** — while primary sender is live (`DMX_LINK_STALE_MS`), other IPs are rejected; primary stale → next sender takes over
- **Sender-swap detection** — controller flips mid-show are counted + visible on `/health`
- **Mutex-guarded buffer** — no tearing between parser and DMX-TX task
- **Stale-link LED** — steady on while receiving, slow pulse on timeout

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

`esp_dmx` 4.1 crashes on UART2 with Arduino-ESP32 core 2.x. UART1 is stable.
</details>

<details>
<summary><strong>Rebuild the web UI after code changes?</strong></summary>

```bash
npm run build               # gzip → include/html_*.h, writes version.h
```

Happens automatically on `pio run` via `pio-scripts/build_ui.py`.
</details>

---

## License

MIT — see [`LICENSE`](LICENSE).
