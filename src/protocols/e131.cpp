#include "protocols/E131Source.h"
#include "Lumox.h"
#include "config.h"

#include <lwip/igmp.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>

// ── E1.31 (sACN) constants ─────────────────────────────────────────────────
// Frame layout per ANSI E1.31-2018. Three-layer ACN packet: Root, Framing, DMP.
// Total min length = 126 bytes for a zero-slot DMX frame.
static constexpr uint16_t E131_PORT             = 5568;

static constexpr uint8_t  ACN_PACKET_ID[12]     = {
    'A','S','C','-','E','1','.','1','7', 0x00, 0x00, 0x00
};

static constexpr uint32_t VECTOR_ROOT_E131_DATA = 0x00000004;
static constexpr uint32_t VECTOR_E131_DATA      = 0x00000002;
static constexpr uint8_t  VECTOR_DMP_SET_PROP   = 0x02;

static constexpr uint16_t E131_UNIVERSE_OFFSET  = 113;   // BE
static constexpr uint16_t E131_PRIORITY_OFFSET  = 108;
static constexpr uint16_t E131_SEQNUM_OFFSET    = 111;
static constexpr uint16_t E131_OPTIONS_OFFSET   = 112;
static constexpr uint16_t E131_PVCOUNT_OFFSET   = 123;   // BE — DMP property value count (incl. start code byte)
static constexpr uint16_t E131_DMP_VECTOR       = 117;
static constexpr uint16_t E131_DMP_DATA_OFFSET  = 125;   // start of [start_code][slot1][slot2]...

static constexpr uint8_t  E131_OPT_TERMINATED   = 0x40;  // stream terminated flag

bool E131Source::begin() {
    if (!_udp.begin(E131_PORT)) {
        LOG_PRINTLN("[E131] UDP bind 5568 failed");
        return false;
    }
    auto& lx = Lumox::instance();
    LOG_PRINTF("[E131] Listening on UDP:%u  (Universe %u, %s)\n",
                  E131_PORT, lx.cfgUniverse,
                  lx.cfgE131Multicast ? "multicast + unicast" : "unicast only");
    if (lx.cfgE131Multicast) _joinMulticast();
    return true;
}

void E131Source::end() {
    _udp.stop();
    _multicastJoined = false;
}

// Multicast group for sACN universe N is 239.255.<N high byte>.<N low byte>.
// IGMP join must run on the active netif — Arduino's WiFiUDP doesn't expose
// a per-netif join, so we use lwIP directly. Joining on every active netif
// keeps both ETH and WiFi paths working.
bool E131Source::_joinMulticast() {
    const uint16_t uni = Lumox::instance().cfgUniverse;
    if (uni == 0 || uni > LUMOX_MAX_UNIVERSE) {
        LOG_PRINTF("[E131] Universe %u out of range (1..%u) — skipping multicast join\n",
                      uni, LUMOX_MAX_UNIVERSE);
        return false;
    }
    ip4_addr_t group;
    IP4_ADDR(&group, 239, 255, (uni >> 8) & 0xFF, uni & 0xFF);

    bool any = false;
    auto joinOn = [&](IPAddress ifIp) {
        if ((uint32_t)ifIp == 0) return;
        ip4_addr_t ifaddr;
        ifaddr.addr = (uint32_t)ifIp;
        // igmp_joingroup is a raw lwIP API and asserts when called from outside
        // the tcpip thread. We're on the protocol RX task → must lock the core
        // explicitly. Same pattern arduino-esp32 uses internally for WiFiUDP
        // multicast joins.
        LOCK_TCPIP_CORE();
        const err_t e = igmp_joingroup(&ifaddr, &group);
        UNLOCK_TCPIP_CORE();
        if (e == ERR_OK) {
            LOG_PRINTF("[E131] IGMP join 239.255.%u.%u on %s\n",
                          (uni >> 8) & 0xFF, uni & 0xFF, ifIp.toString().c_str());
            any = true;
        } else {
            LOG_PRINTF("[E131] IGMP join failed on %s (err=%d)\n",
                          ifIp.toString().c_str(), (int)e);
        }
    };
    auto& lx = Lumox::instance();
    if (lx.isEth())                            joinOn(lx.getEthIp());
    if (WiFi.status() == WL_CONNECTED)         joinOn(WiFi.localIP());
    if (lx.isApActive())                       joinOn(WiFi.softAPIP());
    _multicastJoined = any;
    if (!any) LOG_PRINTLN("[E131] No interface available for IGMP join — unicast only");
    return any;
}

void E131Source::onNetworkChange() {
    // Re-join on every netif change — interfaces coming up/down silently drop
    // group memberships otherwise. Skipped if user disabled multicast.
    if (Lumox::instance().cfgE131Multicast) _joinMulticast();
}

