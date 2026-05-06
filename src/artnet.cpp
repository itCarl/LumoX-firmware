#include "Lumox.h"
#include "config.h"

// ── Art-Net OpCodes ────────────────────────────────────────────────────────
static constexpr uint16_t OP_POLL        = 0x2000;
static constexpr uint16_t OP_POLL_REPLY  = 0x2100;
static constexpr uint16_t OP_DIAG_DATA   = 0x2300;
static constexpr uint16_t OP_COMMAND     = 0x2400;
static constexpr uint16_t OP_DMX         = 0x5000;
static constexpr uint16_t OP_SYNC        = 0x5200;   // Art-Net 4 §10
static constexpr uint16_t OP_ADDRESS     = 0x6000;

// Art-Net 4 current protocol version (spec: receivers must reject < 14).
static constexpr uint16_t ART_PROT_VER   = 14;

// ArtPoll TalkToMe flags (byte 12)
static constexpr uint8_t TTM_REPLY_ON_CHANGE = 0x02;  // node must reply on change
static constexpr uint8_t TTM_SEND_DIAG       = 0x04;  // node should send diag
static constexpr uint8_t TTM_DIAG_UNICAST    = 0x08;  // diag unicast to poll sender

// ArtSync mode timeout — spec: node reverts to non-sync if no sync in 4 s.
static constexpr uint32_t ART_SYNC_TIMEOUT_MS = 4000;

// Periodic unsolicited ArtPollReply — reannounce every 10 s so controllers
// that missed discovery can still find us.
static constexpr uint32_t ART_ANNOUNCE_MS     = 10000;

// ── ArtAddress command codes (byte 106) ───────────────────────────────────
static constexpr uint8_t AC_NONE         = 0x00;
static constexpr uint8_t AC_CANCEL_MERGE = 0x01;
static constexpr uint8_t AC_LED_NORMAL   = 0x02;
static constexpr uint8_t AC_LED_MUTE     = 0x03;
static constexpr uint8_t AC_LED_LOCATE   = 0x04;   // identify blink
static constexpr uint8_t AC_RESET_FLAGS  = 0x05;
static constexpr uint8_t AC_CLEAR_OP0    = 0xE0;   // zero output universe 0

// "No change" sentinels per Art-Net 4 spec
static constexpr uint8_t NC_UNIVERSE     = 0x7F;

void Lumox::beginArtNet() {
    // Single lwIP socket bound to INADDR_ANY:6454 — receives on every netif
    // (WiFi STA, AP, ETH). No per-interface socket needed.
    _udp.begin(ARTNET_UDP_PORT);
    LOG_PRINTF("[ArtNet] Listening on UDP:%d  (Universe %u)\n",
                  ARTNET_UDP_PORT, cfgUniverse);
}

// Returns true whenever a packet was *consumed* from the socket — drain
// semantics. Earlier this returned the dispatch result, which was true only
// for accepted ArtDMX. Non-DMX packets (ArtPoll/Sync/Address) terminated the
// drain loop and stranded any further packets in the RX buffer until the next
// loop tick → bursty stutter under QLC+ load.
bool Lumox::pollArtNet() {
    int len = _udp.parsePacket();
    if (len <= 0) return false;

    IPAddress sender = _udp.remoteIP();
    int read = _udp.read(_udpBuf, sizeof(_udpBuf));
    _dispatchArtNet(_udpBuf, read, sender);
    return true;
}

