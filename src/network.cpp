#include "Lumox.h"
#include "config.h"
#include "const.h"

#include <SPI.h>
#include <esp_mac.h>

// ── Config persistence via Preferences (NVS) ────────────────────────────────
static const char* PREF_NAMESPACE = "lumox";

static String macSuffix() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[8];
    snprintf(buf, sizeof(buf), "-%02X%02X", mac[4], mac[5]);
    return String(buf);
}

// Network hostname (mDNS + DHCP). Always derived from MAC, NEVER from the
// user-editable cfgDeviceName — guarantees uniqueness when several nodes
// share the same friendly name ("Stage Left" on multiple boards). Lower-case
// per RFC 1035; only chars valid in DNS labels.
String Lumox::networkHostname() const {
    String s = String("lumox") + macSuffix();   // e.g. "lumox-A1B2"
    s.toLowerCase();
    return s;
}

void Lumox::loadConfig() {
    Preferences p;
    p.begin(PREF_NAMESPACE, /*readOnly*/ true);

    cfgApSsid      = p.getString("apSsid",   String(DEFAULT_AP_SSID) + macSuffix());
    cfgApPassword  = p.getString("apPass",   DEFAULT_AP_PASSWORD);
    cfgStaSsid     = p.getString("staSsid",  DEFAULT_STA_SSID);
    cfgStaPassword = p.getString("staPass",  DEFAULT_STA_PASSWORD);
    cfgUniverse    = p.getUShort("universe", DEFAULT_ARTNET_UNIVERSE);
    cfgDeviceName  = p.getString("devName",  String(DEFAULT_DEVICE_NAME) + macSuffix());

    // Pre-parse string defaults from config.h once so each Preferences fallback
    // gets a 32-bit IP value rather than the literal 0 → field would otherwise
    // appear blank on first boot.
    auto ipDefault = [](const char* lit) {
        IPAddress ip;
        ip.fromString(lit);
        return (uint32_t)ip;
    };
    cfgEthDhcp     = p.getBool  ("ethDhcp",  DEFAULT_ETH_DHCP);
    cfgEthIp       = IPAddress(p.getUInt("ethIp",  ipDefault(DEFAULT_ETH_IP)));
    cfgEthGw       = IPAddress(p.getUInt("ethGw",  ipDefault(DEFAULT_ETH_GW)));
    cfgEthSub      = IPAddress(p.getUInt("ethSub", ipDefault(DEFAULT_ETH_SUB)));
    cfgEthDns      = IPAddress(p.getUInt("ethDns", ipDefault(DEFAULT_ETH_DNS)));

    cfgWifiDisableOnEth = p.getBool("wifiOffEth", DEFAULT_WIFI_OFF_ON_ETH);

    p.end();

    Serial.printf("[Config] name=\"%s\"  universe=%u  staSsid=\"%s\"  ethDhcp=%d\n",
                  cfgDeviceName.c_str(), cfgUniverse, cfgStaSsid.c_str(), cfgEthDhcp);
}

void Lumox::saveConfig() {
    Preferences p;
    p.begin(PREF_NAMESPACE, /*readOnly*/ false);

    p.putString("apSsid",   cfgApSsid);
    p.putString("apPass",   cfgApPassword);
    p.putString("staSsid",  cfgStaSsid);
    p.putString("staPass",  cfgStaPassword);
    p.putUShort("universe", cfgUniverse);
    p.putString("devName",  cfgDeviceName);

    p.putBool  ("ethDhcp",  cfgEthDhcp);
    p.putUInt  ("ethIp",    (uint32_t)cfgEthIp);
    p.putUInt  ("ethGw",    (uint32_t)cfgEthGw);
    p.putUInt  ("ethSub",   (uint32_t)cfgEthSub);
    p.putUInt  ("ethDns",   (uint32_t)cfgEthDns);
    p.putBool  ("wifiOffEth", cfgWifiDisableOnEth);

    p.end();
    Serial.println("[Config] Saved to NVS.");
}

void Lumox::factoryReset() {
    Preferences p;
    p.begin(PREF_NAMESPACE, /*readOnly*/ false);
    p.clear();
    p.end();
    Serial.println("[Config] Factory reset — rebooting.");
    delay(200);
    ESP.restart();
}

