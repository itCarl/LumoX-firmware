#include "Lumox.h"
#include "config.h"
#include "html.h"
#include <ArduinoJson.h>
#include <ElegantOTA.h>

// ── Helpers ─────────────────────────────────────────────────────────────────

// Sends pre-compressed PROGMEM data with Content-Encoding: gzip. Browser
// decompresses automatically — no runtime overhead on the ESP32.
static void serveGzip(AsyncWebServerRequest* request,
                      const uint8_t* data, uint16_t len, const char* contentType) {
    AsyncWebServerResponse* resp = request->beginResponse(200, contentType, data, len);
    resp->addHeader("Content-Encoding", "gzip");
    request->send(resp);
}

// Mutex-guarded DMX snapshot — see header comment in Lumox.h.
void Lumox::snapshotDmx(uint8_t* out512) {
    xSemaphoreTake(_dmxMutex, portMAX_DELAY);
    memcpy(out512, &dmxBuffer[1], 512);
    xSemaphoreGive(_dmxMutex);
}

// ── WebSocket events (AsyncWebSocket on /ws) ────────────────────────────────
// Mounted on the same AsyncWebServer as HTTP — port 80, path /ws.
// Runs on AsyncTCP's task, no loop tick for IO. cleanupClients() is called
// from loopWebServer() to drop dead sockets.

void Lumox::_onWsEvent(AsyncWebSocket* /*server*/, AsyncWebSocketClient* client,
                       AwsEventType type, void* /*arg*/, uint8_t* /*data*/, size_t /*len*/) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[WS] Client #%u connected (%s)\n",
                          client->id(), client->remoteIP().toString().c_str());
            // Send current state immediately so the dashboard doesn't blink
            // empty until the next 5 Hz broadcast.
            {
                String json = buildStatusJson();
                client->text(json);
                uint8_t snap[512];
                snapshotDmx(snap);
                client->binary(snap, sizeof(snap));
            }
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("[WS] Client #%u disconnected\n", client->id());
            break;
        default:
            break;
    }
}

// ── Captive-portal redirect helper ─────────────────────────────────────────
// Forces the OS "sign in to network" popup on Android/iOS/Windows, lands the
// browser on /config. Cache-Control + Pragma prevent the OS probe from
// caching the response and suppressing the popup on the next probe.
static void sendCaptiveRedirect(AsyncWebServerRequest* request, const String& ip) {
    String loc = String("http://") + ip + "/config";
    AsyncWebServerResponse* resp = request->beginResponse(302, "text/html",
        "<!DOCTYPE html><html><body>"
        "<a href=\"" + loc + "\">Sign in to Lumox</a>"
        "</body></html>");
    resp->addHeader("Location", loc);
    resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    resp->addHeader("Pragma", "no-cache");
    resp->addHeader("Expires", "0");
    request->send(resp);
}

// ── Config POST body handler ────────────────────────────────────────────────
// AsyncWebServer streams body as chunks; gather into a String, parse on final
// chunk. JSON config payload is small (< 1 KB), so a single allocation is OK.
static void handleConfigBody(Lumox& lx, AsyncWebServerRequest* request,
                             uint8_t* data, size_t len, size_t index, size_t total) {
    String* buf = nullptr;
    if (request->_tempObject == nullptr) {
        buf = new String();
        buf->reserve(total + 1);
        request->_tempObject = buf;
    } else {
        buf = static_cast<String*>(request->_tempObject);
    }

    buf->concat((const char*)data, len);
    if (index + len < total) return;        // more chunks coming

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, *buf);
    delete buf;
    request->_tempObject = nullptr;

    if (err) {
        request->send(400, "text/plain", String("JSON parse error: ") + err.c_str());
        return;
    }

    // Validation
    String apSsid = doc["apSsid"] | "";
    String apPass = doc["apPass"] | "";
    if (apSsid.length() == 0) {
        request->send(400, "text/plain", "AP SSID is required");
        return;
    }
    if (apPass.length() > 0 && apPass.length() < 8) {
        request->send(400, "text/plain", "AP password must be empty or >= 8 characters");
        return;
    }

    // Apply
    lx.cfgApSsid      = apSsid;
    lx.cfgApPassword  = apPass;
    lx.cfgStaSsid     = doc["staSsid"]  | "";
    lx.cfgStaPassword = doc["staPass"]  | "";
    lx.cfgUniverse    = doc["universe"] | 0;

    // Ethernet — static fields are accepted even when DHCP is on (used on
    // first enable of static mode). Empty / "0.0.0.0" = unset (0).
    auto parseIp = [](const String& s, IPAddress& out) {
        if (s.length() == 0) { out = IPAddress(); return true; }
        return out.fromString(s);
    };
    lx.cfgEthDhcp = doc["ethDhcp"] | true;
    String eIp  = doc["ethIp"]  | "";
    String eGw  = doc["ethGw"]  | "";
    String eSub = doc["ethSub"] | "";
    String eDns = doc["ethDns"] | "";
    if (!parseIp(eIp,  lx.cfgEthIp) ||
        !parseIp(eGw,  lx.cfgEthGw) ||
        !parseIp(eSub, lx.cfgEthSub) ||
        !parseIp(eDns, lx.cfgEthDns)) {
        request->send(400, "text/plain", "Invalid Ethernet IP/GW/Sub/DNS");
        return;
    }
    if (!lx.cfgEthDhcp && (uint32_t)lx.cfgEthIp == 0) {
        request->send(400, "text/plain", "Static ETH requires an IP");
        return;
    }

    lx.cfgWifiDisableOnEth = doc["wifiOffEth"] | false;

    lx.saveConfig();

    request->send(200, "application/json", "{\"ok\":true}");

    Serial.println("[Config] Saved — rebooting in 1 s...");
    // Reboot from a deferred task — restarting from inside the request lambda
    // would cut the response before the client receives it.
    xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(1000)); ESP.restart(); },
                "rebootDelay", 2048, nullptr, 1, nullptr);
}

