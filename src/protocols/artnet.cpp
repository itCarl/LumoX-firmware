#include "protocols/ArtNetSource.h"
#include "Lumox.h"
#include "config.h"

#include <ETH.h>

// ── Art-Net OpCodes ────────────────────────────────────────────────────────
static constexpr uint16_t OP_POLL        = 0x2000;
static constexpr uint16_t OP_POLL_REPLY  = 0x2100;
static constexpr uint16_t OP_DIAG_DATA   = 0x2300;
static constexpr uint16_t OP_COMMAND     = 0x2400;
static constexpr uint16_t OP_DMX         = 0x5000;
static constexpr uint16_t OP_SYNC        = 0x5200;   // Art-Net 4 §10
static constexpr uint16_t OP_ADDRESS     = 0x6000;

static constexpr uint16_t ART_PROT_VER   = 14;

// ArtPoll TalkToMe flags (byte 12)
static constexpr uint8_t TTM_REPLY_ON_CHANGE = 0x02;
static constexpr uint8_t TTM_SEND_DIAG       = 0x04;
static constexpr uint8_t TTM_DIAG_UNICAST    = 0x08;

static constexpr uint32_t ART_SYNC_TIMEOUT_MS = 4000;
static constexpr uint32_t ART_ANNOUNCE_MS     = 10000;

// ArtAddress command codes (byte 106)
static constexpr uint8_t AC_NONE         = 0x00;
static constexpr uint8_t AC_CANCEL_MERGE = 0x01;
static constexpr uint8_t AC_LED_NORMAL   = 0x02;
static constexpr uint8_t AC_LED_MUTE     = 0x03;
static constexpr uint8_t AC_LED_LOCATE   = 0x04;
static constexpr uint8_t AC_RESET_FLAGS  = 0x05;
static constexpr uint8_t AC_CLEAR_OP0    = 0xE0;

static constexpr uint8_t NC_UNIVERSE     = 0x7F;

bool ArtNetSource::begin() {
    if (!_udp.begin(ARTNET_UDP_PORT)) {
        LOG_PRINTLN("[ArtNet] UDP bind 6454 failed");
        return false;
    }
    LOG_PRINTF("[ArtNet] Listening on UDP:%d  (Universe %u)\n",
                  ARTNET_UDP_PORT, Lumox::instance().cfgUniverse);
    return true;
}

void ArtNetSource::end() {
    _udp.stop();
}

void ArtNetSource::onNetworkChange() {
    sendArtPollReply(IPAddress(255, 255, 255, 255));
}

void ArtNetSource::service() {
    // Drain socket every iteration so bursts don't strand packets in the queue.
    while (true) {
        const int len = _udp.parsePacket();
        if (len <= 0) break;
        const IPAddress sender = _udp.remoteIP();
        const int read = _udp.read(_udpBuf, sizeof(_udpBuf));
        _dispatch(_udpBuf, read, sender);
    }
    _tick();
}

bool ArtNetSource::_dispatch(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 10 || memcmp(buf, "Art-Net\0", 8) != 0) return false;

    const uint16_t opcode = buf[8] | (uint16_t(buf[9]) << 8);

    // ProtVer check (spec §6.4) — bytes 10-11 BE. Reply uses 10-13 for IP
    // so skip the check on that opcode (we never receive PollReply anyway).
    if (opcode != OP_POLL_REPLY && len >= 12) {
        const uint16_t protVer = (uint16_t(buf[10]) << 8) | buf[11];
        if (protVer < ART_PROT_VER) {
            _statsBadProto++;
            return false;
        }
    }

    switch (opcode) {
        case OP_POLL:    _handlePoll(buf, len, sender);        return false;
        case OP_DMX:     return _parseDmx(buf, len, sender);
        case OP_SYNC:    _handleSync();                        return false;
        case OP_ADDRESS: _handleAddress(buf, len, sender);     return false;
        case OP_COMMAND: _handleCommand(buf, len, sender);     return false;
        default:                                                return false;
    }
}

void ArtNetSource::_handlePoll(const uint8_t* buf, int len, IPAddress sender) {
    if (len >= 14) {
        _pollTalkToMe     = buf[12];
        _pollDiagPriority = buf[13];
        _pollDiagTarget   = sender;
    }
    sendArtPollReply(sender);
}

