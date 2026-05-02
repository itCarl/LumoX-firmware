#include "Lumox.h"
#include "config.h"
#include "const.h"

#include <SPI.h>
#include <esp_mac.h>
#include <utility/w5100.h>   // W5500 singleton exposes readPHYCFGR_W5500()

// ── Config persistence via Preferences (NVS) ────────────────────────────────
static const char* PREF_NAMESPACE = "lumox";

static String macSuffix() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[8];
    snprintf(buf, sizeof(buf), "-%02X%02X", mac[4], mac[5]);
    return String(buf);
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
static void registerMdns(const String& hostname, uint16_t universe, const String& deviceName) {
    String mdnsName = hostname;
    mdnsName.toLowerCase();
    mdnsName.replace(' ', '-');

    MDNS.begin(mdnsName.c_str());
    MDNS.addService("http",  "tcp", WEB_SERVER_PORT);
    MDNS.addService("ws",    "tcp", WS_PORT);
    MDNS.addService("lumox", "tcp", WEB_SERVER_PORT);

    MDNS.addServiceTxt("lumox", "tcp", "name",     deviceName.c_str());
    MDNS.addServiceTxt("lumox", "tcp", "universe", String(universe).c_str());

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    MDNS.addServiceTxt("lumox", "tcp", "mac", (const char*)macStr);

    Serial.printf("[mDNS] %s.local  _lumox._tcp  (universe=%u)\n",
                  mdnsName.c_str(), universe);
}

// ── Start network: ETH preferred → STA backup → AP fallback ────────────────
// Called AFTER beginEthernet(), so _ethUp already reflects wired state.
// Rule: any "real" interface (ETH or STA) skips AP fallback. ETH wins over
// STA in getIP() preference, so when ETH is up, STA becomes pure backup.
void Lumox::beginNetwork() {
    _apMode     = true;
    _apFallback = false;

    // Policy: "disable WiFi when ETH is connected" — skip WiFi bring-up
    // entirely. mDNS is not available (Ethernet lib has its own stack).
    if (cfgWifiDisableOnEth && _ethUp) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        _wifiOff = true;
        _apMode  = false;
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
        WiFi.setHostname(cfgDeviceName.c_str());
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

    // ETH or STA up = real network present, no AP captive portal needed.
    if (_ethUp || staConnected) {
        _apMode = false;
        registerMdns(cfgDeviceName, cfgUniverse, cfgDeviceName);
        Serial.printf("[Network] Active: %s%s%s  primary=%s\n",
                      _ethUp       ? "ETH" : "",
                      (_ethUp && staConnected) ? "+" : "",
                      staConnected ? "STA" : "",
                      _ethUp ? "ETH" : "STA");
        return;
    }

    Serial.println("[Network] No ETH or STA — falling back to AP + captive portal.");
    _apFallback = true;
    WiFi.disconnect(true);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(cfgApSsid.c_str(), cfgApPassword.c_str());
    Serial.printf("[Network] AP started  SSID=\"%s\"  IP=%s\n",
                  cfgApSsid.c_str(),
                  WiFi.softAPIP().toString().c_str());

    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", WiFi.softAPIP());

    registerMdns(cfgDeviceName, cfgUniverse, cfgDeviceName);
}

// ── W5500 Ethernet (arduino-libraries/Ethernet) ────────────────────────────
// Runs independently of WiFi. Shares nothing with lwIP — has its own TCP/IP
// stack on the W5500 chip. We bring it up *after* WiFi so the WiFi boot path
// isn't blocked by DHCP timeouts when the cable is unplugged.

static const char* hwStatusName(EthernetHardwareStatus s) {
    switch (s) {
        case EthernetNoHardware: return "NO-HARDWARE";
        case EthernetW5100:      return "W5100";
        case EthernetW5200:      return "W5200";
        case EthernetW5500:      return "W5500";
        default:                 return "?";
    }
}