// ── Shared packet dispatcher ─────────────────────────────────────────────
// Header + ProtVer validation in one place so WiFi and ETH paths behave
// identically. Returns true only for accepted ArtDMX (drives loop's drain).
bool Lumox::_dispatchArtNet(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 10 || memcmp(buf, "Art-Net\0", 8) != 0) return false;

    const uint16_t opcode = buf[8] | (uint16_t(buf[9]) << 8);

    // ProtVer check — spec §6.4. Bytes 10-11 BE; valid for packets OTHER THAN
    // OpPollReply (reply uses bytes 10-13 for IP). All packets we accept here
    // carry ProtVer at [10..11] and must be ≥ 14.
    if (opcode != OP_POLL_REPLY && len >= 12) {
        const uint16_t protVer = (uint16_t(buf[10]) << 8) | buf[11];
        if (protVer < ART_PROT_VER) {
            statsArtNetBadProto++;
            return false;
        }
    }

    switch (opcode) {
        case OP_POLL:
            _handleArtPoll(buf, len, sender);
            return false;

        case OP_DMX: {
            bool ok = _parseArtNet(buf, len, sender);
            if (ok) {
                statsArtnetPackets++;
                statsArtnetLastMs = millis();
                if (statsArtnetSender != sender) {
                    if ((uint32_t)statsArtnetSender != 0) {
                        statsArtnetSenderSwaps++;
                        statsArtnetLastSwapMs = millis();
                        LOG_PRINTF("[ArtNet] ⚠ sender swap %s → %s (total %u)\n",
                                      statsArtnetSender.toString().c_str(),
                                      sender.toString().c_str(),
                                      (unsigned)statsArtnetSenderSwaps);
                    }
                    statsArtnetSender = sender;
                }
                // (mutex high-water decay is time-based now — see _dmxTask.)
            }
            return ok;
        }

        case OP_SYNC:
            _handleArtSync();
            return false;

        case OP_ADDRESS:
            _handleArtAddress(buf, len, sender);
            return false;

        case OP_COMMAND:
            _handleArtCommand(buf, len, sender);
            return false;

        default:
            return false;
    }
}

// ── ArtPoll handler ────────────────────────────────────────────────────────
// Spec §11: node must reply, and should latch the TalkToMe flags + priority
// for future ArtDiagData routing.
void Lumox::_handleArtPoll(const uint8_t* buf, int len, IPAddress sender) {
    if (len >= 14) {
        _pollTalkToMe     = buf[12];
        _pollDiagPriority = buf[13];
        _pollDiagTarget   = sender;
    }
    sendArtPollReply(sender);
}

// ── ArtSync handler — multi-node frame release ─────────────────────────────
// Spec §10: when in sync mode, ArtDMX updates the shadow buffer; ArtSync
// flips shadow → live so all receiving nodes release simultaneously. Reverts
// to non-sync after ART_SYNC_TIMEOUT_MS of silence.
void Lumox::_handleArtSync() {
    statsArtSyncs++;
    statsArtSyncLastMs = millis();
    statsInSyncMode    = true;

    if (_dmxShadowDirty) {
        portENTER_CRITICAL(&_dmxLock);
        memcpy(&dmxBuffer[1], &_dmxShadow[1], _dmxShadowLen);
        dmxChannelCount = _dmxShadowLen;
        portEXIT_CRITICAL(&_dmxLock);
        _dmxShadowDirty = false;
    }
}

// ── UDP send helper ────────────────────────────────────────────────────────
// Single lwIP socket — route table picks the right netif based on dest IP.
// Unicast replies to the controller's IP egress on whichever interface that
// IP is reachable through (no per-packet bookkeeping needed).
void Lumox::_sendArtNetUdp(IPAddress target, uint16_t port,
                           const uint8_t* buf, size_t len) {
    _udp.beginPacket(target, port);
    _udp.write(buf, len);
    _udp.endPacket();
}

