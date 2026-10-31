// Native tests of the wire contract: value formatting, canonical payloads, HMAC-SHA256 and Base64
// (portable crypto). Run with `python3 extras/test/run_tests.py`, which also feeds the modes below
// with random input and checks every answer against Python's own hmac/hashlib and decimal.
//
//   test_protocol            built-in cases, exit code 1 on a failure
//   test_protocol format     "<float bits as hex>" per line -> formatted value
//   test_protocol sign       "<key hex> <message hex>" per line -> Base64 HMAC-SHA256
//   test_protocol payload    "<sensorId> <TYPE> <float bits hex> <timestamp>" -> one-value payload
//   test_protocol round      "<sensorId> <ts> <n> <TYPE> <bits> ..." -> one-round payload
//   test_protocol envelope   "<sensorId> <sent> <n> <signature> ..." -> stored-rounds envelope
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "HydroNodeCrypto.h"
#include "HydroNodeProtocol.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                                                    \
    do {                                                                                               \
        checks++;                                                                                      \
        std::string a_ = (actual), e_ = (expected);                                                    \
        if (a_ != e_) {                                                                                \
            failures++;                                                                                \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_.c_str(), e_.c_str()); \
        }                                                                                              \
    } while (0)

static std::string fmt(float v) {
    char out[hydronode::VALUE_TEXT_SIZE];
    hydronode::formatValue(v, out, sizeof(out));
    return out;
}

static std::string sign(const std::string& key, const std::string& message) {
    char out[hydronode::SIGNATURE_SIZE];
    hydronode::signBase64(key.data(), key.size(), message.data(), message.size(), nullptr, 0, out);
    return out;
}

static std::string hex(const uint8_t* data, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 15];
    }
    return s;
}

static void formatting() {
    CHECK_STR(fmt(21.5f), "21.50");
    CHECK_STR(fmt(21.125f), "21.13");    // exact tie in binary: half up, like Java
    CHECK_STR(fmt(-21.125f), "-21.13");  // away from zero
    CHECK_STR(fmt(0.0f), "0.00");
    CHECK_STR(fmt(-0.0f), "-0.00");      // Java keeps the sign of -0.0
    CHECK_STR(fmt(-0.004f), "-0.00");
    CHECK_STR(fmt(0.005f), "0.00");      // the float is 0.004999999888..., below the half
    CHECK_STR(fmt(3039.0f), "3039.00");
    CHECK_STR(fmt(23.456f), "23.46");
    CHECK_STR(fmt(1013.25f), "1013.25");
    CHECK_STR(fmt(100000.0f), "100000.00");
    char tiny[4];
    CHECK(hydronode::formatValue(21.5f, tiny, sizeof(tiny)) == 0);  // does not fit
    CHECK(hydronode::formatValue(0.0f / 0.0f, tiny, sizeof(tiny)) == 0);
}

static void types() {
    CHECK(hydronode::isValidType("TEMPERATURE"));
    CHECK(hydronode::isValidType("PM25"));
    CHECK(hydronode::isValidType("A"));
    CHECK(!hydronode::isValidType("temperature"));
    CHECK(!hydronode::isValidType("1TEMP"));
    CHECK(!hydronode::isValidType("TEMP-1"));
    CHECK(!hydronode::isValidType(""));
    CHECK(!hydronode::isValidType(nullptr));
    std::string max(64, 'A'), over(65, 'A');
    CHECK(hydronode::isValidType(max.c_str()));
    CHECK(!hydronode::isValidType(over.c_str()));
}

static void payloads() {
    std::string one;
    hydronode::appendValuePayload(one, "550e8400-e29b-41d4-a716-446655440000", "TEMPERATURE", 21.5f, 1730638800);
    CHECK_STR(one, "{\"sensorId\":\"550e8400-e29b-41d4-a716-446655440000\",\"type\":\"TEMPERATURE\",\"value\":21.50,"
                   "\"timestamp\":1730638800}");

    HydroNodeValue values[] = {{"TEMPERATURE", 21.5f}, {"HUMIDITY", 48.0f}, {"CO2", 612.0f}};
    std::string round;
    hydronode::appendRoundPayload(round, "id", 1730638800, values, nullptr, 3);
    CHECK_STR(round, "{\"sensorId\":\"id\",\"timestamp\":1730638800,\"values\":[{\"type\":\"TEMPERATURE\",\"value\":21.50},"
                     "{\"type\":\"HUMIDITY\",\"value\":48.00},{\"type\":\"CO2\",\"value\":612.00}]}");

    size_t index[] = {2, 0};
    std::string picked;
    hydronode::appendRoundPayload(picked, "id", 5, values, index, 2);
    CHECK_STR(picked, "{\"sensorId\":\"id\",\"timestamp\":5,\"values\":[{\"type\":\"CO2\",\"value\":612.00},"
                      "{\"type\":\"TEMPERATURE\",\"value\":21.50}]}");

    const char* signatures[] = {"abc=", "def="};
    std::string envelope;
    hydronode::appendRoundsEnvelope(envelope, "id", 1730642400, signatures, 2);
    CHECK_STR(envelope, "{\"sensorId\":\"id\",\"timestamp\":1730642400,\"rounds\":[\"abc=\",\"def=\"]}");

    std::string zero;
    hydronode::appendNumber(zero, 0);
    CHECK_STR(zero, "0");
}

