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
#include "const.h"   // LOG_* macros + hardware pin defs
#include "protocols/IDmxSource.h"

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
    bool        isApActive() const { return _apActive; }
    bool        isFallback() const { return _apFallback; }
    bool        isSta()      const { return !_apMode && WiFi.status() == WL_CONNECTED; }
    bool        isEth()      const { return _ethUp; }

    // Preference order: AP fallback → Ethernet → STA WiFi.
    IPAddress   getIP() const {
        if (_apMode) return WiFi.softAPIP();
        if (_ethUp)  return _ethIp;
        return WiFi.localIP();
    }
    IPAddress   getEthIp()    const { return _ethUp ? _ethIp : IPAddress(); }
    IPAddress   getStaIp()    const { return isSta() ? WiFi.localIP() : IPAddress(); }
    String      getEthMac()   const;
    uint32_t    getEthSpeed() const { return _ethSpeedMbps; }
    bool        getEthFdx()   const { return _ethFullDuplex; }
    int         getRssi()     const { return _apMode ? 0 : WiFi.RSSI(); }
    int         getClients()  const { return _apActive ? WiFi.softAPgetStationNum() : 0; }
    void        getMac(uint8_t out[6]) const { memcpy(out, _macCached, 6); }

    void        loopEthernet();
    void        debugEthStatus();
    void        announceProtocolNode();   // unsolicited PollReply / re-join multicast

    // ── DMX source (protocols/) ────────────────────────────────────────────
    // One active source at a time, selected by cfgProtocol. Created in
    // beginProtocol() — switching protocols today requires a reboot.
    void          beginProtocol();
    void          endProtocol();
    IDmxSource*   source() const { return _source; }

    // Single ingestion path — called by every source after protocol-specific
    // validation. Returns true if the frame was accepted and written to the
    // live DMX buffer; false if the sender is locked out by another active
    // primary. Updates statsArtnet* counters (the ingress stats are protocol-
    // agnostic — the field names predate the multi-protocol refactor).
    bool          feedDmxFrame(const uint8_t* slots, uint16_t len, IPAddress sender);

    // Identify-LED locate blink. Sources call this from OpCommand "Identify"
    // / OpAddress LED_LOCATE handling. ms=0 cancels.
    void          setIdentify(uint32_t durationMs);

    // Clear the live DMX buffer (set all 512 slots to 0). Called from
    // OpCommand "Clear" + OpAddress AC_CLEAR_OP0.
    void          clearDmxBuffer();

    // ── Device name (used by mDNS + ArtPollReply) ──────────────────────────
    String cfgDeviceName;

    // ── DMX output (dmx.cpp) ──────────────────────────────────────────────
    void beginDmx();
    void writeDmx(uint8_t* frame);
    void snapshotDmx(uint8_t* out512);

    // ── Webserver (webserver.cpp) ──────────────────────────────────────────
    void beginWebServer();
    void loopWebServer();
    String buildStatusJson();
    void   pushStateToClients();

    // ── Config (Preferences / NVS) ─────────────────────────────────────────
    void loadConfig();
    void saveConfig();
    void factoryReset();

    // ── DMX buffer ─────────────────────────────────────────────────────────
    uint8_t  dmxBuffer[513]  = {0};
    uint16_t dmxChannelCount = 512;

    // ── Manual override (control page) ─────────────────────────────────────
    int16_t  manualValue[513];
    bool     manualEnabled = false;
    bool     manualActive  = false;
    void     setManual(int ch, int val);
    void     clearAllManual();
    void     setManualEnabled(bool on);

    uint32_t manualActiveBits[16] = {0};
    uint16_t manualActiveCount    = 0;

    // ── Runtime config (loaded from NVS) ───────────────────────────────────
    String       cfgApSsid;
    String       cfgApPassword;
    String       cfgStaSsid;
    String       cfgStaPassword;
    uint16_t     cfgUniverse = 0;
    ProtocolType cfgProtocol = ProtocolType::ArtNet;

    // ── Network DMX input options ──────────────────────────────────────────
    // E1.31 multicast — when false, the source binds unicast only (no IGMP
    // join). Default OFF: most home/lab WiFi APs handle multicast badly, and
    // unicast just works. Enable when an IGMP-snooping switch is in play and
    // multiple receivers need the same stream.
    bool      cfgE131Multicast   = false;

    // Sequence-skip — drop strictly-older sequence numbers from the same
    // sender. Default on (matches spec hardening). Turn off if a buggy sender
    // re-uses sequence bytes and most frames look stale to us.
    bool      cfgSkipStaleSeq    = true;

    // E1.31 minimum-priority filter (0..200). Frames below this priority are
    // dropped. 0 = accept everything. Used to ignore preview / low-prio test
    // streams while honouring the active high-prio show.
    uint8_t   cfgE131MinPriority = 0;

    // Hold-last-frame timeout in ms. After this many ms of no input, the DMX
    // task zeroes the outgoing frame (effectively blackout) until input
    // resumes. 0 = hold the last frame forever. Default 5000 = ~5 s grace
    // for a console hiccup, then blackout so the show doesn't hold a stale
    // cue indefinitely.
    uint32_t  cfgHoldTimeoutMs   = 5000;

    // Ethernet
    bool      cfgEthDhcp    = true;
    IPAddress cfgEthIp;
    IPAddress cfgEthGw;
    IPAddress cfgEthSub;
    IPAddress cfgEthDns;

    bool      cfgWifiDisableOnEth = false;