void ArtNetSource::_handleSync() {
    _statsArtSyncs++;
    _statsSyncLastMs = millis();
    _inSyncMode      = true;

    if (_shadowDirty) {
        // Flush shadow → live via the unified ingestion path so the sender
        // lock + swap detection stay coherent. Shadow content was captured
        // from a single primary sender; pass that sender on.
        Lumox::instance().feedDmxFrame(&_shadow[1], _shadowLen, _shadowSender);
        _shadowDirty = false;
    }
}

bool ArtNetSource::_parseDmx(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 18) return false;

    auto& lx = Lumox::instance();
    const uint16_t universe = buf[14] | (uint16_t(buf[15]) << 8);
    lx.statsArtnetLastUni = universe;
    if (universe != lx.cfgUniverse) return false;

    uint16_t dataLen = (uint16_t(buf[16]) << 8) | buf[17];
    if (dataLen > 512) dataLen = 512;
    if (len < 18 + dataLen) return false;

    // Sequence check (byte 12). seq=0 disables ordering (spec). Reject
    // strictly older packets from same sender — int8 cast handles wrap.
    // Duplicates (diff == 0) accepted: senders that hold a static frame
    // and re-emit with same seq byte (e.g. QLC+ EFX Partial) rely on this.
    // cfgSkipStaleSeq=false bypasses the check entirely (debug aid).
    const uint8_t seq = buf[12];
    if (seq != 0 && lx.cfgSkipStaleSeq) {
        if (sender == _lastSeqSender && _lastSeq != 0) {
            const int8_t diff = (int8_t)(seq - _lastSeq);
            if (diff < 0) {
                lx.statsArtnetStale++;
                return false;
            }
        }
    }

    if (_inSyncMode) {
        // Buffer to shadow — OpSync flushes via feedDmxFrame. Shadow sender
        // tracked so the flush honours the sender-lock contract.
        memcpy(&_shadow[1], &buf[18], dataLen);
        _shadowLen    = dataLen;
        _shadowDirty  = true;
        _shadowSender = sender;
        // Update seq state — flush only happens on OpSync.
        _lastSeq       = seq;
        _lastSeqSender = sender;
        return true;
    }

    if (lx.feedDmxFrame(&buf[18], dataLen, sender)) {
        _lastSeq       = seq;
        _lastSeqSender = sender;
        return true;
    }
    // Sender locked out — drop seq tracking so the next packet from the
    // accepted sender starts fresh.
    return false;
}

// ── UDP send helpers ──────────────────────────────────────────────────────
// Single lwIP socket — route table picks the netif by destination. Unicast
// replies egress on whichever interface that IP is reachable through.
void ArtNetSource::_sendUdp(IPAddress target, uint16_t port,
                            const uint8_t* buf, size_t len) {
    _udp.beginPacket(target, port);
    _udp.write(buf, len);
    _udp.endPacket();
}

// 255.255.255.255 only egresses on the default netif. To reach controllers
// on both ETH and WiFi we send to each interface's directed broadcast.
void ArtNetSource::_broadcastReply(const uint8_t* buf, size_t len) {
    auto bcastFor = [](IPAddress ip, IPAddress mask) -> IPAddress {
        return IPAddress(ip[0] | (uint8_t)~mask[0],
                         ip[1] | (uint8_t)~mask[1],
                         ip[2] | (uint8_t)~mask[2],
                         ip[3] | (uint8_t)~mask[3]);
    };
    auto& lx = Lumox::instance();
    bool sent = false;
    if (lx.isEth()) {
        IPAddress b = bcastFor(ETH.localIP(), ETH.subnetMask());
        _sendUdp(b, ARTNET_UDP_PORT, buf, len);
        sent = true;
    }
    if (!lx.isApMode() && WiFi.status() == WL_CONNECTED) {
        IPAddress b = bcastFor(WiFi.localIP(), WiFi.subnetMask());
        _sendUdp(b, ARTNET_UDP_PORT, buf, len);
        sent = true;
    }
    if (lx.isApActive()) {
        IPAddress b = bcastFor(WiFi.softAPIP(), IPAddress(255, 255, 255, 0));
        _sendUdp(b, ARTNET_UDP_PORT, buf, len);
        sent = true;
    }
    if (!sent) {
        _sendUdp(IPAddress(255, 255, 255, 255), ARTNET_UDP_PORT, buf, len);
    }
}