// Broadcast helper for unsolicited ArtPollReply. lwIP routes 255.255.255.255
// out the default netif only — to ensure both WiFi and ETH controllers see
// our announce, send to each interface's directed broadcast address.
void Lumox::_broadcastArtPollReply(const uint8_t* buf, size_t len) {
    auto bcastFor = [](IPAddress ip, IPAddress mask) -> IPAddress {
        return IPAddress(ip[0] | (uint8_t)~mask[0],
                         ip[1] | (uint8_t)~mask[1],
                         ip[2] | (uint8_t)~mask[2],
                         ip[3] | (uint8_t)~mask[3]);
    };
    bool sent = false;
    if (_ethUp) {
        IPAddress b = bcastFor(ETH.localIP(), ETH.subnetMask());
        _udp.beginPacket(b, ARTNET_UDP_PORT);
        _udp.write(buf, len);
        _udp.endPacket();
        sent = true;
    }
    if (!_apMode && WiFi.status() == WL_CONNECTED) {
        IPAddress b = bcastFor(WiFi.localIP(), WiFi.subnetMask());
        _udp.beginPacket(b, ARTNET_UDP_PORT);
        _udp.write(buf, len);
        _udp.endPacket();
        sent = true;
    }
    if (_apActive) {
        // AP serving (fallback or aux to ETH). Send to AP subnet broadcast so
        // any controller on the AP side (phone, laptop) sees the announce.
        IPAddress b = bcastFor(WiFi.softAPIP(), IPAddress(255, 255, 255, 0));
        _udp.beginPacket(b, ARTNET_UDP_PORT);
        _udp.write(buf, len);
        _udp.endPacket();
        sent = true;
    }
    if (!sent) {
        // No netif up yet — limited broadcast as a last resort.
        _udp.beginPacket(IPAddress(255, 255, 255, 255), ARTNET_UDP_PORT);
        _udp.write(buf, len);
        _udp.endPacket();
    }
}

// ── ArtPollReply (239 bytes, Art-Net 4 layout) ─────────────────────────────
// Byte layout corrected per spec. Populates firmware version, GoodOutput
// from live DMX state, rolling NodeReport, proper SwOut position, BindIndex.
void Lumox::sendArtPollReply(IPAddress target) {
    uint8_t reply[239] = {0};

    // 0-7 : ID
    memcpy(reply, "Art-Net\0", 8);

    // 8-9 : OpCode (LE)
    reply[8] = OP_POLL_REPLY & 0xFF;
    reply[9] = (OP_POLL_REPLY >> 8) & 0xFF;

    // 10-13 : IP
    IPAddress ip = getIP();
    reply[10] = ip[0]; reply[11] = ip[1]; reply[12] = ip[2]; reply[13] = ip[3];

    // 14-15 : Port (LE)
    reply[14] = ARTNET_UDP_PORT & 0xFF;
    reply[15] = (ARTNET_UDP_PORT >> 8) & 0xFF;

    // 16-17 : Firmware version (high, low)
    reply[16] = FW_VERSION_MAJOR;
    reply[17] = FW_VERSION_MINOR;

    // 18 : NetSwitch (Net bits 14-8 of Port-Address)
    reply[18] = (cfgUniverse >> 8) & 0x7F;
    // 19 : SubSwitch (bits 7-4)
    reply[19] = (cfgUniverse >> 4) & 0x0F;

    // 20-21 : OEM (high, low)
    reply[20] = (ARTNET_OEM_CODE >> 8) & 0xFF;
    reply[21] = ARTNET_OEM_CODE & 0xFF;

    // 22 : UbeaVersion
    reply[22] = 0;

    // 23 : Status1
    //   bits 7-6 : Indicator state (11=normal, 01=locate)
    //   bits 5-4 : Port address authority (11=web+Art-Net)
    //   bit 1    : RDM (0 = unsupported)
    //   bit 0    : UBEA (0)
    {
        uint8_t indicator = (_identifyUntilMs > millis()) ? 0b01 : 0b11;
        reply[23] = (indicator << 6) | (0b11 << 4);
    }

    // 24-25 : ESTA manufacturer (LE)
    reply[24] = ARTNET_ESTA_CODE & 0xFF;
    reply[25] = (ARTNET_ESTA_CODE >> 8) & 0xFF;

    // 26-43 : ShortName (18 bytes, null-terminated)
    snprintf((char*)&reply[26], 18, "%s", cfgDeviceName.c_str());

    // 44-107 : LongName (64 bytes)
    snprintf((char*)&reply[44], 64, "Lumox DMX U%u %s",
             cfgUniverse, cfgDeviceName.c_str());

    // 108-171 : NodeReport — "#xxxx [nnnn] text"
    //   xxxx = status code (0001 = booted/ok)
    //   nnnn = rolling counter (NodeReport seq)
    const char* statusText = (statsArtnetPackets == 0)
        ? "Awaiting ArtDMX"
        : "Receiving";
    snprintf((char*)&reply[108], 64, "#0001 [%04u] %s",
             _nodeReportSeq++ & 0xFFFF, statusText);

    // 172-173 : NumPorts (BE)
    reply[172] = 0;
    reply[173] = 1;

    // 174-177 : PortTypes[4]
    //   bit 7 = output from Art-Net, bit 6 = input to Art-Net, bits 5-0 = protocol
    //   protocol 0x00 = DMX-512
    reply[174] = 0x80;

    // 178-181 : GoodInput[4] — no inputs on this node
    // (all zero = no data received since we don't consume input)

    // 182-185 : GoodOutput[4]
    //   bit 7 = data being transmitted, bit 1 = merge LTP
    //   bit 2 = merging from multiple sources (placeholder off)
    reply[182] = _dmxReady ? 0x80 : 0x00;

    // 186-189 : SwIn[4] — unused, leave zero

    // 190-193 : SwOut[4] — universe low nibble
    reply[190] = cfgUniverse & 0x0F;

    // 194 : AcnPriority (Art-Net 4) — leave 0
    // 195 : SwMacro
    // 196 : SwRemote

    // 200 : Style — StNode (0x00)
    reply[200] = 0x00;

    // 201-206 : MAC (cached once at boot — see Lumox::begin)
    memcpy(&reply[201], _macCached, 6);

    // 207-210 : BindIp (same as node IP for single-bind devices)
    reply[207] = ip[0]; reply[208] = ip[1]; reply[209] = ip[2]; reply[210] = ip[3];

    // 211 : BindIndex — 1 = primary
    reply[211] = 1;

    // 212 : Status2
    //   bit 0 : web browser config
    //   bit 1 : DHCP configured IP
    //   bit 2 : DHCP-capable
    //   bit 3 : Art-Net 3/4 support
    //   bit 6 : 15-bit port addressing
    reply[212] = _apMode ? 0x4D : 0x4F;

    // Limited broadcast = unsolicited announce → send to every netif's
    // directed broadcast so both WiFi and ETH controllers see it.
    if (target == IPAddress(255, 255, 255, 255)) {
        _broadcastArtPollReply(reply, 239);
    } else {
        _sendArtNetUdp(target, ARTNET_UDP_PORT, reply, 239);
    }
}

