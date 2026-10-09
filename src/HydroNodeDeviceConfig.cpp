#include "HydroNodeDeviceConfig.h"

#include <stdio.h>
#include <string.h>

#include "HydroNodeBatteryGuard.h"

namespace hydronode {

namespace {

/** Appends " key=value" when it fits, otherwise nothing (no half key). */
void append(char* out, size_t size, size_t& length, const char* part) {
    size_t add = strlen(part);
    size_t limit = size - 1 < DEVICE_CONFIG_MAX ? size - 1 : DEVICE_CONFIG_MAX;
    size_t sep = length > 0 ? 1 : 0;
    if (length + sep + add > limit) return;
    if (sep) out[length++] = ' ';
    memcpy(out + length, part, add);
    length += add;
    out[length] = '\0';
}

void appendNumber(char* out, size_t size, size_t& length, const char* key, uint32_t value) {
    char part[48];
    snprintf(part, sizeof(part), "%s=%lu", key, static_cast<unsigned long>(value));
    append(out, size, length, part);
}

/** Lower case token of a-z, 0-9 and _ (at most 32), so a value can never break the header. */
void appendToken(char* out, size_t size, size_t& length, const char* key, const char* value) {
    if (!value) return;
    char token[33];
    size_t n = 0;
    for (const char* c = value; *c && n < sizeof(token) - 1; c++) {
        char ch = *c >= 'A' && *c <= 'Z' ? static_cast<char>(*c - 'A' + 'a') : *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_') token[n++] = ch;
    }
    token[n] = '\0';
    if (n == 0) return;
    char part[48];
    snprintf(part, sizeof(part), "%s=%s", key, token);
    append(out, size, length, part);
}

}  // namespace

size_t formatDeviceConfig(char* out, size_t size, const HydroNodeDeviceConfig& c, bool acceptsSettings,
                          int32_t rejectedRevision, const char* error) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    size_t length = 0;
    append(out, size, length, "v=1");
    appendNumber(out, size, length, "rev", c.revision);
    if (c.intervalSeconds) appendNumber(out, size, length, "int", c.intervalSeconds);
    if (c.saveMv) appendNumber(out, size, length, "save", c.saveMv);
    if (c.recoveryMv) appendNumber(out, size, length, "rec", c.recoveryMv);
    if (c.standbyMv) appendNumber(out, size, length, "sby", c.standbyMv);
    if (c.resumeMv) appendNumber(out, size, length, "res", c.resumeMv);
    appendToken(out, size, length, "src", c.source);
    appendToken(out, size, length, "gauge", c.gauge);
    if (c.cells) appendNumber(out, size, length, "cells", c.cells);
    if (c.capacityMah) appendNumber(out, size, length, "mah", c.capacityMah);
    appendToken(out, size, length, "pwr", c.powerState);
    if (acceptsSettings) append(out, size, length, "caps=settings");
    if (rejectedRevision >= 0) appendNumber(out, size, length, "rej", static_cast<uint32_t>(rejectedRevision));
    appendToken(out, size, length, "err", error ? error : c.error);
    return length;
}

bool settingsValid(const HydroNodeSettings& s) {
    if (s.intervalSeconds < INTERVAL_MIN_S || s.intervalSeconds > INTERVAL_MAX_S) return false;
    if (!s.saveMv && !s.recoveryMv && !s.standbyMv && !s.resumeMv) return true;  // interval only
    HydroNodeThresholds t{s.saveMv, s.recoveryMv, s.standbyMv, s.resumeMv};
    return HydroNodeBatteryGuard::validate(t, 1, 0, 0) == 0;
}

}  // namespace hydronode