static const char* linkStatusName(EthernetLinkStatus s) {
    switch (s) {
        case LinkON:  return "UP";
        case LinkOFF: return "DOWN";
        default:      return "UNKNOWN";
    }
}

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

    // Point the Ethernet library at our CS pin. SPI bus uses default VSPI
    // pins on ESP32 (SCK=18, MISO=19, MOSI=23) — matches ETH_*_PIN macros.
    Ethernet.init(ETH_CS_PIN);

    // Initial begin() probes the chip + acquires IP. Short DHCP timeout — if
    // the cable is unplugged at boot, loopEthernet() picks it up later on
    // plug-in. Static config skips DHCP entirely (near-instant bring-up).
    int dhcpOk = 0;
    if (cfgEthDhcp) {
        Serial.println("[ETH] Using DHCP");
        dhcpOk = Ethernet.begin(_ethMac, /*dhcp*/ 5000, /*resp*/ 2000);
    } else if ((uint32_t)cfgEthIp != 0) {
        Serial.printf("[ETH] Using STATIC  ip=%s gw=%s sub=%s dns=%s\n",
                      cfgEthIp.toString().c_str(),
                      cfgEthGw.toString().c_str(),
                      cfgEthSub.toString().c_str(),
                      cfgEthDns.toString().c_str());
        Ethernet.begin(_ethMac, cfgEthIp, cfgEthDns, cfgEthGw, cfgEthSub);
        dhcpOk = 1;   // "success" — static has no async step
    } else {
        Serial.println("[ETH] STATIC mode enabled but no IP set — skipping bring-up.");
    }

    const EthernetHardwareStatus hw = Ethernet.hardwareStatus();
    _ethHw = (hw == EthernetW5500 || hw == EthernetW5200 || hw == EthernetW5100);

    Serial.printf("[ETH] probe: hardware=%s  link=%s  result=%s\n",
                  hwStatusName(hw),
                  linkStatusName(Ethernet.linkStatus()),
                  dhcpOk ? "OK" : "failed/no-link");

    if (!_ethHw) {
        Serial.println("[ETH] W5500 not detected — check SPI wiring + 3V3/GND + RST pin.");
        return;
    }

    if (dhcpOk) {
        _ethIp = Ethernet.localIP();
        _ethUp = true;
        _ethLinkUp = true;
        _ethUdp.begin(ARTNET_UDP_PORT);
        _refreshEthPhy();
        Serial.printf("[ETH] UP  IP=%s  GW=%s  SUB=%s  DNS=%s  (%u Mbps %s-duplex)\n",
                      _ethIp.toString().c_str(),
                      Ethernet.gatewayIP().toString().c_str(),
                      Ethernet.subnetMask().toString().c_str(),
                      Ethernet.dnsServerIP().toString().c_str(),
                      _ethSpeedMbps,
                      _ethFullDuplex ? "full" : "half");
    } else {
        Serial.println("[ETH] DHCP not acquired at boot — will retry on link-up event.");
    }
    _ethLastCheckMs = millis();
}

