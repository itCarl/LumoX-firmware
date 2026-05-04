#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ETH.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_dmx.h>
#include "config.h"

class Lumox {
public:
    // ── Singleton ──────────────────────────────────────────────────────────
    static Lumox& instance() {
        static Lumox inst;
        return inst;
    }
    Lumox(const Lumox&)            = delete;
    Lumox& operator=(const Lumox&) = delete;

    // ── Lifecycle (Lumox.cpp) ──────────────────────────────────────────────
    void begin();
    void loop();

    // ── Network (network.cpp) ──────────────────────────────────────────────
    void        beginNetwork();
    void        beginEthernet();
    String      networkHostname() const;   // mDNS + DHCP, MAC-derived, unique
    bool        isApMode()   const { return _apMode; }
    bool        isFallback() const { return _apFallback; }
    bool        isSta()      const { return !_apMode && WiFi.status() == WL_CONNECTED; }
    bool        isEth()      const { return _ethUp; }

    // Preference order: AP fallback → Ethernet → STA WiFi.
    // Ethernet wins over WiFi once link+DHCP are up (wired = more reliable).
    IPAddress   getIP() const {
        if (_apMode) return WiFi.softAPIP();
        if (_ethUp)  return _ethIp;
        return WiFi.localIP();
    }
    IPAddress   getEthIp()    const { return _ethUp ? _ethIp : IPAddress(); }
    IPAddress   getStaIp()    const { return isSta() ? WiFi.localIP() : IPAddress(); }
    String      getEthMac()   const;
    uint32_t    getEthSpeed() const { return _ethSpeedMbps; }    // 10 / 100 / 0
    bool        getEthFdx()   const { return _ethFullDuplex; }
    int         getRssi()     const { return _apMode ? 0 : WiFi.RSSI(); }
    // Count AP clients whenever softAP is up — including AP-aux mode where
    // _apMode is false but a phone may still be connected via the AP for config.
    int         getClients()  const { return _apActive ? WiFi.softAPgetStationNum() : 0; }

    // Called from loop() — detects link up/down, runs DHCP on plug-in,
    // maintains DHCP lease, and prints a periodic status line.
    void        loopEthernet();
    void        debugEthStatus();
    void        announceArtNetNode();   // unsolicited ArtPollReply broadcast

    // ── Art-Net (artnet.cpp) ───────────────────────────────────────────────
    // Single UDP socket binds to lwIP → receives on both WiFi + ETH netifs.
    void beginArtNet();
    bool pollArtNet();
    void sendArtPollReply(IPAddress target);
    void sendArtDiagData(IPAddress target, uint8_t priority, const char* text);
    void loopArtNet();      // periodic unsolicited broadcast, sync-mode timer

    // ── Device name (used by mDNS + ArtPollReply) ──────────────────────────
    String cfgDeviceName;

    // ── DMX output (dmx.cpp) ──────────────────────────────────────────────
    void beginDmx();
    // Caller supplies a fully-prepared 513-byte frame ([0]=start code 0x00,
    // [1..512]=channels). writeDmx hands it directly to esp_dmx — no inner
    // copy or zero — so the ~44 Hz TX path runs without extra allocation.
    void writeDmx(uint8_t* frame);

    // Mutex-guarded read of the 512 active DMX slots into out512. Use this
    // from webserver/WS code to avoid torn reads while the parser writes.
    void snapshotDmx(uint8_t* out512);

    // ── Webserver (webserver.cpp) ──────────────────────────────────────────
    void beginWebServer();
    void loopWebServer();              // call from the main loop
    String buildStatusJson();          // compact status (no DMX data)
    void   pushStateToClients();       // WebSocket broadcast

    // ── Config (Preferences / NVS) ─────────────────────────────────────────
    void loadConfig();
    void saveConfig();
    void factoryReset();

    // ── DMX buffer ─────────────────────────────────────────────────────────
    uint8_t  dmxBuffer[513]  = {0};    // [0] = start code 0x00, [1..512] = channels
    uint16_t dmxChannelCount = 512;

