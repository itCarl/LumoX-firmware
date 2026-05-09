# sACN E1.31 — Protocol Reference

Practical guide for developers and technicians working with sACN / E1.31 (including Lumox). Covers what sACN is, how it differs from Art-Net, the wire format, addressing, multicast, priority arbitration, and how Lumox implements it.

## What is sACN / E1.31?

**sACN** = "Streaming ACN" = streaming variant of ACN (Architecture for Control Networks). Standardised by **ESTA / PLASA** as **ANSI E1.31** (current revision: E1.31-2018). The professional, vendor-neutral counterpart to Art-Net.

- **Purpose**: carry DMX512 (and a tiny bit more) over IP. Same job as Art-Net.
- **Transport**: UDP, port **5568**, IPv4 multicast `239.255.<universe-hi>.<universe-lo>` (unicast also legal).
- **Royalty-free**, full spec at ESTA. Designed by a committee of broadcast/theatre vendors; tighter, more "enterprise" feel than Art-Net.
- **Adoption**: ETC, Pathway, Vectorworks, GrandMA, Hog, Resolume — most pro consoles speak it.

## sACN vs Art-Net at a glance

| Aspect | sACN E1.31 | Art-Net 4 |
|---|---|---|
| Default port | 5568 | 6454 |
| Default delivery | **Multicast** (`239.255.U.U`) | Broadcast/unicast |
| Universe range | 1..63999 (16-bit) | 0..32767 (15-bit) |
| Discovery | E1.31 Universe Discovery (multicast) | ArtPoll / ArtPollReply |
| Priority | Built-in 0..200 per packet | None (Art-Net 4 added an AcnPriority hint) |
| Sender ID | 16-byte UUID (CID) | IP address |
| Spec author | ESTA (vendor committee) | Artistic Licence |
| Header size | ~125 bytes (3 ACN layers) | 18 bytes (ArtDMX) |
| Sync | E1.31 Sync (separate packet w/ universe) | OpSync (single broadcast) |
| RDM | E1.20 over E1.33 (separate spec) | OpRdm in same protocol |
| Used by | ETC, MA, Pathway, theatre/broadcast | DIY, Resolume, MagicQ, club/event |

Both are widely supported. Many nodes implement both. **Lumox switches between them** via the `cfgProtocol` setting.

## Where it sits

```
Lighting console (ETC Eos / GrandMA / Hog / xLights / Resolume / Madrix)
        │
        │  UDP 5568, multicast 239.255.U.U  (or unicast)
        ▼
sACN node (Lumox / ETC Net3 Gateway / Pathway VIA / Enttec ODE Mk3)
        │
        │  DMX512 — XLR, RS-485, 250 kbps
        ▼
Fixtures
```

## Universe addressing

A "universe" is 512 DMX channels — same as Art-Net. sACN universes are **1..63999** (Universe 0 is reserved; 64000+ are reserved for Universe Discovery and future use).

The universe number directly drives the multicast group:

```
Universe N  →  239.255.<N high byte>.<N low byte>
Universe 1  →  239.255.0.1
Universe 2  →  239.255.0.2
Universe 256 → 239.255.1.0
Universe 1000 → 239.255.3.232
```

Receivers join the multicast group for the universes they need. The network only delivers traffic for those universes (IGMP-snooping switches make this efficient at scale).

**Always-multicast**: even if you only have 1 receiver, sACN normally sends multicast. Unicast is allowed and works on the same socket; some gateways direct-send.

## ACN packet structure

sACN packets are **three nested ACN layers** wrapped in UDP:

```
┌──────────────────────────────────────────────────────────┐
│ Root Layer    (38 bytes)                                 │
│   ├─ Preamble + ACN Packet Identifier "ASC-E1.17"        │
│   ├─ Length / Vector (= 0x00000004 for E1.31 data)       │
│   └─ CID (16-byte UUID — sender identity)                │
├──────────────────────────────────────────────────────────┤
│ Framing Layer  (77 bytes)                                │
│   ├─ Length / Vector (= 0x00000002 for E1.31 data)       │
│   ├─ Source Name (64-byte UTF-8)                         │
│   ├─ Priority (0..200)                                   │
│   ├─ Sync Universe (for Sync Packet pairing)             │
│   ├─ Sequence Number                                     │
│   ├─ Options flags                                       │
│   └─ Universe (1..63999)                                 │
├──────────────────────────────────────────────────────────┤
│ DMP Layer      (10 bytes header + slot data)             │
│   ├─ Length / Vector (= 0x02 SET_PROPERTY)               │
│   ├─ Address Type / First Address / Increment            │
│   ├─ Property Value Count (1..513 — start code + slots)  │
│   └─ Property Values: [start_code][slot1][slot2]...      │
└──────────────────────────────────────────────────────────┘
```

