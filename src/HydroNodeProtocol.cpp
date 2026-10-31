#include "HydroNodeProtocol.h"

#include <math.h>
#include <stdio.h>

namespace hydronode {

bool isValidType(const char* type) {
    // Mirrors the backend rule [A-Z][A-Z0-9_]{0,63}.
    if (!type || type[0] < 'A' || type[0] > 'Z') return false;
    size_t len = 1;
    for (const char* c = type + 1; *c; c++, len++) {
        bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!ok || len >= MAX_TYPE_LENGTH) return false;
    }
    return true;
}

size_t formatValue(float value, char* out, size_t size) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!isfinite(value)) return 0;

    // value * 100 is exact in a double (24-bit float mantissa times 100 needs at most 31 bits),
    // so adding 0.5 and cutting off rounds half up on the exact value, as Java does.
    bool negative = signbit(value);
    double cents = fabs((double)value) * 100.0;
    if (cents >= 9.0e15) {
        // Beyond 9e13 a float has no fractional digits left: plain printf gives the same text.
        int written = snprintf(out, size, "%.2f", (double)value);
        if (written < 0 || (size_t)written >= size) {
            out[0] = '\0';
            return 0;
        }
        return (size_t)written;
    }

    // Digits by hand: printf with 64-bit integers is missing in some embedded C libraries.
    uint64_t whole = (uint64_t)(cents + 0.5);
    char reversed[24];
    size_t n = 0;
    reversed[n++] = char('0' + whole % 10);
    whole /= 10;
    reversed[n++] = char('0' + whole % 10);
    whole /= 10;
    reversed[n++] = '.';
    do {
        reversed[n++] = char('0' + whole % 10);
        whole /= 10;
    } while (whole > 0);
    if (negative) reversed[n++] = '-';
    if (n + 1 > size) return 0;
    for (size_t i = 0; i < n; i++) out[i] = reversed[n - 1 - i];
    out[n] = '\0';
    return n;
}

}  // namespace hydronode