    // ── Manual override (control page) ─────────────────────────────────────
    // Per-channel override applied at DMX TX time when manualEnabled is set.
    // -1 = AUTO (Art-Net wins), 0..255 = manual value forced. Indexed
    // [1..512]; [0] unused. Updated by /api/manual handlers; read by
    // _dmxTask (no mutex — int16 reads are atomic on ESP32, occasional
    // torn updates are visually invisible).
    //
    // manualEnabled is the master switch — defaults OFF every boot (session-
    // only, not persisted) so Lumox always comes up as a clean Art-Net node.
    int16_t  manualValue[513];
    bool     manualEnabled = false;
    bool     manualActive  = false;        // true if any manualValue >= 0
    void     setManual(int ch, int val);   // val = -1 → release that channel
    void     clearAllManual();
    void     setManualEnabled(bool on);

    // Active-channel bitmap — bit (ch-1) is set when manualValue[ch] >= 0.
    // _dmxTask iterates only set bits via __builtin_ctz, skipping the
    // 512-channel scan when the override list is sparse (typical case).
    uint32_t manualActiveBits[16] = {0};
    uint16_t manualActiveCount    = 0;

    // ── Runtime config (loaded from NVS) ───────────────────────────────────
    // STA = normal mode (join configured WiFi).
    // AP  = automatic fallback when STA SSID empty or connect fails.
    String    cfgApSsid;
    String    cfgApPassword;
    String    cfgStaSsid;
    String    cfgStaPassword;
    uint16_t  cfgUniverse   = 0;

    // Ethernet (0.0.0.0 static fields mean "unset" — ignored unless DHCP is off)
    bool      cfgEthDhcp    = true;
    IPAddress cfgEthIp;
    IPAddress cfgEthGw;
    IPAddress cfgEthSub;
    IPAddress cfgEthDns;

    // When true + ETH up → WiFi is turned off (STA + AP). When ETH goes down,
    // WiFi is re-enabled automatically.
    bool      cfgWifiDisableOnEth = false;

    // ── Runtime stats ──────────────────────────────────────────────────────
    uint32_t  statsArtnetPackets = 0;
    uint32_t  statsArtnetLastMs  = 0;       // millis() of the last ArtDMX packet
    uint32_t  statsArtnetStale   = 0;       // packets rejected for stale sequence
    uint32_t  statsArtnetLocked  = 0;       // packets rejected — other sender had lock
    IPAddress statsArtnetSender;
    uint16_t  statsArtnetLastUni = 0;

    // Sender-swap detection — flipping between controllers mid-show causes
    // visible flicker because each sender has its own scene state.
    uint32_t  statsArtnetSenderSwaps = 0;
    uint32_t  statsArtnetLastSwapMs  = 0;

    // ArtSync (multi-node frame synchronization — Art-Net spec §10)
    uint32_t  statsArtSyncs        = 0;
    uint32_t  statsArtSyncLastMs   = 0;
    bool      statsInSyncMode      = false;   // true while ArtSync seen < 4 s ago
    uint32_t  statsArtNetBadProto  = 0;       // packets rejected for ProtVer < 14

    // DMX transmit-path health. Any of these moving fast = bad signal.
    uint32_t  statsDmxFramesSent    = 0;
    uint32_t  statsDmxSendErrors    = 0;    // dmx_send() returned 0
    uint32_t  statsDmxWaitTimeouts  = 0;    // dmx_wait_sent() returned false
    uint32_t  statsDmxConsecErrors  = 0;    // current consecutive error streak
    uint32_t  statsDmxMaxConsecErr  = 0;    // worst streak seen
    uint32_t  statsDmxLastFrameMs   = 0;
    uint32_t  statsDmxRateHz        = 0;    // measured frame rate
    uint32_t  statsDmxMaxMutexUs    = 0;    // worst mutex-take duration

    // Explicit health check — return true only if the signal is flicker-free.
    // 'reason' points to a static string describing the first failed criterion
    // (empty when ok). Cheap — just reads counters.
    struct DmxHealth { bool ok; const char* reason; };
    DmxHealth dmxHealth() const;

private:
    Lumox() = default;