// ── Register routes ─────────────────────────────────────────────────────────

void Lumox::_registerRoutes() {
    _http.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveGzip(req, PAGE_index, PAGE_index_length, "text/html");
    });

    _http.on("/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveGzip(req, PAGE_config, PAGE_config_length, "text/html");
    });

    _http.on("/health", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveGzip(req, PAGE_health, PAGE_health_length, "text/html");
    });

    _http.on("/control", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveGzip(req, PAGE_control, PAGE_control_length, "text/html");
    });

    _http.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", buildStatusJson());
    });

    _http.on("/api/dmx", HTTP_GET, [this](AsyncWebServerRequest* req) {
        // Snapshot first so the JSON build doesn't see torn writes from the
        // Art-Net parser running concurrently on Core 1.
        uint8_t snap[512];
        snapshotDmx(snap);
        String out;
        out.reserve(2048);
        out = "[";
        for (int i = 0; i < 512; i++) {
            if (i > 0) out += ',';
            out += snap[i];
        }
        out += "]";
        req->send(200, "application/json", out);
    });

    _http.on("/api/config", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["apSsid"]     = cfgApSsid;
        doc["apPass"]     = cfgApPassword;
        doc["staSsid"]    = cfgStaSsid;
        doc["staPass"]    = cfgStaPassword;
        doc["universe"]   = cfgUniverse;
        doc["ethDhcp"]    = cfgEthDhcp;
        doc["ethIp"]      = cfgEthIp.toString();
        doc["ethGw"]      = cfgEthGw.toString();
        doc["ethSub"]     = cfgEthSub.toString();
        doc["ethDns"]     = cfgEthDns.toString();
        doc["wifiOffEth"] = cfgWifiDisableOnEth;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // POST /api/config — body arrives in chunks; collected by handleConfigBody.
    _http.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) { /* request handler — body handler does the work */ },
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handleConfigBody(*this, req, data, len, index, total);
        });

    // GET /api/manual — { enabled: bool, overrides: { "ch": val, ... } }
    _http.on("/api/manual", HTTP_GET, [this](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["enabled"] = manualEnabled;
        auto ov = doc["overrides"].to<JsonObject>();
        for (int i = 1; i <= 512; i++) {
            if (manualValue[i] >= 0) ov[String(i)] = manualValue[i];
        }
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // POST /api/manual — body may contain any combination of:
    //   { "enabled": bool }                — flip master switch
    //   { "ch": 1..512, "val": -1..255 }   — single channel (val=-1 releases)
    //   { "values": [{ch, val}, ...] }     — batch
    _http.on("/api/manual", HTTP_POST,
        [](AsyncWebServerRequest* req) { /* body handler does the work */ },
        nullptr,
        [this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            String* buf;
            if (req->_tempObject == nullptr) {
                buf = new String();
                buf->reserve(total + 1);
                req->_tempObject = buf;
            } else {
                buf = static_cast<String*>(req->_tempObject);
            }
            buf->concat((const char*)data, len);
            if (index + len < total) return;

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, *buf);
            delete buf;
            req->_tempObject = nullptr;
            if (err) { req->send(400, "text/plain", err.c_str()); return; }

            if (doc["enabled"].is<bool>()) {
                setManualEnabled(doc["enabled"].as<bool>());
            }
            if (doc["values"].is<JsonArray>()) {
                for (JsonObject e : doc["values"].as<JsonArray>()) {
                    setManual(e["ch"] | 0, e["val"] | -1);
                }
            } else if (doc["ch"].is<int>()) {
                setManual(doc["ch"] | 0, doc["val"] | -1);
            }
            req->send(200, "application/json", "{\"ok\":true}");
        });

    // POST /api/manual/clear — release every channel back to Art-Net (master
    // switch state is left as-is; clearing values doesn't disable the mode).
    _http.on("/api/manual/clear", HTTP_POST, [this](AsyncWebServerRequest* req) {
        clearAllManual();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    _http.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        Serial.println("[System] Reboot requested.");
        xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(500)); ESP.restart(); },
                    "rebootDelay", 2048, nullptr, 1, nullptr);
    });

    _http.on("/api/reset", HTTP_POST, [this](AsyncWebServerRequest* req) {
        req->send(200, "application/json", "{\"ok\":true}");
        // factoryReset() calls ESP.restart() — defer so the response goes out.
        xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(200));
                               Lumox::instance().factoryReset(); },
                    "factoryReset", 4096, nullptr, 1, nullptr);
    });

    // ── Captive-portal probe endpoints ─────────────────────────────────────
    // Each OS pings its own URL on WiFi connect. A response other than the
    // expected "success" makes the OS show a "Sign in to network" notification.
    // Captive fires whenever softAP is up (_apActive) — including AP-aux mode
    // (ETH up + STA down + AP serving). Redirect uses softAPIP() explicitly so
    // AP clients don't get pointed at the ETH address they can't reach.
    auto captive = [this](AsyncWebServerRequest* req) {
        if (_apActive) sendCaptiveRedirect(req, WiFi.softAPIP().toString());
        else           req->send(204);
    };

    // Android (Google Play Services + stock)
    _http.on("/generate_204",                 HTTP_GET, captive);
    _http.on("/gen_204",                      HTTP_GET, captive);
    // iOS / macOS
    _http.on("/hotspot-detect.html",          HTTP_GET, captive);
    _http.on("/library/test/success.html",    HTTP_GET, captive);
    _http.on("/success.html",                 HTTP_GET, captive);
    // Windows NCSI
    _http.on("/connecttest.txt",              HTTP_GET, captive);
    _http.on("/ncsi.txt",                     HTTP_GET, captive);
    _http.on("/redirect",                     HTTP_GET, captive);

    // RFC 8908 — structured captive-portal API
    _http.on("/.well-known/captive-portal", HTTP_GET, [this](AsyncWebServerRequest* req) {
        // Use softAPIP for AP clients (incl. AP-aux mode) — they can't reach
        // ETH/STA addresses. Otherwise advertise the primary IP.
        const String host = _apActive ? WiFi.softAPIP().toString() : getIP().toString();
        String portal = String("http://") + host + "/config";
        String body = String("{\"captive\":true,\"user-portal-url\":\"") + portal + "\"}";
        AsyncWebServerResponse* resp = req->beginResponse(200, "application/captive+json", body);
        resp->addHeader("Cache-Control", "private");
        req->send(resp);
    });

    _http.onNotFound([this](AsyncWebServerRequest* req) {
        if (_apActive) sendCaptiveRedirect(req, WiFi.softAPIP().toString());
        else           req->send(404, "text/plain", "Not found");
    });
}