#if LUMOX_CH1_PIN_NONZERO
    bool      cfgCh1PinNonzero = DEFAULT_CH1_PIN_NONZERO;
#endif

    // ── Runtime stats ──────────────────────────────────────────────────────
    // statsArtnet* are protocol-agnostic ingress counters now — the prefix
    // is historical. Updated by feedDmxFrame() regardless of source type.
    uint32_t  statsArtnetPackets = 0;
    uint32_t  statsArtnetLastMs  = 0;
    uint32_t  statsArtnetStale   = 0;
    uint32_t  statsArtnetLocked  = 0;
    IPAddress statsArtnetSender;
    uint16_t  statsArtnetLastUni = 0;

    uint32_t  statsArtnetSenderSwaps = 0;
    uint32_t  statsArtnetLastSwapMs  = 0;

    // DMX-TX health
    uint32_t  statsDmxFramesSent    = 0;
    uint32_t  statsDmxSendErrors    = 0;
    uint32_t  statsDmxWaitTimeouts  = 0;
    uint32_t  statsDmxConsecErrors  = 0;
    uint32_t  statsDmxMaxConsecErr  = 0;
    uint32_t  statsDmxLastFrameMs   = 0;
    uint32_t  statsDmxRateHz        = 0;
    uint32_t  statsDmxRateTenths    = 0;
    uint32_t  statsDmxMaxMutexUs    = 0;

    struct DmxHealth { bool ok; const char* reason; };
    DmxHealth dmxHealth() const;

    // Mutex-spike ring buffer (debug aid)
    static constexpr uint8_t  MUTEX_LOG_SIZE         = 25;
    static constexpr uint32_t MUTEX_LOG_THRESHOLD_US = 1000;
    static constexpr uint32_t MUTEX_DECAY_MS         = 60000;
    struct MutexLogEntry { uint32_t timeMs; uint32_t durationUs; };
    MutexLogEntry _mutexLog[MUTEX_LOG_SIZE] = {};
    uint8_t       _mutexLogHead  = 0;
    uint8_t       _mutexLogCount = 0;
    uint32_t      _mutexDecayMs  = 0;

private:
    Lumox() = default;

    // Active DMX source — owned (heap), created in beginProtocol().
    IDmxSource* _source = nullptr;

    uint8_t   _macCached[6]   = {0};       // populated once in begin()
    uint32_t  _identifyUntilMs = 0;        // ms until identify-blink ends

    // Status LED
    void _updateLed();

    // DMX
    dmx_port_t         _dmxPort  = DMX_NUM_1;
    portMUX_TYPE       _dmxLock  = portMUX_INITIALIZER_UNLOCKED;
    bool               _dmxReady = false;
    static void        _dmxTask(void* param);

    // Protocol RX task — replaces the old _artNetTaskWrap. Calls _source->service()
    // in a tight cycle so HTTP / OTA can't delay packet pickup.
    TaskHandle_t       _protoTaskH = nullptr;
    volatile bool      _pendingNetEvent = false;
    static void        _protocolTaskWrap(void* param);

    // Network
    bool _apMode      = true;
    bool _apActive    = true;
    bool _apFallback  = false;

    // Ethernet (W5500 via arduino-esp32 v3.x ETH.h, lwIP-backed)
    bool      _ethHw          = false;
    bool      _ethUp          = false;
    bool      _ethLinkUp      = false;
    IPAddress _ethIp;
    uint8_t   _ethMac[6]      = {0};
    uint32_t  _ethLastDbgMs   = 0;
    uint32_t  _ethSpeedMbps   = 0;
    bool      _ethFullDuplex  = false;
    bool      _wifiOff        = false;
    void      _onEthEvent(arduino_event_id_t event, arduino_event_info_t info);
    void      _applyWifiOnEthPolicy();

    // Webserver
    AsyncWebServer    _http{WEB_SERVER_PORT};
    AsyncWebSocket    _ws{"/ws"};
    DNSServer         _dns;
    uint32_t          _lastWsPushMs = 0;

    void _onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                    AwsEventType type, void* arg, uint8_t* data, size_t len);
    void _registerRoutes();
};