    // Art-Net
    WiFiUDP   _udp;
    uint8_t   _udpBuf[600] = {0};
    uint8_t   _macCached[6] = {0};           // populated once in begin()
    uint8_t   _lastSeq     = 0;              // last accepted Art-Net sequence byte
    IPAddress _lastSeqSender;                // sender for _lastSeq (per-source tracking)
    uint16_t  _nodeReportSeq = 0;            // rolling counter in ArtPollReply node report
    uint32_t  _identifyUntilMs = 0;          // ms until identify-blink ends (0 = off)
    bool      _parseArtNet(const uint8_t* buf, int len, IPAddress sender);
    void      _handleArtAddress(const uint8_t* buf, int len, IPAddress sender);
    void      _handleArtCommand(const uint8_t* buf, int len, IPAddress sender);
    void      _handleArtPoll   (const uint8_t* buf, int len, IPAddress sender);
    void      _handleArtSync();
    bool      _dispatchArtNet(const uint8_t* buf, int len, IPAddress sender);
    void      _sendArtNetUdp(IPAddress target, uint16_t port,
                             const uint8_t* buf, size_t len);
    void      _broadcastArtPollReply(const uint8_t* buf, size_t len);

    // Shadow buffer for ArtSync mode. While in sync mode, incoming ArtDMX
    // writes to _dmxShadow; OpSync flips shadow → dmxBuffer atomically so all
    // nodes release new frame together. _dmxShadowLen tracks the highest slot
    // written so partial-universe packets don't get padded with stale bytes
    // when the shadow flips into the live buffer.
    uint8_t   _dmxShadow[513]   = {0};
    uint16_t  _dmxShadowLen     = 512;
    bool      _dmxShadowDirty   = false;

    // Last ArtPoll state — used for diagnostic routing (spec §11).
    uint8_t   _pollTalkToMe     = 0;          // byte 12 flags
    uint8_t   _pollDiagPriority = 0x10;       // byte 13 min priority
    IPAddress _pollDiagTarget;                // unicast target for diag (if reply-mode)

    // Periodic unsolicited ArtPollReply — spec recommends re-announce so
    // controllers recover from missed packets without a discovery round.
    uint32_t  _lastAnnounceMs   = 0;

    // Status LED
    void _updateLed();

    // DMX
    dmx_port_t         _dmxPort  = DMX_NUM_1;
    SemaphoreHandle_t  _dmxMutex = nullptr;
    bool               _dmxReady = false;
    static void        _dmxTask(void* param);

    // Network
    // _apMode:    AP is the SOLE network path (no ETH, no STA). Used to gate
    //             getIP() preference + captive-portal redirects when the only
    //             way to reach the node is via the AP itself.
    // _apActive:  softAP is up and serving — may coexist with ETH (AP-aux mode).
    //             Drives DNS-server tick + captive-portal lambda + ArtPollReply
    //             AP broadcast branch. Always true when _apMode is true.
    // _apFallback: AP came up because STA failed (informational — shown on UI).
    bool _apMode      = true;
    bool _apActive    = true;
    bool _apFallback  = false;

    // Ethernet (W5500 via arduino-esp32 v3.x ETH.h, lwIP-backed)
    bool      _ethHw          = false;   // ETH.begin() succeeded (chip on SPI)
    bool      _ethUp          = false;   // link UP + IP acquired (GOT_IP event)
    bool      _ethLinkUp      = false;   // CONNECTED event seen (link, pre-IP)
    IPAddress _ethIp;
    uint8_t   _ethMac[6]      = {0};
    uint32_t  _ethLastDbgMs   = 0;
    uint32_t  _ethSpeedMbps   = 0;       // ETH.linkSpeed() (10 / 100, 0 = down)
    bool      _ethFullDuplex  = false;   // ETH.fullDuplex()
    bool      _wifiOff        = false;   // true while WiFi is suspended by ETH
    void      _onEthEvent(arduino_event_id_t event, arduino_event_info_t info);
    void      _applyWifiOnEthPolicy();   // enable/disable WiFi on ETH state change

    // Webserver — single AsyncWebServer + AsyncWebSocket on port 80.
    // Both run on AsyncTCP's task, no loop tick needed for IO.
    AsyncWebServer    _http{WEB_SERVER_PORT};
    AsyncWebSocket    _ws{"/ws"};
    DNSServer         _dns;
    uint32_t          _lastWsPushMs = 0;

    void _onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                    AwsEventType type, void* arg, uint8_t* data, size_t len);
    void _registerRoutes();
};