Total minimum packet (zero slots) = 126 bytes. Full universe (start code + 512 slots) = 638 bytes.

## Key fields

### CID (Component Identifier)

16-byte UUID, generated once per sender, embedded in every packet. Receivers use it to track which physical source is sending — survives IP changes (DHCP renewal, NIC swap). Unlike Art-Net's "sender = IP" model.

Lumox tracks senders by IP for parity with the Art-Net path (CID-aware tracking is a possible improvement).

### Priority (byte 108, framing layer)

Range **0..200**, default **100**. The killer feature vs Art-Net.

When two sources send to the same universe:
- Higher priority wins. Always.
- Equal priority: receiver merges (HTP/LTP) or picks one (depends on implementation).
- Priority `0` = "do not output" sentinel for some receivers.

This lets you do **automatic backup**: main console at priority 100, backup at priority 99. If the main console drops off the network, the backup takes over within the receiver's stale window. When main returns, it pre-empts again.

**Lumox**: the active source's priority is held for `DMX_LINK_STALE_MS` (2 s). Lower-priority frames during that window are rejected (`lowerPrio` stat). After 2 s of silence, any priority can take over.

### Sequence number (byte 111)

8-bit, wraps. Same purpose as Art-Net's seq byte: drop reordered packets. `0` disabled.

### Options flags (byte 112)

| Bit | Meaning |
|---|---|
| 6 | **Stream Terminated** — sender announces it's stopping. Receivers should release priority hold. |
| 7 | Preview Data — non-output preview stream (mostly ignored) |
| 5 | Force Synchronization — see Sync packet section |

Lumox honours Stream Terminated (clears priority).

### Sync Universe (bytes 109-110)

Non-zero = this packet is part of a synchronised group. Receivers buffer the frame until a matching **E1.31 Sync packet** arrives on the sync universe. Same idea as Art-Net's OpSync but *per-group* instead of *one global flush*.

(Lumox doesn't implement E1.31 Sync yet — single-universe-per-node, no tearing problem to solve. Sync universe field is read but ignored.)

### Start Code (first byte of DMP property values)

| Code | Meaning |
|---|---|
| `0x00` | NULL — standard DMX, the only thing Lumox forwards |
| `0xCC` | RDM (E1.20) — handled by E1.33 RDMnet receivers |
| `0xDD` | System Information Packet |
| other | Vendor-defined / proprietary |

Lumox drops anything but `0x00` (`statsBadStartCode++`).

## Multicast group join

Receivers must IGMP-join `239.255.<U-hi>.<U-lo>` for each universe they want. On a switched LAN with IGMP snooping, only joined ports get traffic — keeps the network quiet even with hundreds of universes flying around.

**Gotcha**: cheap unmanaged switches flood multicast like broadcast. Fine for small shows; ugly at scale. **IGMP-snooping** switch is recommended for any show with > ~16 universes.

**Gotcha 2**: WiFi multicast often reverts to lowest-rate broadcast → poor performance. Many APs convert multicast to unicast per-client (Aruba, Ruckus, Cisco) — confirm the AP supports this for sACN over WiFi.

**Lumox** joins the configured universe's group on every active netif (ETH + STA + AP). Re-joins on `onNetworkChange()` because interface changes silently drop group memberships otherwise.

## Universe Discovery

Separate packet type, multicast on universe **64214** (`239.255.250.214`). Senders periodically broadcast a list of every universe they're sending. Receivers can build a discovery view without polling.

Lumox doesn't emit Universe Discovery packets (they're sender-side; Lumox is RX-only). Universal Discovery on the **receive** side is also not implemented — Lumox uses mDNS for being-found instead.

## Tools & consoles that speak sACN

- **ETC Eos / Cobalt** — sACN is the native ETC protocol (E1.31 plus E1.33 RDMnet)
- **GrandMA2 / GrandMA3** — sACN + Art-Net + MA-Net
- **Hog 4** — Hog OS speaks sACN
- **Avolites Titan** — sACN out
- **xLights / Vixen / LightShowPi** — DIY/holiday-light tools
- **Madrix** — pro pixel mapping
- **Resolume Arena** — sACN + Art-Net out from video
- **sACNView** (free, Windows) — receiver/sender debug tool
- **Wireshark** with sACN dissector — packet-level debugging
- **OLA** (Open Lighting Architecture) — Linux daemon, multi-protocol bridge