static void crypto() {
    uint8_t digest[32];
    hydronode::sha256(reinterpret_cast<const uint8_t*>("abc"), 3, digest);
    CHECK_STR(hex(digest, 32), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    hydronode::sha256(reinterpret_cast<const uint8_t*>(""), 0, digest);
    CHECK_STR(hex(digest, 32), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // RFC 4231 test case 2.
    CHECK_STR(sign("Jefe", "what do ya want for nothing?"), "W9zBRr9gdU5qBCQmCJV1x1oAPwidJzmDnexYuWTsOEM=");

    // Two parts sign the same as one text.
    char split[hydronode::SIGNATURE_SIZE];
    hydronode::signBase64("Jefe", 4, "what do ya ", 11, "want for nothing?", 17, split);
    CHECK_STR(split, "W9zBRr9gdU5qBCQmCJV1x1oAPwidJzmDnexYuWTsOEM=");

    char b64[16];
    CHECK(hydronode::base64Encode(reinterpret_cast<const uint8_t*>("ab"), 2, b64, sizeof(b64)) == 4);
    CHECK_STR(b64, "YWI=");
    CHECK(hydronode::base64Encode(reinterpret_cast<const uint8_t*>("abcd"), 4, b64, sizeof(b64)) == 8);
    CHECK_STR(b64, "YWJjZA==");
}

static std::string unhex(const char* s) {
    std::string out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        char pair[3] = {s[i], s[i + 1], 0};
        out += char(strtoul(pair, nullptr, 16));
    }
    return out;
}

static float fromBits(const char* hexBits) {
    uint32_t bits = uint32_t(strtoul(hexBits, nullptr, 16));
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

int main(int argc, char** argv) {
    char a[4096], b[4096], c[256], d[64];
    if (argc > 1 && strcmp(argv[1], "format") == 0) {
        while (scanf("%255s", c) == 1) printf("%s\n", fmt(fromBits(c)).c_str());
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "sign") == 0) {
        while (scanf("%4095s %4095s", a, b) == 2) printf("%s\n", sign(unhex(a), unhex(b)).c_str());
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "round") == 0) {
        // "<sensorId> <timestamp> <count> <TYPE> <bits> ..." -> one-round payload
        unsigned long ts;
        unsigned count;
        while (scanf("%255s %lu %u", a, &ts, &count) == 3) {
            std::vector<std::string> types(count);
            std::vector<HydroNodeValue> values(count);
            for (unsigned i = 0; i < count; i++) {
                if (scanf("%255s %63s", b, c) != 2) return 1;
                types[i] = b;
                values[i].value = fromBits(c);
            }
            for (unsigned i = 0; i < count; i++) values[i].type = types[i].c_str();
            std::string out;
            hydronode::appendRoundPayload(out, a, uint32_t(ts), values.data(), nullptr, count);
            printf("%s\n", out.c_str());
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "envelope") == 0) {
        // "<sensorId> <sent> <count> <sig> ..." -> stored-rounds envelope
        unsigned long sent;
        unsigned count;
        while (scanf("%255s %lu %u", a, &sent, &count) == 3) {
            std::vector<std::string> sigs(count);
            std::vector<const char*> list(count);
            for (unsigned i = 0; i < count; i++) {
                if (scanf("%255s", b) != 1) return 1;
                sigs[i] = b;
            }
            for (unsigned i = 0; i < count; i++) list[i] = sigs[i].c_str();
            std::string out;
            hydronode::appendRoundsEnvelope(out, a, uint32_t(sent), list.data(), count);
            printf("%s\n", out.c_str());
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "payload") == 0) {
        while (scanf("%255s %255s %63s %63s", a, b, c, d) == 4) {
            std::string out;
            hydronode::appendValuePayload(out, a, b, fromBits(c), uint32_t(strtoul(d, nullptr, 10)));
            printf("%s\n", out.c_str());
        }
        return 0;
    }
    formatting();
    types();
    payloads();
    crypto();
    printf("protocol: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