// Called from loop() every tick. Detects link plug/unplug, retries DHCP, and
// services the Ethernet library's internal DHCP-renewal + socket housekeeping.
void Lumox::loopEthernet() {
    if (!_ethHw) return;                      // no point polling if no chip

    const uint32_t now = millis();
    if (now - _ethLastCheckMs < 500) return;  // 2 Hz is enough
    _ethLastCheckMs = now;

    const EthernetLinkStatus ls = Ethernet.linkStatus();
    // Tri-state: LinkON / LinkOFF / Unknown. Treat Unknown as "no change" —
    // a noisy SPI read can return Unknown momentarily and flipping _ethUp
    // false on it would tear down the working interface for no reason.
    const bool linkUp   = (ls == LinkON);
    const bool linkDown = (ls == LinkOFF);
    if (linkUp || linkDown) _ethLinkUp = linkUp;

    if (linkUp && !_ethUp) {
        // Link just came up (or first time after boot with cable plugged).
        bool ok;
        if (cfgEthDhcp) {
            Serial.println("[ETH] link UP detected — requesting DHCP...");
            ok = Ethernet.begin(_ethMac, /*dhcp*/ 6000, /*resp*/ 2000) != 0;
        } else if ((uint32_t)cfgEthIp != 0) {
            Serial.printf("[ETH] link UP detected — applying STATIC %s\n",
                          cfgEthIp.toString().c_str());
            Ethernet.begin(_ethMac, cfgEthIp, cfgEthDns, cfgEthGw, cfgEthSub);
            ok = true;
        } else {
            Serial.println("[ETH] link UP but STATIC IP unset — skipping");
            ok = false;
        }
        if (ok) {
            _ethIp = Ethernet.localIP();
            _ethUp = true;
            _ethUdp.stop();                   // re-open UDP on new interface
            _ethUdp.begin(ARTNET_UDP_PORT);
            _refreshEthPhy();
            Serial.printf("[ETH] UP (%s)  IP=%s  GW=%s  (%u Mbps %s-duplex)\n",
                          cfgEthDhcp ? "DHCP" : "STATIC",
                          _ethIp.toString().c_str(),
                          Ethernet.gatewayIP().toString().c_str(),
                          _ethSpeedMbps,
                          _ethFullDuplex ? "full" : "half");
            announceArtNetNode();             // tell controllers we moved
        } else if (cfgEthDhcp) {
            Serial.println("[ETH] DHCP failed — retrying in 500 ms");
        }
    }
    else if (linkDown && _ethUp) {
        Serial.println("[ETH] link DOWN — falling back to WiFi for Art-Net");
        _ethUp = false;
        _ethLinkUp = false;
        _ethSpeedMbps  = 0;
        _ethFullDuplex = false;
        _ethUdp.stop();
        announceArtNetNode();                 // updated IP for controllers
    }

    // Apply WiFi-on-ETH policy after any ETH state transition (or always —
    // it's a no-op when the desired state already matches).
    _applyWifiOnEthPolicy();

    // DHCP lease renewal (no-op between renewal intervals; cheap to call often)
    if (_ethUp) Ethernet.maintain();
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
        _wifiOff = true;
        _apMode  = false;
        MDNS.end();
    }
    else if (!wantOff && _wifiOff) {
        Serial.println("[WiFi] Re-enabling — ETH gone or policy cleared");
        _wifiOff = false;
        beginNetwork();   // re-runs STA/AP + mDNS
    }
}

// Periodic serial dump — called from Lumox::loop() via debugEthStatus().
void Lumox::debugEthStatus() {
    const EthernetHardwareStatus hw = Ethernet.hardwareStatus();
    const EthernetLinkStatus     ls = Ethernet.linkStatus();
    if (_ethUp) _refreshEthPhy();
    Serial.printf("[ETH] status: hw=%s  link=%s  dhcp=%s  ip=%s  phy=%uMbps/%s\n",
                  hwStatusName(hw),
                  linkStatusName(ls),
                  _ethUp ? "YES" : "NO",
                  _ethUp ? _ethIp.toString().c_str() : "—",
                  _ethSpeedMbps,
                  _ethFullDuplex ? "FDX" : "HDX");

    if (hw == EthernetNoHardware) {
        Serial.println("[ETH]   no chip on SPI — wiring (MOSI/MISO/SCK/CS), power, RST?");
    } else if (ls == LinkOFF) {
        Serial.println("[ETH]   chip OK but no link — cable unplugged, switch port dead, or MDI mismatch?");
    } else if (ls == LinkON && !_ethUp) {
        Serial.println("[ETH]   link UP but no DHCP lease — router not serving on this VLAN, or MAC filtered?");
    }
}

// ── Unsolicited ArtPollReply broadcast ────────────────────────────────────
// Art-Net spec: nodes MAY send unsolicited ArtPollReply on state change so
// controllers update their node list without an explicit discovery round.
void Lumox::announceArtNetNode() {
    sendArtPollReply(IPAddress(255, 255, 255, 255));
    Serial.printf("[ArtNet] Unsolicited ArtPollReply broadcast — reporting IP %s\n",
                  getIP().toString().c_str());
}

// Read W5500 PHYCFGR (common register 0x002E) to extract link speed + duplex.
// arduino-libraries/Ethernet exposes this via the W5100 singleton but doesn't
// surface it as a public Ethernet API — read it directly.
//   bit 2: DPX (1=full, 0=half)
//   bit 1: SPD (1=100M, 0=10M)
//   bit 0: LNK (1=up)
void Lumox::_refreshEthPhy() {
    if (!_ethHw) return;
    const uint8_t phy = W5100.readPHYCFGR_W5500();
    _ethSpeedMbps  = (phy & 0x02) ? 100 : 10;
    _ethFullDuplex = (phy & 0x04) != 0;
}

String Lumox::getEthMac() const {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             _ethMac[0], _ethMac[1], _ethMac[2],
             _ethMac[3], _ethMac[4], _ethMac[5]);
    return String(buf);
}