// ── ArtDiagData — node-to-controller diagnostic text ──────────────────────
// Spec §11: only emit when a controller has subscribed via TalkToMe bit 2,
// and only at or above its requested priority. Avoids flooding the network.
void Lumox::sendArtDiagData(IPAddress target, uint8_t priority, const char* text) {
    // Gate: controller-triggered calls carry target directly; unsolicited
    // emits go through _pollDiagTarget and must honour subscription + priority.
    const bool subscriptionMode = (target == _pollDiagTarget);
    if (subscriptionMode) {
        if (!(_pollTalkToMe & TTM_SEND_DIAG))      return;
        if (priority < _pollDiagPriority)          return;
        // Unicast iff requested; else broadcast to the diag priority audience.
        if (!(_pollTalkToMe & TTM_DIAG_UNICAST)) target = IPAddress(255,255,255,255);
    }

    uint8_t buf[512 + 14] = {0};
    const size_t tlen = strnlen(text, 500);

    memcpy(buf, "Art-Net\0", 8);
    buf[8] = OP_DIAG_DATA & 0xFF;
    buf[9] = (OP_DIAG_DATA >> 8) & 0xFF;
    buf[10] = 0; buf[11] = 14;           // ProtVer BE
    buf[12] = 0;                         // Filler1
    buf[13] = priority;                  // 0x10=Low, 0x40=Med, 0x80=High, 0xE0=Critical
    buf[14] = 0; buf[15] = 0;            // Filler2
    // 16-17: Length of text including null (BE)
    const uint16_t lenField = (uint16_t)(tlen + 1);
    buf[16] = (lenField >> 8) & 0xFF;
    buf[17] = lenField & 0xFF;
    memcpy(&buf[18], text, tlen);
    buf[18 + tlen] = 0;

    _sendArtNetUdp(target, ARTNET_UDP_PORT, buf, 18 + tlen + 1);
}

