# Art-Net — Protocol Reference

> **Art-Net™ Designed by and Copyright Artistic Licence Holdings Ltd.**

Practical guide for developers and technicians working with Art-Net nodes (including Lumox). Covers what Art-Net is, the wire format, addressing, discovery, common gotchas, and how Lumox implements it.

## What is Art-Net?

Art-Net is a UDP-based protocol for sending DMX512 lighting data over Ethernet/WiFi. Created by **Artistic Licence** (UK, ~1998). Royalty-free, open spec, the de-facto standard for IP-to-DMX.

- **Purpose**: carry DMX512 (and a bit more) across IP networks instead of XLR cables.
- **Transport**: UDP, port **6454**, default network `2.0.0.0/8` (Class A — Art-Net II convention; not enforced, any subnet works).
- **Latency**: ~1 ms LAN, ~10 ms typical WiFi. One DMX frame ≈ 22 ms on the wire (44 Hz max), so packet jitter > 22 ms = visible flicker.
- **Versions**: Art-Net I (1998), II (2006), 3 (2011), 4 (2016, current). Lumox implements **Art-Net 4** with **15-bit Port-Address** support.

## Where it sits

```
Lighting console (QLC+ / MagicQ / Resolume / xLights / GrandMA)
        │
        │  UDP 6454 (Art-Net) — over Ethernet or WiFi
        ▼
Art-Net node (Lumox / Enttec ODE / Open DMX Ethernet / DIY ESP32)
        │
        │  DMX512 — XLR-3/5, RS-485 differential, 250 kbps
        ▼
Fixtures (lights, dimmers, moving heads, fog machines, ...)
```

Each Art-Net node = one or more **DMX universes** (= 512 channels). Lumox = 1 universe per ESP32. A console can drive dozens of nodes simultaneously.

## Universes & Port-Addresses

A "universe" is 512 DMX channels. Big shows need many. Art-Net 4 addresses up to **32,768 universes** with a 15-bit Port-Address split into three nibbles:

```
Port-Address (15 bit) = Net (7 bit)  ‖  Sub-Net (4 bit)  ‖  Universe (4 bit)
                        bits 14..8       bits 7..4           bits 3..0
```

Example: Port-Address `0x0103` = Net 0, Sub-Net 1, Universe 3 → decimal "Universe 19" in console UIs.

Most consoles let you set the universe as a single decimal number (0..32767) and split internally. Lumox `cfgUniverse` is the full 15-bit value.

**Convention**: Universe 0 is the first; some old gear is 1-based. When in doubt, sniff with Wireshark.

## IP addressing

Art-Net II suggested **`2.x.x.x / 255.0.0.0`**, with the first octet `2` to mark "Art-Net subnet". Modern shows use whatever the venue gives them (typically `192.168.x.x` from DHCP). Both work; consoles don't enforce the `2.x.x.x` convention.

Lumox defaults to `2.0.0.10 / /8` for static-ETH mode, but DHCP works fine on any subnet.

## Packet format

All Art-Net packets share an 8-byte ID + 2-byte OpCode header:

| Bytes | Field | Notes |
|------:|---|---|
| 0–7   | `Art-Net\0` magic | ASCII, null-terminated, exactly 8 bytes |
| 8–9   | OpCode | **little-endian** (the only field that is) |
| 10–11 | ProtVer | big-endian, ≥ 14 for Art-Net 4 |
| ...   | OpCode-specific payload | |

Total max ~530 bytes (ArtDMX = 18 + 512). UDP MTU: stays well under 1500 even on Ethernet.

## Major OpCodes

| OpCode | Name | Direction | Purpose |
|---|---|---|---|
| `0x2000` | **OpPoll** | controller → broadcast | "Anyone out there?" — discovery query |
| `0x2100` | **OpPollReply** | node → unicast/broadcast | "Yes, I'm here" — 239-byte node descriptor |
| `0x2300` | OpDiagData | node → controller | Free-form diagnostic text (with priority filter) |
| `0x2400` | OpCommand | controller → node | ASCII commands (`Clear`, `Reboot`, `Identify`, …) |
| `0x5000` | **OpDmx** | controller → node | The actual DMX frame (1–512 channels) |
| `0x5200` | OpSync | controller → broadcast | "Release frame now" — multi-node frame coherence |
| `0x6000` | OpAddress | controller → node | Remote re-config (universe, name, identify) |
| `0x8300` | OpRdm / OpTodRequest | both | RDM bidirectional — fixture configuration over Art-Net |