// ── Register mDNS with Lumox service ────────────────────────────────────────
// Hostname comes from networkHostname() (MAC-derived, always unique). The
// human-friendly cfgDeviceName is published as the "name" TXT record — not
// used for the DNS label.
static void registerMdns(const String& mdnsName, uint16_t universe, const String& deviceName) {
    MDNS.begin(mdnsName.c_str());
    MDNS.addService("http",  "tcp", WEB_SERVER_PORT);
    MDNS.addService("lumox", "tcp", WEB_SERVER_PORT);

    MDNS.addServiceTxt("lumox", "tcp", "name",     deviceName.c_str());
    MDNS.addServiceTxt("lumox", "tcp", "universe", String(universe).c_str());

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    MDNS.addServiceTxt("lumox", "tcp", "mac", (const char*)macStr);

    Serial.printf("[mDNS] %s.local  _lumox._tcp  (name=\"%s\" universe=%u)\n",
                  mdnsName.c_str(), deviceName.c_str(), universe);
}

// ── Start network: ETH preferred → STA → AP (always when STA down) ─────────
// Called AFTER beginEthernet(), so _ethUp already reflects wired state.
//
// Rule: AP starts whenever STA is not up. With ETH up + STA down, AP runs as
// an aux interface so a phone can still reach /config without unplugging the
// cable (quick on-site reconfig). AP is "primary" (_apMode=true) only when
// nothing else is up. cfgWifiDisableOnEth still wins — explicit user opt-out.
void Lumox::beginNetwork() {
    _apMode     = false;
    _apActive   = false;
    _apFallback = false;

    // Policy: "disable WiFi when ETH is connected" — skip WiFi bring-up
    // entirely. mDNS still works on ETH (lwIP single stack).
    if (cfgWifiDisableOnEth && _ethUp) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        _wifiOff = true;
        Serial.println("[Network] ETH up + 'disable WiFi on ETH' set — WiFi suspended");
        return;
    }
    _wifiOff = false;

    // If ETH is already the primary interface, shorten the STA connect timeout
    // — we don't want to block boot 15 s on WiFi when Art-Net is already
    // flowing over Ethernet. STA still runs as a backup path.
    const uint32_t staTimeout = _ethUp ? 5000 : STA_CONNECT_TIMEOUT_MS;

    bool staConnected = false;
    if (cfgStaSsid.length() > 0) {
        Serial.printf("[Network] Connecting to \"%s\" (timeout %u ms, ETH %s)...",
                      cfgStaSsid.c_str(), staTimeout, _ethUp ? "up" : "down");
        WiFi.mode(WIFI_STA);
        WiFi.setHostname(networkHostname().c_str());
        // Disable modem sleep — Art-Net is latency-sensitive (~22 ms/frame).
        // With sleep on, UDP packets see 100+ ms jitter → EFX stutter on WiFi.
        WiFi.setSleep(false);
        WiFi.begin(cfgStaSsid.c_str(), cfgStaPassword.c_str());

        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < staTimeout) {
            delay(500);
            Serial.print(".");
        }
        Serial.println();

        staConnected = (WiFi.status() == WL_CONNECTED);
        if (staConnected) {
            Serial.printf("[Network] STA connected  IP=%s  RSSI=%d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
        } else {
            Serial.println("[Network] STA failed.");
        }
    } else {
        Serial.println("[Network] No STA SSID configured.");
    }

    if (staConnected) {
        // STA up → no AP needed. ETH (if up) carries Art-Net; STA serves
        // browser config + mDNS discovery from the WiFi side.
        registerMdns(networkHostname(), cfgUniverse, cfgDeviceName);
        Serial.printf("[Network] Active: %s%sSTA  primary=%s\n",
                      _ethUp ? "ETH+" : "",
                      "",
                      _ethUp ? "ETH" : "STA");
        return;
    }

    // STA down — open AP. Two flavours:
    //   • ETH up   → AP runs *aux* to ETH (quick on-site reconfig path).
    //                getIP() still returns ETH IP, ArtPollReply still
    //                advertises ETH, captive portal works for AP clients.
    //   • ETH down → AP is the SOLE path (_apMode=true) → captive portal +
    //                getIP returns softAPIP for redirects.
    Serial.printf("[Network] STA down — opening AP (%s).\n",
                  _ethUp ? "aux to ETH" : "primary fallback");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfgApSsid.c_str(), cfgApPassword.c_str());
    Serial.printf("[Network] AP started  SSID=\"%s\"  IP=%s\n",
                  cfgApSsid.c_str(),
                  WiFi.softAPIP().toString().c_str());

    _apActive   = true;
    _apMode     = !_ethUp;       // primary only when nothing else is up
    _apFallback = !_ethUp;       // "fallback" only if ETH isn't carrying traffic

    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", WiFi.softAPIP());

    registerMdns(networkHostname(), cfgUniverse, cfgDeviceName);
}