// ── Public: begin & loop ────────────────────────────────────────────────────

void Lumox::beginWebServer() {
    _registerRoutes();

    // AsyncWebSocket mounted at /ws on the same HTTP server (port 80).
    _ws.onEvent([this](AsyncWebSocket* s, AsyncWebSocketClient* c,
                       AwsEventType t, void* a, uint8_t* d, size_t l) {
        _onWsEvent(s, c, t, a, d, l);
    });
    _http.addHandler(&_ws);

    // ElegantOTA in async mode (ELEGANTOTA_USE_ASYNC_WEBSERVER=1) — mounts /update.
    ElegantOTA.begin(&_http);

    _http.begin();
    Serial.printf("[Web] HTTP (async) on port %d, WS on /ws\n", WEB_SERVER_PORT);
}

void Lumox::loopWebServer() {
    // AsyncWebServer + AsyncWebSocket need no loop tick for IO — they run on
    // AsyncTCP's task. cleanupClients() drops disconnected sockets so the
    // tracking list doesn't grow unbounded.
    _ws.cleanupClients();
    ElegantOTA.loop();

    if (_apActive) {
        _dns.processNextRequest();
    }
}

// ── JSON status for dashboard + WS push ─────────────────────────────────────

String Lumox::buildStatusJson() {
    JsonDocument doc;

    // Network
    auto net = doc["net"].to<JsonObject>();
    // "mode" = primary active interface. ETH wins over STA when both up.
    const char* mode = _apMode ? "AP" : (_ethUp ? "ETH" : "STA");
    net["mode"]    = mode;
    net["fb"]      = _apFallback;
    net["ap"]      = _apActive;       // AP-aux flag (true even when ETH primary)
    net["apIp"]    = _apActive ? WiFi.softAPIP().toString() : String("");
    net["ssid"]    = _apMode ? cfgApSsid : cfgStaSsid;
    net["ip"]      = getIP().toString();
    net["rssi"]    = getRssi();
    net["clients"] = getClients();
    // Network hostname — MAC-derived, used for mDNS + DHCP. Decoupled from
    // the user-friendly cfgDeviceName so multiple boards can share a label.
    net["host"]    = networkHostname();
    net["name"]    = cfgDeviceName;

    // STA sub-state (so dashboard can show both interfaces side-by-side)
    auto sta = net["sta"].to<JsonObject>();
    const bool staOn = (WiFi.status() == WL_CONNECTED);
    sta["up"]   = staOn;
    sta["ip"]   = staOn ? WiFi.localIP().toString() : String("");
    sta["rssi"] = staOn ? WiFi.RSSI()               : 0;

    // Ethernet sub-state. State updated from ETH events (_onEthEvent), so
    // these fields are always coherent with the active interface.
    auto eth = net["eth"].to<JsonObject>();
    eth["hw"]    = _ethHw;
    eth["up"]    = _ethUp;
    eth["ip"]    = _ethUp ? _ethIp.toString() : String("");
    eth["mac"]   = getEthMac();
    eth["link"]  = _ethLinkUp;
    eth["speed"] = _ethSpeedMbps;
    eth["fdx"]   = _ethFullDuplex;

    // Art-Net
    auto an = doc["artnet"].to<JsonObject>();
    an["uni"]      = cfgUniverse;
    an["pkts"]     = statsArtnetPackets;
    an["stale"]    = statsArtnetStale;
    an["locked"]   = statsArtnetLocked;
    an["age"]      = statsArtnetLastMs > 0 ? millis() - statsArtnetLastMs : 0;
    an["sender"]   = statsArtnetSender.toString();
    an["luni"]     = statsArtnetLastUni;
    an["swaps"]    = statsArtnetSenderSwaps;
    an["syncs"]    = statsArtSyncs;
    an["inSync"]   = statsInSyncMode;
    an["badProto"] = statsArtNetBadProto;

    // Manual override state
    auto man = doc["manual"].to<JsonObject>();
    man["enabled"] = manualEnabled;
    man["active"]  = manualActive;

    // DMX signal health — explicit pass/fail + detailed counters.
    auto dmx = doc["dmx"].to<JsonObject>();
    const DmxHealth h = dmxHealth();
    dmx["ok"]         = h.ok;
    dmx["reason"]     = h.reason;
    dmx["frames"]     = statsDmxFramesSent;
    dmx["sendErr"]    = statsDmxSendErrors;
    dmx["waitTO"]     = statsDmxWaitTimeouts;
    dmx["consecErr"]  = statsDmxConsecErrors;
    dmx["maxConsec"]  = statsDmxMaxConsecErr;
    dmx["rateHz"]     = statsDmxRateHz;
    dmx["maxMutexUs"] = statsDmxMaxMutexUs;

    // System
    auto sys = doc["sys"].to<JsonObject>();
    sys["up"]        = millis() / 1000;
    sys["heap"]      = ESP.getFreeHeap();
    sys["heapTotal"] = ESP.getHeapSize();
    sys["mhz"]       = ESP.getCpuFreqMHz();
    sys["chip"]      = ESP.getChipModel();
    sys["rev"]       = ESP.getChipRevision();
    sys["cores"]     = ESP.getChipCores();
    sys["flash"]     = ESP.getFlashChipSize();

    String out;
    serializeJson(doc, out);
    return out;
}

// ── WebSocket Broadcast ─────────────────────────────────────────────────────

void Lumox::pushStateToClients() {
    String json = buildStatusJson();
    _ws.textAll(json);

    // Snapshot under mutex — concurrent Art-Net writes would otherwise
    // produce torn frames in the dashboard.
    uint8_t snap[512];
    snapshotDmx(snap);
    _ws.binaryAll(snap, sizeof(snap));
}