Lumox implements: Poll, PollReply, DiagData, Command, Dmx, Sync, Address. RDM is not implemented (Status1 advertises `0` for the RDM bit).

### ArtDMX (OpDmx, 0x5000) — the hot path

| Bytes | Field | Notes |
|------:|---|---|
| 0–9   | Art-Net header + OpCode | |
| 10–11 | ProtVer | BE, ≥ 14 |
| 12    | Sequence | 1..255, 0 = sequencing disabled |
| 13    | Physical | informational, 0 OK |
| 14    | Sub-Universe | low byte of Port-Address (Sub + Uni) |
| 15    | Net | high byte of Port-Address |
| 16–17 | Length | BE, 2..512, must be even (spec) |
| 18..  | DMX data | `Length` bytes |

Senders bump the Sequence byte 1→255→1 so receivers can drop reordered packets. Sequence = 0 disables ordering — used by some fixtures and proxies.

### ArtPollReply (0x2100) — 239 bytes

This is the discovery response. Big and finicky. Key fields a controller cares about:

| Offset | Field | Lumox value |
|------:|---|---|
| 10–13 | IP of node | active interface (ETH preferred over STA) |
| 14–15 | Port (LE) | 6454 |
| 16–17 | Firmware version | `FW_VERSION_MAJOR`, `FW_VERSION_MINOR` |
| 18    | NetSwitch | bits 14..8 of Port-Address |
| 19    | SubSwitch | bits 7..4 |
| 20–21 | OEM | manufacturer ID (Lumox ships generic `0x00FF`) |
| 23    | Status1 | indicator state, port authority, RDM bit |
| 24–25 | ESTA | manufacturer code (Lumox: 0x0000 = none) |
| 26–43 | ShortName | 18 bytes, null-terminated |
| 44–107 | LongName | 64 bytes |
| 108–171 | NodeReport | `#xxxx [seq] text` — rolling status |
| 174–177 | PortTypes[4] | `0x80` = output |
| 190–193 | SwOut[4] | low nibble of Port-Address |
| 201–206 | MAC | 6 bytes |
| 211 | BindIndex | 1 = primary |
| 212 | Status2 | DHCP / Art-Net 3+ / 15-bit / web-config flags |

If a controller doesn't see your node it's almost always because of these bytes — wrong OEM/Status flags can hide nodes from picky controllers.

### ArtSync (0x5200) — multi-universe frame coherence

When a console drives several nodes (e.g. Stage Left + Stage Right + Truss), naive ArtDMX makes each node release its frame as soon as it arrives — visible tearing on cross-fades. ArtSync fixes this:

1. Console sends OpDmx to N nodes (no immediate output).
2. Console broadcasts OpSync.
3. All N nodes flip their shadow buffer to the wire on the same UDP receive.

Lumox enters sync mode on first OpSync received; reverts after 4 s without one. ArtDMX while in sync mode goes to a `_shadow[513]` buffer; OpSync flushes it through `Lumox::feedDmxFrame()` so the sender lock + swap detection still apply.

### OpAddress (0x6000) — remote configuration

Console can set Net/Sub/Uni, ShortName, and trigger commands:
- `0x04` LED Locate → identify blink (Lumox: 10 s)
- `0x05` Reset Flags
- `0xE0` Clear Op0 → zero output buffer
- `0x7F` "no change" sentinel for universe nibbles

After OpAddress: Lumox saves to NVS and emits an unsolicited PollReply.

## Discovery flow

1. Controller broadcasts ArtPoll on UDP 6454.
2. Every node replies with ArtPollReply (unicast back to controller, or broadcast if controller asks).
3. Controller builds its node list. May poll periodically (every few seconds) or rely on unsolicited replies.

Nodes **should** also send unsolicited PollReplies on state changes (IP gained, universe changed, name edited) and periodically (~10 s in Lumox) so a controller that started after the node still finds it without re-polling.

`255.255.255.255` (limited broadcast) only egresses on the **default** netif. Multi-homed nodes (Lumox = ETH + WiFi STA + AP) must send to each interface's **directed broadcast** explicitly:

```
broadcast(ip, mask) = ip | ~mask
```

Lumox does this in `ArtNetSource::_broadcastReply`.

## Priority / merging / arbitration

Art-Net spec: when multiple controllers send to the same universe, nodes can merge **HTP** (Highest Takes Precedence) or **LTP** (Latest Takes Precedence). Most nodes pick one and stick with it.

**Lumox doesn't merge**. It locks onto a single primary sender for `DMX_LINK_STALE_MS` (2 s) and rejects others. Once stale, any sender can take over. Sender swaps mid-show are counted (`statsArtnetSenderSwaps`) and surfaced on `/health` because they cause flicker.

Art-Net 4 also adds an `AcnPriority` field in PollReply (byte 194) for sACN compatibility — Lumox sends 0 (no priority preference).

## Sequence number

Byte 12 of ArtDMX. Senders increment 1..255, wrap to 1 (skip 0 in some senders). Receivers can drop out-of-order packets.

**Gotcha**: some senders (e.g. QLC+ with EFX in Partial mode) re-emit the same sequence byte for static frames. Treating duplicates (`seq == last`) as stale drops 80%+ of legitimate traffic. Lumox accepts duplicates (`int8_t diff >= 0`) and only rejects strictly older packets.

`seq = 0` disables ordering entirely. Honour it.

## Tools & consoles that speak Art-Net

- **QLC+** (free, cross-platform) — preferred dev/test tool
- **MagicQ** (ChamSys, free up to 4 universes via dongle, paid for more)
- **GrandMA2 / GrandMA3** (MA Lighting, industry standard)
- **Resolume Arena** — VJ software, sends DMX from video timeline
- **xLights** — show-design / sequencing
- **Madrix** — LED-pixel-mapping software
- **ETC Eos** — Broadway / theatre console
- **Avolites** — Tiger Touch, etc.
- **Wireshark** with [Art-Net dissector](https://wiki.wireshark.org/ArtNet) — debugging gold

For free local testing: install QLC+, set output to Art-Net unicast → Lumox IP, set universe to match.

## Common pitfalls

| Symptom | Likely cause |
|---|---|
| Node not in console's discovery list | Firewall blocks UDP 6454, or wrong VLAN, or PollReply missing OEM/Status bits the console expects |
| Light flickers under WiFi load | `WiFi.setSleep(false)` not called → modem sleep adds 100+ ms jitter |
| One channel works, others don't | Universe mismatch (off by ±1, or Net/Sub/Uni split confused) |
| Two consoles, weird mid-show jumps | Both sending to same universe; receiver merges or sender-locks differently |
| Output stops when controller is paused | Receiver doesn't retransmit; needs persistent buffer (Lumox does this) |
| EFX/wave effects stutter | Seq-byte handling rejects duplicates as stale (sender bug or strict receiver) |
| Multi-node show tears across fixtures | No ArtSync — controller sends DMX async, each node releases at different times |

## Lumox-specific notes

- Single UDP 6454 socket, bound `INADDR_ANY` — receives on every netif (ETH, STA, AP) at once.
- Per-sender sequence tracking + single-sender lock (`DMX_LINK_STALE_MS = 2000`).
- ArtSync shadow + 4 s timeout per spec §10.
- ProtVer ≥ 14 enforced; lower → `statsBadProto++`.
- Sender swap counted + logged; visible on `/health`.
- DMX retransmits at ~44 Hz from `dmxBuffer` regardless of network state — fixtures stay lit during WiFi hiccups.
- OpAddress writes are persisted to NVS and trigger a fresh PollReply.
- OpCommand `Reboot` reboots the node after a 100 ms grace.

## Spec links

- Official PDF: [Art-Net 4 specification (Artistic Licence)](https://artisticlicence.com/WebSiteMaster/User%20Guides/art-net.pdf)
- Wikipedia: [Art-Net](https://en.wikipedia.org/wiki/Art-Net)
- DMX512 underneath: [ANSI E1.11](https://tsp.esta.org/tsp/documents/published_docs.php)

## Quick mental model

> "Art-Net is DMX over UDP. Every packet is 'here are channels 1–512 of universe N, sequence S, please display now.' Controllers broadcast 'who's there?' (Poll); nodes reply with their identity (PollReply). Add OpSync if you have multiple nodes that must release together. That's 90% of it."
