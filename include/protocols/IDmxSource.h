#pragma once

// ── DMX source abstraction ────────────────────────────────────────────────
// Each ingress protocol (Art-Net, sACN E1.31, future API) implements this
// interface. Lumox owns one IDmxSource at a time, selected by cfgProtocol.
//
// Lifecycle: factory creates → begin() → loop() called from a dedicated task
// in tight cycles → end() on shutdown (today: only on reboot; hot-swap is a
// future improvement). Sources call back into Lumox::feedDmxFrame() to ingest
// validated DMX, into Lumox::saveConfig() / setIdentify() / requestReboot()
// for control commands, and read network state via the existing accessors.

#include <Arduino.h>
#include <ArduinoJson.h>

enum class ProtocolType : uint8_t {
    ArtNet = 0,
    E131   = 1,
    Api    = 2,
};

// String <-> enum helpers used by the /api/config JSON layer + factory.
const char*  protocolTypeName(ProtocolType t);
bool         protocolTypeFromString(const char* s, ProtocolType& out);

class IDmxSource {
public:
    virtual ~IDmxSource() = default;

    virtual ProtocolType type()  const = 0;
    virtual const char*  name()  const = 0;     // short label, matches NVS string

    virtual bool begin()   = 0;   // open sockets, register mDNS extras if any
    virtual void end()     = 0;   // close sockets — used for hot-swap (TODO)
    virtual void service() = 0;   // drain RX queue + protocol-specific housekeeping (announce, timers)

    // Called when the active interface comes up / changes (ETH GOT_IP,
    // WiFi connect, etc). Source decides what "network changed" means.
    virtual void onNetworkChange() {}

    // Optional hooks. Default no-op so sources only override what they need.
    virtual bool identifyActive() const { return false; }   // LED locate blink
    virtual void httpStatus(JsonObject& out) {}             // /api/status extras
};

// Factory — defined in src/protocols/factory.cpp. Returns nullptr on bad type.
class IDmxSource* createDmxSource(ProtocolType t);
