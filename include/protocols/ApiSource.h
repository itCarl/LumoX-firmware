#pragma once

#include "IDmxSource.h"

// Stub source — reserved for a future direct-control API (likely a custom
// WebSocket DMX push from the Lumox desktop app, no Art-Net framing).
// Today: starts cleanly, does nothing, reports "Not implemented" in status.
//
// Picking this protocol parks the device with no ingress — output holds the
// last buffer (zero on fresh boot) and the manual /control page still works.
class ApiSource : public IDmxSource {
public:
    ApiSource() = default;

    ProtocolType type()  const override { return ProtocolType::Api; }
    const char*  name()  const override { return "api"; }

    bool begin()   override;
    void end()     override {}
    void service() override {}
    void httpStatus(JsonObject& out) override;
};