For free local testing: **sACNView** to send, Lumox to receive on the matching universe.

## Common pitfalls

| Symptom | Likely cause |
|---|---|
| Receiver doesn't see anything | IGMP not joined on the right netif, or switch drops multicast, or universe mismatch |
| WiFi receiver flaky | AP not converting multicast→unicast; lowest-rate broadcast = drops |
| Two consoles randomly swap | Both at priority 100; receiver merge logic kicks in unpredictably |
| Backup never takes over | Priority equal; should be backup < primary, primary stop = backup wins |
| One channel works, others don't | Universe number off by 1 (universe 0 doesn't exist in sACN — start at 1) |
| Console says "sending" but nothing arrives | Multicast TTL = 0 (some implementations); check sender's TTL setting |
| Output goes black on console pause | Stream Terminated flag — sender released priority. Many receivers respect it |
| sACN works on bench, fails at venue | Switch IGMP snooping with no querier → multicast gets pruned. Need an IGMP querier on the LAN |

## Performance / network design

- **Refresh rate**: same 44 Hz cap as DMX (can't transmit faster on the wire). E1.31 senders typically run 30–44 Hz.
- **Bandwidth per universe**: 638 B × 44 Hz ≈ **225 kbit/s**. Trivial on Gigabit, fine on 100 Mbps.
- **Packet rate**: 44 packets/s/universe. 64 universes = ~3000 packets/s — small for any switch.
- **VLAN isolation**: best practice — put lighting on its own VLAN, no broadcast storms from office traffic.
- **Multicast querier**: managed switch must have one (or be told the upstream router is the querier). Without it, snooping prunes everything.

## Lumox-specific notes

- Listens on UDP 5568; binds `INADDR_ANY` so it serves both ETH and WiFi.
- IGMP joins `239.255.<U-hi>.<U-lo>` for the configured universe on every active netif (ETH, STA, AP).
- Re-joins on `onNetworkChange()` (interface up/down).
- Validates ACN packet ID (`ASC-E1.17`), root vector (`0x4`), framing vector (`0x2`), DMP vector (`0x2`), DMP start code (`0x00`).
- Universe filter against `cfgUniverse`.
- Priority arbitration: lower priority dropped while a higher-priority sender is live (< 2 s ago).
- Stream Terminated flag honoured (releases priority hold).
- Sequence check per-sender, drops strictly-older packets.
- Calls `Lumox::feedDmxFrame()` on accept — universal sender lock + swap detection apply.
- E1.31 Sync packets: not implemented yet (single-universe node, no inter-universe tearing).
- Universe Discovery (sender-side advertise): not implemented.

## Picking sACN vs Art-Net for a show

| Use | Pick |
|---|---|
| Theatre / broadcast / big rigs | sACN — proper priority, IGMP discipline, ETC-native |
| Club / event / VJ / DIY | Art-Net — simpler stack, looser networks tolerate it |
| Mixed venue, both consoles in play | Lumox supports both — set the protocol per device |
| Lots of universes (>32) on one cable | sACN — IGMP keeps the network sane |
| Single console, single switch, < 10 nodes | Either works. Pick whatever the console defaults to |

## Spec links

- ANSI E1.31-2018 PDF: [ESTA TSP](https://tsp.esta.org/tsp/documents/published_docs.php) (search "E1.31")
- ACN spec (root protocol): [ANSI E1.17](https://tsp.esta.org/tsp/documents/published_docs.php)
- RDM over IP: ANSI E1.33 (RDMnet)
- Wikipedia: [E1.31](https://en.wikipedia.org/wiki/E1.31) — quick overview, less detailed than the PDF
- Reference C++ library: [forkineye/ESPAsyncE131](https://github.com/forkineye/ESPAsyncE131) (used by WLED)
- Linux daemon: [OpenLightingProject/ola](https://www.openlighting.org/ola/) — speaks every protocol

## Quick mental model

> "sACN is DMX over UDP multicast. Every universe N goes to `239.255.<N>`. Receivers IGMP-join the groups they care about. Each packet carries a priority (0..200) and a sender UUID, so multiple consoles can run in priority-stacked failover without configuration. ACN's three-layer header is bigger than Art-Net's — the price for vendor-neutral framing and proper failover semantics."