// ── ArtDMX Parser ───────────────────────────────────────────────────────
// Drops corrupted, wrong-universe, and out-of-order packets. Buffer holds last
// valid frame so the DMX task keeps retransmitting on network hiccups.
bool Lumox::_parseArtNet(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 18)                               return false;

    uint16_t universe = buf[14] | (uint16_t(buf[15]) << 8);
    statsArtnetLastUni = universe;
    if (universe != cfgUniverse)                return false;

    uint16_t dataLen = (uint16_t(buf[16]) << 8) | buf[17];
    if (dataLen > 512) dataLen = 512;
    if (len < 18 + dataLen)                     return false;

    // Single-sender lockout: while the current primary is live (< stale window),
    // reject packets from any other IP. When primary goes stale, any sender may
    // take over on the next packet.
    if (statsArtnetLastMs != 0 && sender != statsArtnetSender) {
        const uint32_t since = millis() - statsArtnetLastMs;
        if (since < DMX_LINK_STALE_MS) {
            statsArtnetLocked++;
            return false;
        }
        _lastSeq = 0;
    }

    // Art-Net sequence check (byte 12). seq=0 disables ordering. Reject only
    // strictly-older packets (diff < 0) — duplicates (diff == 0) are common
    // when senders hold a static frame and re-emit with the same seq byte
    // (e.g. QLC+ EFX Partial, controllers that don't increment on identical
    // updates). Treating duplicates as stale dropped 80%+ of legitimate
    // traffic on those senders. int8_t cast handles wraparound.
    const uint8_t seq = buf[12];
    if (seq != 0) {
        if (sender == _lastSeqSender && _lastSeq != 0) {
            const int8_t diff = (int8_t)(seq - _lastSeq);
            if (diff < 0) {
                statsArtnetStale++;
                return false;
            }
        }
        _lastSeq       = seq;
        _lastSeqSender = sender;
    }

    if (statsInSyncMode) {
        // Sync mode: write to shadow; ArtSync will flip it to live.
        memcpy(&_dmxShadow[1], &buf[18], dataLen);
        _dmxShadowLen   = dataLen;
        _dmxShadowDirty = true;
    } else {
        portENTER_CRITICAL(&_dmxLock);
        memcpy(&dmxBuffer[1], &buf[18], dataLen);
        dmxChannelCount = dataLen;
        portEXIT_CRITICAL(&_dmxLock);
    }

    return true;
}

// ── Periodic Art-Net housekeeping ─────────────────────────────────────────
// Drives two spec behaviors:
//   1. ArtSync timeout — revert to non-sync after 4 s silence
//   2. Unsolicited ArtPollReply every 10 s — improves controller discovery
//      when our first reply was lost (or controller started after us).
void Lumox::loopArtNet() {
    const uint32_t now = millis();

    if (statsInSyncMode && (now - statsArtSyncLastMs) > ART_SYNC_TIMEOUT_MS) {
        statsInSyncMode = false;
        LOG_PRINTLN("[ArtNet] ArtSync timeout — reverting to non-sync mode");
        // Flush any pending shadow so we don't hold stale values.
        if (_dmxShadowDirty) {
            portENTER_CRITICAL(&_dmxLock);
            memcpy(&dmxBuffer[1], &_dmxShadow[1], _dmxShadowLen);
            dmxChannelCount = _dmxShadowLen;
            portEXIT_CRITICAL(&_dmxLock);
            _dmxShadowDirty = false;
        }
    }

    if (now - _lastAnnounceMs >= ART_ANNOUNCE_MS) {
        _lastAnnounceMs = now;
        sendArtPollReply(IPAddress(255, 255, 255, 255));   // unsolicited
    }
}