void ArtNetSource::sendArtPollReply(IPAddress target) {
    auto& lx = Lumox::instance();
    uint8_t reply[239] = {0};

    memcpy(reply, "Art-Net\0", 8);
    reply[8] = OP_POLL_REPLY & 0xFF;
    reply[9] = (OP_POLL_REPLY >> 8) & 0xFF;

    IPAddress ip = lx.getIP();
    reply[10] = ip[0]; reply[11] = ip[1]; reply[12] = ip[2]; reply[13] = ip[3];

    reply[14] = ARTNET_UDP_PORT & 0xFF;
    reply[15] = (ARTNET_UDP_PORT >> 8) & 0xFF;

    reply[16] = FW_VERSION_MAJOR;
    reply[17] = FW_VERSION_MINOR;

    reply[18] = (lx.cfgUniverse >> 8) & 0x7F;
    reply[19] = (lx.cfgUniverse >> 4) & 0x0F;

    reply[20] = (ARTNET_OEM_CODE >> 8) & 0xFF;
    reply[21] = ARTNET_OEM_CODE & 0xFF;
    reply[22] = 0;

    {
        const uint8_t indicator = identifyActive() ? 0b01 : 0b11;
        reply[23] = (indicator << 6) | (0b11 << 4);
    }

    reply[24] = ARTNET_ESTA_CODE & 0xFF;
    reply[25] = (ARTNET_ESTA_CODE >> 8) & 0xFF;

    snprintf((char*)&reply[26], 18, "%s", lx.cfgDeviceName.c_str());
    snprintf((char*)&reply[44], 64, "Lumox DMX U%u %s",
             lx.cfgUniverse, lx.cfgDeviceName.c_str());

    const char* statusText = (lx.statsArtnetPackets == 0) ? "Awaiting ArtDMX" : "Receiving";
    snprintf((char*)&reply[108], 64, "#0001 [%04u] %s",
             _nodeReportSeq++ & 0xFFFF, statusText);

    reply[172] = 0;
    reply[173] = 1;
    reply[174] = 0x80;
    reply[182] = 0x80;          // GoodOutput — data being transmitted
    reply[190] = lx.cfgUniverse & 0x0F;
    reply[200] = 0x00;          // StNode

    uint8_t mac[6];
    lx.getMac(mac);
    memcpy(&reply[201], mac, 6);
    reply[207] = ip[0]; reply[208] = ip[1]; reply[209] = ip[2]; reply[210] = ip[3];
    reply[211] = 1;
    reply[212] = lx.isApMode() ? 0x4D : 0x4F;

    if (target == IPAddress(255, 255, 255, 255)) {
        _broadcastReply(reply, 239);
    } else {
        _sendUdp(target, ARTNET_UDP_PORT, reply, 239);
    }
}

void ArtNetSource::_sendDiagData(IPAddress target, uint8_t priority, const char* text) {
    const bool subscriptionMode = (target == _pollDiagTarget);
    if (subscriptionMode) {
        if (!(_pollTalkToMe & TTM_SEND_DIAG))     return;
        if (priority < _pollDiagPriority)         return;
        if (!(_pollTalkToMe & TTM_DIAG_UNICAST)) target = IPAddress(255,255,255,255);
    }

    uint8_t buf[512 + 14] = {0};
    const size_t tlen = strnlen(text, 500);

    memcpy(buf, "Art-Net\0", 8);
    buf[8] = OP_DIAG_DATA & 0xFF;
    buf[9] = (OP_DIAG_DATA >> 8) & 0xFF;
    buf[10] = 0; buf[11] = 14;
    buf[12] = 0;
    buf[13] = priority;
    buf[14] = 0; buf[15] = 0;
    const uint16_t lenField = (uint16_t)(tlen + 1);
    buf[16] = (lenField >> 8) & 0xFF;
    buf[17] = lenField & 0xFF;
    memcpy(&buf[18], text, tlen);
    buf[18 + tlen] = 0;

    _sendUdp(target, ARTNET_UDP_PORT, buf, 18 + tlen + 1);
}

