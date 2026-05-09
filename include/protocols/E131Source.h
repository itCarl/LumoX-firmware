#pragma once

#include "IDmxSource.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <IPAddress.h>

// sACN E1.31 source — listens on UDP 5568. Joins the multicast group
// 239.255.<hi>.<lo> for the configured universe (most consoles multicast),
// also accepts unicast on the same socket (gateways that direct-send).
//
// Priority arbitration: byte 108 (0..200, default 100). While a higher-prio
// source is live, lower-prio frames are dropped. Stale window mirrors
// DMX_LINK_STALE_MS — same logic as Art-Net sender lock.
class E131Source : public IDmxSource {
public:
    E131Source() = default;

    ProtocolType type()  const override { return ProtocolType::E131; }
    const char*  name()  const override { return "e131"; }

    bool begin()   override;
    void end()     override;
    void service() override;
    void onNetworkChange() override;
    void httpStatus(JsonObject& out) override;

private:
    WiFiUDP   _udp;
    uint8_t   _udpBuf[638] = {0};   // E1.31 max frame ≈ 638 bytes
    uint8_t   _lastSeq      = 0;
    IPAddress _lastSeqSender;
    uint8_t   _activePriority = 0;
    uint32_t  _lastPrioMs     = 0;
    bool      _multicastJoined = false;

    // Stats (E1.31 specific)
    uint32_t  _statsBadHeader    = 0;
    uint32_t  _statsBadStartCode = 0;
    uint32_t  _statsLowerPrio    = 0;

    bool _dispatch(const uint8_t* buf, int len, IPAddress sender);
    bool _joinMulticast();
};