// ── W5500 Ethernet (arduino-esp32 v3.x ETH.h, lwIP-backed) ─────────────────
// Single TCP/IP stack with WiFi → AsyncWebServer + Art-Net UDP work on both
// interfaces from one socket. Driver is event-driven (CONNECTED / GOT_IP /
// DISCONNECTED via WiFi.onEvent), no polling-loop / DHCP-maintain calls.

void Lumox::beginEthernet() {
    // Derive a stable MAC from the ESP32 ETH efuse slot (W5500 has no burned-in MAC).
    esp_read_mac(_ethMac, ESP_MAC_ETH);
    Serial.printf("[ETH] Init W5500  CS=%d RST=%d  MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  ETH_CS_PIN, ETH_RST_PIN,
                  _ethMac[0], _ethMac[1], _ethMac[2],
                  _ethMac[3], _ethMac[4], _ethMac[5]);

    // Hardware reset pulse — some W5500 breakouts need an explicit LOW pulse
    // before SPI becomes responsive.
    pinMode(ETH_RST_PIN, OUTPUT);
    digitalWrite(ETH_RST_PIN, LOW);
    delay(20);
    digitalWrite(ETH_RST_PIN, HIGH);
    delay(200);          // W5500 datasheet: ~50 ms, be generous

    // Subscribe to ETH lifecycle events before begin() so we don't miss the
    // first CONNECTED / GOT_IP. Lambda forwards into the singleton instance.
    WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
        Lumox::instance()._onEthEvent(event, info);
    });

    // arduino-esp32 v3.x: ETH.begin(phy_type, phy_addr, cs, irq, rst,
    //                                spi_host, sck, miso, mosi).
    //
    // IRQ mode requires the W5500 INT line to idle HIGH — i.e. a pullup. ESP32
    // GPIO34 (our default ETH_INT_PIN) is input-only and has no internal
    // pullup, so without an external 10k pullup the line floats and link
    // events never fire. Polling mode (irq = -1) is safe in all cases — driver
    // periodically reads the PHY status register instead.
    //
    // ETH_USE_IRQ (config.h, compile-time): set to 1 only if INT pin has a
    // pullup wired. Don't pre-call SPI.begin() — ETH driver inits SPI itself.
#if ETH_USE_IRQ
    const int irq = ETH_INT_PIN;
    Serial.println("[ETH] IRQ mode: INT pin (build-time)");
#else
    const int irq = -1;
    Serial.println("[ETH] IRQ mode: polling (build-time)");
#endif
    const bool ok = ETH.begin(ETH_PHY_W5500, LUMOX_ETH_PHY_ADDR,
                              ETH_CS_PIN, irq, ETH_RST_PIN,
                              SPI3_HOST,
                              ETH_SCK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN,
                              ETH_SPI_CLK_MHZ);
    _ethHw = ok;
    if (!ok) {
        Serial.println("[ETH] ETH.begin() failed — check SPI wiring + 3V3/GND + RST pin.");
        return;
    }

    // Set hostname before DHCP so the lease appears as "lumox-xxyy" on the
    // router. MAC-derived → always unique even when several boards share the
    // same human-friendly cfgDeviceName.
    ETH.setHostname(networkHostname().c_str());

    // Static config: ETH.config() before DHCP attempt skips DHCP altogether.
    if (!cfgEthDhcp && (uint32_t)cfgEthIp != 0) {
        Serial.printf("[ETH] STATIC  ip=%s gw=%s sub=%s dns=%s\n",
                      cfgEthIp.toString().c_str(),
                      cfgEthGw.toString().c_str(),
                      cfgEthSub.toString().c_str(),
                      cfgEthDns.toString().c_str());
        ETH.config(cfgEthIp, cfgEthGw, cfgEthSub, cfgEthDns);
    } else if (!cfgEthDhcp) {
        Serial.println("[ETH] STATIC mode enabled but no IP set — DHCP will run instead.");
    } else {
        Serial.println("[ETH] Using DHCP");
    }

    // Wait briefly for link + IP at boot so beginNetwork() can shorten the
    // STA timeout when ETH is already up. State machine continues async via
    // _onEthEvent() if the cable is unplugged at boot.
    const uint32_t start = millis();
    while (!_ethUp && millis() - start < ETH_LINK_WAIT_MS) {
        delay(50);
    }
    if (!_ethUp) {
        Serial.println("[ETH] No link/IP yet — will continue async on plug-in.");
    }
}