void ArtNetSource::_tick() {
    const uint32_t now = millis();

    if (_inSyncMode && (now - _statsSyncLastMs) > ART_SYNC_TIMEOUT_MS) {
        _inSyncMode = false;
        LOG_PRINTLN("[ArtNet] ArtSync timeout — reverting to non-sync mode");
        if (_shadowDirty) {
            Lumox::instance().feedDmxFrame(&_shadow[1], _shadowLen, _shadowSender);
            _shadowDirty = false;
        }
    }

    if (now - _lastAnnounceMs >= ART_ANNOUNCE_MS) {
        _lastAnnounceMs = now;
        sendArtPollReply(IPAddress(255, 255, 255, 255));
    }
}

void ArtNetSource::_handleAddress(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 107) return;
    auto& lx = Lumox::instance();

    bool changed = false;

    const uint8_t netSwitch = buf[12];
    const uint8_t subSwitch = buf[104];
    const uint8_t swOut0    = buf[100];

    uint16_t newUni = lx.cfgUniverse;
    if (netSwitch != NC_UNIVERSE) newUni = (newUni & 0x00FF) | ((netSwitch & 0x7F) << 8);
    if (subSwitch != NC_UNIVERSE) newUni = (newUni & 0xFF0F) | ((subSwitch & 0x0F) << 4);
    if (swOut0    != NC_UNIVERSE) newUni = (newUni & 0xFFF0) | (swOut0 & 0x0F);
    if (newUni != lx.cfgUniverse) {
        lx.cfgUniverse = newUni;
        changed = true;
    }

    if (buf[14] != 0) {
        char name[19] = {0};
        memcpy(name, &buf[14], 18);
        String s(name);
        s.trim();
        if (s.length() > 0 && s != lx.cfgDeviceName) {
            lx.cfgDeviceName = s;
            changed = true;
        }
    }

    const uint8_t cmd = buf[106];
    switch (cmd) {
        case AC_LED_LOCATE:
            lx.setIdentify(10000);
            LOG_PRINTLN("[ArtNet] Identify (LED locate) 10 s");
            break;
        case AC_LED_NORMAL:
        case AC_LED_MUTE:
            lx.setIdentify(0);
            break;
        case AC_CLEAR_OP0:
            lx.clearDmxBuffer();
            LOG_PRINTLN("[ArtNet] ClearOp0 — DMX buffer zeroed");
            break;
        case AC_RESET_FLAGS:
            lx.statsArtnetStale  = 0;
            lx.statsArtnetLocked = 0;
            break;
        case AC_CANCEL_MERGE:
        case AC_NONE:
        default:
            break;
    }

    if (changed) {
        lx.saveConfig();
        LOG_PRINTF("[ArtNet] OpAddress from %s: uni=%u name=\"%s\"\n",
                      sender.toString().c_str(), lx.cfgUniverse, lx.cfgDeviceName.c_str());
    }
    sendArtPollReply(sender);
}

void ArtNetSource::_handleCommand(const uint8_t* buf, int len, IPAddress sender) {
    if (len < 18) return;
    auto& lx = Lumox::instance();

    const uint16_t textLen = (uint16_t(buf[14]) << 8) | buf[15];
    if (textLen == 0 || 16 + textLen > len) return;

    char text[256] = {0};
    const size_t copyLen = textLen > 255 ? 255 : textLen;
    memcpy(text, &buf[16], copyLen);

    String cmd(text);
    cmd.toLowerCase();
    cmd.trim();

    LOG_PRINTF("[ArtNet] OpCommand from %s: \"%s\"\n",
                  sender.toString().c_str(), cmd.c_str());

    if (cmd.startsWith("clear")) {
        lx.clearDmxBuffer();
        _sendDiagData(sender, 0x40, "DMX cleared");
    } else if (cmd.startsWith("reboot") || cmd.startsWith("restart")) {
        _sendDiagData(sender, 0x80, "Rebooting");
        delay(100);
        ESP.restart();
    } else if (cmd.startsWith("identify") || cmd.startsWith("locate")) {
        lx.setIdentify(10000);
        _sendDiagData(sender, 0x10, "Identify 10 s");
    } else if (cmd.startsWith("resetstats")) {
        lx.statsArtnetStale  = 0;
        lx.statsArtnetLocked = 0;
        _sendDiagData(sender, 0x10, "Stats reset");
    } else {
        _sendDiagData(sender, 0x10, "Unknown command");
    }
}

void ArtNetSource::httpStatus(JsonObject& out) {
    out["badProto"] = _statsBadProto;
    out["syncs"]    = _statsArtSyncs;
    out["inSync"]   = _inSyncMode;
}