// ── OpAddress (0x6000) — remote configuration from controller ─────────────
// Allows the Lumox app to set universe, device name, or run node commands
// without visiting the /config web page.
void Lumox::_handleArtAddress(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 107) return;

    bool changed = false;

    // 12 : NetSwitch (or 0x7F = no change)
    const uint8_t netSwitch = buf[12];
    // 104 : SubSwitch
    const uint8_t subSwitch = buf[104];
    // 100 : SwOut[0]
    const uint8_t swOut0    = buf[100];

    uint16_t newUni = cfgUniverse;
    if (netSwitch != NC_UNIVERSE) newUni = (newUni & 0x00FF) | ((netSwitch & 0x7F) << 8);
    if (subSwitch != NC_UNIVERSE) newUni = (newUni & 0xFF0F) | ((subSwitch & 0x0F) << 4);
    if (swOut0    != NC_UNIVERSE) newUni = (newUni & 0xFFF0) | (swOut0 & 0x0F);

    if (newUni != cfgUniverse) {
        cfgUniverse = newUni;
        changed = true;
    }

    // 14-31 : ShortName (18 bytes). Spec: first byte 0 = no change.
    if (buf[14] != 0) {
        char name[19] = {0};
        memcpy(name, &buf[14], 18);
        String s(name);
        s.trim();
        if (s.length() > 0 && s != cfgDeviceName) {
            cfgDeviceName = s;
            changed = true;
        }
    }

    // 106 : Command
    const uint8_t cmd = buf[106];
    switch (cmd) {
        case AC_LED_LOCATE:
            _identifyUntilMs = millis() + 10000;     // 10 s identify
            LOG_PRINTLN("[ArtNet] Identify (LED locate) 10 s");
            break;

        case AC_LED_NORMAL:
        case AC_LED_MUTE:
            _identifyUntilMs = 0;
            break;

        case AC_CLEAR_OP0: {
            portENTER_CRITICAL(&_dmxLock);
            memset(&dmxBuffer[1], 0, 512);
            portEXIT_CRITICAL(&_dmxLock);
            LOG_PRINTLN("[ArtNet] ClearOp0 — DMX buffer zeroed");
            break;
        }

        case AC_RESET_FLAGS:
            statsArtnetStale  = 0;
            statsArtnetLocked = 0;
            break;

        case AC_CANCEL_MERGE:
        case AC_NONE:
        default:
            break;
    }

    if (changed) {
        saveConfig();
        LOG_PRINTF("[ArtNet] OpAddress from %s: uni=%u name=\"%s\"\n",
                      sender.toString().c_str(), cfgUniverse, cfgDeviceName.c_str());
    }

    // Spec: always reply after OpAddress
    sendArtPollReply(sender);
}

// ── OpCommand (0x2400) — ASCII control commands ───────────────────────────
// Parses null-terminated text from byte 16 onwards. Recognised:
//   Clear           → zero DMX output buffer
//   Reboot          → restart node
//   Identify        → LED locate for 10 s
//   ResetStats      → zero stale/locked counters
void Lumox::_handleArtCommand(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 18) return;

    const uint16_t textLen = (uint16_t(buf[14]) << 8) | buf[15];
    if (textLen == 0 || 16 + textLen > len) return;

    char text[256] = {0};
    const size_t copyLen = textLen > 255 ? 255 : textLen;
    memcpy(text, &buf[16], copyLen);

    // Normalise: lowercase, trim
    String cmd(text);
    cmd.toLowerCase();
    cmd.trim();

    LOG_PRINTF("[ArtNet] OpCommand from %s: \"%s\"\n",
                  sender.toString().c_str(), cmd.c_str());

    if (cmd.startsWith("clear")) {
        portENTER_CRITICAL(&_dmxLock);
        memset(&dmxBuffer[1], 0, 512);
        portEXIT_CRITICAL(&_dmxLock);
        sendArtDiagData(sender, 0x40, "DMX cleared");
    }
    else if (cmd.startsWith("reboot") || cmd.startsWith("restart")) {
        sendArtDiagData(sender, 0x80, "Rebooting");
        delay(100);
        ESP.restart();
    }
    else if (cmd.startsWith("identify") || cmd.startsWith("locate")) {
        _identifyUntilMs = millis() + 10000;
        sendArtDiagData(sender, 0x10, "Identify 10 s");
    }
    else if (cmd.startsWith("resetstats")) {
        statsArtnetStale  = 0;
        statsArtnetLocked = 0;
        sendArtDiagData(sender, 0x10, "Stats reset");
    }
    else {
        sendArtDiagData(sender, 0x10, "Unknown command");
    }
}