// ETH state machine — called from WiFi.onEvent() lambda. lwIP gives us four
// transitions of interest; everything else is logged through CORE_DEBUG.
void Lumox::_onEthEvent(arduino_event_id_t event, arduino_event_info_t /*info*/) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            Serial.println("[ETH] driver started");
            break;

        case ARDUINO_EVENT_ETH_CONNECTED:
            _ethLinkUp     = true;
            _ethSpeedMbps  = ETH.linkSpeed();
            _ethFullDuplex = ETH.fullDuplex();
            Serial.printf("[ETH] link UP  (%u Mbps %s-duplex)\n",
                          _ethSpeedMbps,
                          _ethFullDuplex ? "full" : "half");
            break;

        case ARDUINO_EVENT_ETH_GOT_IP: {
            _ethIp = ETH.localIP();
            _ethUp = true;
            Serial.printf("[ETH] UP  IP=%s  GW=%s  SUB=%s  DNS=%s\n",
                          _ethIp.toString().c_str(),
                          ETH.gatewayIP().toString().c_str(),
                          ETH.subnetMask().toString().c_str(),
                          ETH.dnsIP().toString().c_str());
            _applyWifiOnEthPolicy();
            announceArtNetNode();
            break;
        }

        case ARDUINO_EVENT_ETH_LOST_IP:
            Serial.println("[ETH] lost IP");
            _ethUp = false;
            announceArtNetNode();
            _applyWifiOnEthPolicy();
            break;

        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("[ETH] link DOWN");
            _ethUp         = false;
            _ethLinkUp     = false;
            _ethSpeedMbps  = 0;
            _ethFullDuplex = false;
            _applyWifiOnEthPolicy();
            announceArtNetNode();
            break;

        case ARDUINO_EVENT_ETH_STOP:
            Serial.println("[ETH] driver stopped");
            _ethUp = false;
            _ethLinkUp = false;
            break;

        default:
            break;
    }
}

// Kept as a thin compat shim — driver is event-driven, no per-tick work needed.
// Called from loop(); cheap no-op so we don't have to ifdef the call site.
void Lumox::loopEthernet() {
    // intentionally empty — see _onEthEvent()
}

// Enforce cfgWifiDisableOnEth. Transitions:
//   _wifiOff=false, policy wants off → disconnect + WIFI_OFF
//   _wifiOff=true,  policy wants on  → WIFI_STA/AP re-bring-up via beginNetwork()
void Lumox::_applyWifiOnEthPolicy() {
    const bool wantOff = cfgWifiDisableOnEth && _ethUp;

    if (wantOff && !_wifiOff) {
        Serial.println("[WiFi] Disabling — ETH connected + policy active");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        _wifiOff  = true;
        _apMode   = false;
        _apActive = false;
        _dns.stop();
        MDNS.end();
    }
    else if (!wantOff && _wifiOff) {
        Serial.println("[WiFi] Re-enabling — ETH gone or policy cleared");
        _wifiOff = false;
        beginNetwork();   // re-runs STA/AP + mDNS
    }
}

// Periodic serial dump — called from Lumox::loop() via debugEthStatus().
// Compiled out in production (LUMOX_DEBUG=0) — used for bench debugging only.
void Lumox::debugEthStatus() {
#if LUMOX_DEBUG
    if (_ethUp) {
        _ethSpeedMbps  = ETH.linkSpeed();
        _ethFullDuplex = ETH.fullDuplex();
    }
    DEBUG_PRINTF("[ETH] status: hw=%s  link=%s  ip=%s  phy=%uMbps/%s\n",
          _ethHw     ? "OK"  : "NO-HARDWARE",
          _ethLinkUp ? "UP"  : "DOWN",
          _ethUp     ? _ethIp.toString().c_str() : "—",
          _ethSpeedMbps,
          _ethFullDuplex ? "FDX" : "HDX");

    if (!_ethHw) {
        DEBUG_PRINTLN("[ETH]   driver not started — wiring (MOSI/MISO/SCK/CS), power, RST?");
    } else if (!_ethLinkUp) {
        DEBUG_PRINTLN("[ETH]   chip OK but no link — cable unplugged, switch port dead, or MDI mismatch?");
    } else if (_ethLinkUp && !_ethUp) {
        DEBUG_PRINTLN("[ETH]   link UP but no IP — DHCP server not responding, or static IP misconfigured?");
    }
#endif
}

// ── Unsolicited ArtPollReply broadcast ────────────────────────────────────
// Art-Net spec: nodes MAY send unsolicited ArtPollReply on state change so
// controllers update their node list without an explicit discovery round.
void Lumox::announceArtNetNode() {
    sendArtPollReply(IPAddress(255, 255, 255, 255));
    Serial.printf("[ArtNet] Unsolicited ArtPollReply broadcast — reporting IP %s\n",
                  getIP().toString().c_str());
}

String Lumox::getEthMac() const {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             _ethMac[0], _ethMac[1], _ethMac[2],
             _ethMac[3], _ethMac[4], _ethMac[5]);
    return String(buf);
}