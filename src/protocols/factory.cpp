#include "protocols/IDmxSource.h"
#include "protocols/ArtNetSource.h"
#include "protocols/E131Source.h"
#include "protocols/ApiSource.h"
#include <string.h>

const char* protocolTypeName(ProtocolType t) {
    switch (t) {
        case ProtocolType::ArtNet: return "artnet";
        case ProtocolType::E131:   return "e131";
        case ProtocolType::Api:    return "api";
    }
    return "artnet";
}

bool protocolTypeFromString(const char* s, ProtocolType& out) {
    if (!s) return false;
    if (strcasecmp(s, "artnet") == 0 || strcasecmp(s, "art-net") == 0) {
        out = ProtocolType::ArtNet; return true;
    }
    if (strcasecmp(s, "e131") == 0 || strcasecmp(s, "sacn") == 0) {
        out = ProtocolType::E131;   return true;
    }
    if (strcasecmp(s, "api") == 0) {
        out = ProtocolType::Api;    return true;
    }
    return false;
}

IDmxSource* createDmxSource(ProtocolType t) {
    switch (t) {
        case ProtocolType::ArtNet: return new ArtNetSource();
        case ProtocolType::E131:   return new E131Source();
        case ProtocolType::Api:    return new ApiSource();
    }
    return nullptr;
}
