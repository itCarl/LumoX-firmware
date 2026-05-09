#pragma once

#include "IDmxSource.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <IPAddress.h>

// Full Art-Net 4 node — replaces the old Lumox::*ArtNet methods. Owns its own
// UDP socket on 6454, bound INADDR_ANY so it serves both WiFi + ETH netifs
// from a single bind. All protocol-specific state (sync shadow, NodeReport
// counter, OpPoll subscription flags) lives here.
class ArtNetSource : public IDmxSource {
public:
    ArtNetSource() = default;

    ProtocolType type()  const override { return ProtocolType::ArtNet; }
    const char*  name()  const override { return "artnet"; }

    bool begin()   override;
    void end()     override;
    void service() override;
    void onNetworkChange() override;
    void httpStatus(JsonObject& out) override;

    // Public so Lumox::announceProtocolNode() can trigger an unsolicited reply
    // from the protocol task (cross-task UDP TX is single-threaded that way).
    void sendArtPollReply(IPAddress target);

private:
    WiFiUDP   _udp;
    uint8_t   _udpBuf[600] = {0};
    uint8_t   _lastSeq     = 0;
    IPAddress _lastSeqSender;
    uint16_t  _nodeReportSeq = 0;

    // ArtSync shadow — written while in sync mode, flipped to the live buffer
    // (via Lumox::feedDmxFrame) on OpSync arrival. Owned here, not in Lumox,
    // because ArtSync is Art-Net-specific.
    uint8_t   _shadow[513]    = {0};
    uint16_t  _shadowLen      = 512;
    bool      _shadowDirty    = false;
    IPAddress _shadowSender;

    // OpPoll subscription state for ArtDiagData routing.
    uint8_t   _pollTalkToMe     = 0;
    uint8_t   _pollDiagPriority = 0x10;
    IPAddress _pollDiagTarget;

    uint32_t  _lastAnnounceMs = 0;

    // Per-source stats (Art-Net specific) — common ingress stats live on
    // Lumox so they survive a protocol switch in shape if not in count.
    uint32_t  _statsBadProto    = 0;
    uint32_t  _statsArtSyncs    = 0;
    uint32_t  _statsSyncLastMs  = 0;
    bool      _inSyncMode       = false;

    // Packet handlers
    bool _dispatch(const uint8_t* buf, int len, IPAddress sender);
    bool _parseDmx(const uint8_t* buf, int len, IPAddress sender);
    void _handlePoll   (const uint8_t* buf, int len, IPAddress sender);
    void _handleSync   ();
    void _handleAddress(const uint8_t* buf, int len, IPAddress sender);
    void _handleCommand(const uint8_t* buf, int len, IPAddress sender);

    // Send helpers
    void _sendUdp(IPAddress target, uint16_t port, const uint8_t* buf, size_t len);
    void _broadcastReply(const uint8_t* buf, size_t len);
    void _sendDiagData(IPAddress target, uint8_t priority, const char* text);

    // Periodic housekeeping
    void _tick();
};
