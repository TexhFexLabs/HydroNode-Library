#pragma once
// The HydroNode wire contract: the exact text the backend rebuilds to check a signature.
//
// Plain C++ without Arduino, so the same code runs on the board and in the native tests
// (extras/test), where it is checked against the shared signature vectors
// (extras/test/vectors/signature-vectors.json, the same file the backend and hydronode-sdk test).
//
// Every request is signed the same way:
//
//   X-Signature = Base64( HMAC-SHA256( secret, <canonical payload> + <X-Timestamp> ) )
//
// The canonical payloads (no whitespace, keys in this order, values with exactly 2 decimals):
//
//   one value     {"sensorId":"<id>","type":"<TYPE>","value":21.50,"timestamp":<s>}
//   one round     {"sensorId":"<id>","timestamp":<s>,"values":[{"type":"<TYPE>","value":21.50},...]}
//   stored rounds {"sensorId":"<id>","timestamp":<sent s>,"rounds":["<round signature>",...]}
//
// A stored round is signed like "one round" with the time it was measured; the request that
// carries several of them is signed over the list of their signatures and the time it was sent.
#include <stddef.h>
#include <stdint.h>

/** One measurement: sensor type (e.g. "TEMPERATURE") and value. */
struct HydroNodeValue {
    const char* type;
    float value;
};

namespace hydronode {

/** Longest sensor type: [A-Z][A-Z0-9_]{0,63}. */
static constexpr size_t MAX_TYPE_LENGTH = 64;

/** Characters of a value formatted by formatValue(), including the terminator. */
static constexpr size_t VALUE_TEXT_SIZE = 48;

/** Characters of a Base64 HMAC-SHA256 signature, including the terminator. */
static constexpr size_t SIGNATURE_SIZE = 45;

/** True for A-Z, 0-9 and _, starting with a letter, at most 64 characters (the backend rule). */
bool isValidType(const char* type);

/**
 * The value with exactly 2 decimals, rounded half up like the backend
 * (Java String.format("%.2f")): 21.5 -> "21.50", 21.125 -> "21.13", -0.004 -> "-0.00".
 * Returns the length written, 0 when the buffer is too small or the value is not finite.
 */
size_t formatValue(float value, char* out, size_t size);

/** Appends an unsigned decimal number. */
template <typename Text>
void appendNumber(Text& out, uint64_t number) {
    char digits[21];
    size_t n = 0;
    do {
        digits[n++] = char('0' + number % 10);
        number /= 10;
    } while (number > 0);
    while (n > 0) out += digits[--n];
}

template <typename Text>
void appendValue(Text& out, float value) {
    char text[VALUE_TEXT_SIZE];
    if (formatValue(value, text, sizeof(text)) > 0) out += text;
}

/** {"sensorId":"<id>","type":"<TYPE>","value":21.50,"timestamp":<s>} */
template <typename Text>
void appendValuePayload(Text& out, const char* sensorId, const char* type, float value, uint32_t timestamp) {
    out += "{\"sensorId\":\"";
    out += sensorId;
    out += "\",\"type\":\"";
    out += type;
    out += "\",\"value\":";
    appendValue(out, value);
    out += ",\"timestamp\":";
    appendNumber(out, timestamp);
    out += '}';
}

/**
 * {"sensorId":"<id>","timestamp":<s>,"values":[{"type":"A","value":1.00},...]}
 * `index` picks the values to include (in that order); nullptr means all `count` values.
 */
template <typename Text>
void appendRoundPayload(Text& out, const char* sensorId, uint32_t timestamp, const HydroNodeValue* values,
                        const size_t* index, size_t count) {
    out += "{\"sensorId\":\"";
    out += sensorId;
    out += "\",\"timestamp\":";
    appendNumber(out, timestamp);
    out += ",\"values\":[";
    for (size_t k = 0; k < count; k++) {
        const HydroNodeValue& v = values[index ? index[k] : k];
        if (k) out += ',';
        out += "{\"type\":\"";
        out += v.type;
        out += "\",\"value\":";
        appendValue(out, v.value);
        out += '}';
    }
    out += "]}";
}

/** {"sensorId":"<id>","timestamp":<sent s>,"rounds":["<sig>",...]} */
template <typename Text>
void appendRoundsEnvelope(Text& out, const char* sensorId, uint32_t sentAt, const char* const* signatures,
                          size_t count) {
    out += "{\"sensorId\":\"";
    out += sensorId;
    out += "\",\"timestamp\":";
    appendNumber(out, sentAt);
    out += ",\"rounds\":[";
    for (size_t k = 0; k < count; k++) {
        if (k) out += ',';
        out += '"';
        out += signatures[k];
        out += '"';
    }
    out += "]}";
}

}  // namespace hydronode