void E131Source::service() {
    while (true) {
        const int len = _udp.parsePacket();
        if (len <= 0) break;
        const IPAddress sender = _udp.remoteIP();
        const int read = _udp.read(_udpBuf, sizeof(_udpBuf));
        _dispatch(_udpBuf, read, sender);
    }
}

bool E131Source::_dispatch(const uint8_t* buf, int len, IPAddress sender) {
    // Min E1.31 DMX packet (zero slots) = 126 bytes. Anything shorter is junk.
    if (len < 126) { _statsBadHeader++; return false; }

    // ACN root — vector + packet identifier.
    if (memcmp(&buf[4], ACN_PACKET_ID, 12) != 0) { _statsBadHeader++; return false; }
    const uint32_t rootVec = (uint32_t(buf[18]) << 24) | (uint32_t(buf[19]) << 16) |
                              (uint32_t(buf[20]) << 8)  | uint32_t(buf[21]);
    if (rootVec != VECTOR_ROOT_E131_DATA) { _statsBadHeader++; return false; }

    // Framing layer vector.
    const uint32_t frameVec = (uint32_t(buf[40]) << 24) | (uint32_t(buf[41]) << 16) |
                               (uint32_t(buf[42]) << 8)  | uint32_t(buf[43]);
    if (frameVec != VECTOR_E131_DATA) { _statsBadHeader++; return false; }

    // DMP layer vector.
    if (buf[E131_DMP_VECTOR] != VECTOR_DMP_SET_PROP) { _statsBadHeader++; return false; }

    // Universe filter — silently ignore packets for other universes (consoles
    // often blast all 64k for discovery / preview).
    const uint16_t uni = (uint16_t(buf[E131_UNIVERSE_OFFSET]) << 8) |
                         buf[E131_UNIVERSE_OFFSET + 1];
    auto& lx = Lumox::instance();
    if (uni != lx.cfgUniverse) return false;

    // Stream-terminated → release any priority hold and stop accepting.
    const uint8_t options = buf[E131_OPTIONS_OFFSET];
    if (options & E131_OPT_TERMINATED) {
        _activePriority = 0;
        return false;
    }

    // Property value count includes the start code byte at offset 125.
    const uint16_t pvCount = (uint16_t(buf[E131_PVCOUNT_OFFSET]) << 8) |
                              buf[E131_PVCOUNT_OFFSET + 1];
    if (pvCount < 1 || pvCount > 513) { _statsBadHeader++; return false; }
    if (E131_DMP_DATA_OFFSET + pvCount > (uint16_t)len) { _statsBadHeader++; return false; }

    // Start code must be 0x00 for null DMX. Anything else (RDM 0xCC, ASCII
    // 0x17, etc.) is a non-DMX stream we don't transmit.
    if (buf[E131_DMP_DATA_OFFSET] != 0x00) {
        _statsBadStartCode++;
        return false;
    }

    const uint8_t prio = buf[E131_PRIORITY_OFFSET];
    const uint32_t now = millis();

    // Minimum-priority filter — drop frames below the user's threshold so
    // preview / low-prio test feeds can be ignored while honouring the active
    // show. 0 (default) accepts everything.
    if (prio < lx.cfgE131MinPriority) {
        _statsLowerPrio++;
        return false;
    }

    // Priority arbitration — within DMX_LINK_STALE_MS, a higher-priority
    // source wins. Equal or higher wins; lower is dropped. Ties go to the
    // current sender (first-come wins). After the stale window, any priority
    // can take over.
    if (_lastPrioMs != 0 && (now - _lastPrioMs) < DMX_LINK_STALE_MS) {
        if (prio < _activePriority) {
            _statsLowerPrio++;
            return false;
        }
    }

    // Sequence — 8-bit wrap. Reject strictly-older packets from same sender.
    // Toggleable: cfgSkipStaleSeq=false bypasses this check (use only for
    // debugging buggy senders that recycle sequence bytes).
    const uint8_t seq = buf[E131_SEQNUM_OFFSET];
    if (lx.cfgSkipStaleSeq && sender == _lastSeqSender && _lastPrioMs != 0) {
        const int8_t diff = (int8_t)(seq - _lastSeq);
        if (diff < 0) return false;
    }

    const uint16_t slotCount = pvCount - 1;   // exclude start code
    const uint8_t* slots     = &buf[E131_DMP_DATA_OFFSET + 1];

    if (lx.feedDmxFrame(slots, slotCount, sender)) {
        _activePriority = prio;
        _lastPrioMs     = now;
        _lastSeq        = seq;
        _lastSeqSender  = sender;
        return true;
    }
    return false;
}

void E131Source::httpStatus(JsonObject& out) {
    out["multicast"]    = _multicastJoined;
    out["priority"]     = _activePriority;
    out["minPrio"]      = Lumox::instance().cfgE131MinPriority;
    out["badHeader"]    = _statsBadHeader;
    out["badStartCode"] = _statsBadStartCode;
    out["lowerPrio"]    = _statsLowerPrio;
}
