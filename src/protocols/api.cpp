#include "protocols/ApiSource.h"
#include "Lumox.h"

bool ApiSource::begin() {
    LOG_PRINTLN("[API] Source selected — direct API ingest is not implemented yet.");
    LOG_PRINTLN("[API] DMX output will hold the last buffered frame; /control still works.");
    return true;
}

void ApiSource::httpStatus(JsonObject& out) {
    out["status"] = "not-implemented";
    out["note"]   = "API source is reserved for future use";
}
