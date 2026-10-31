#include "HydroNodeCrypto.h"

#include <string.h>

#if !defined(HYDRONODE_PORTABLE_CRYPTO) && defined(ESP32)
#define HYDRONODE_CRYPTO_MBEDTLS 1
#include <mbedtls/md.h>
#elif !defined(HYDRONODE_PORTABLE_CRYPTO) && defined(ESP8266)
#define HYDRONODE_CRYPTO_BEARSSL 1
#include <bearssl/bearssl_hmac.h>
#endif

namespace hydronode {

// ---- Portable SHA-256 (FIPS 180-4) ----------------------------------------------------------

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    uint32_t h[8];
    uint8_t block[64];
    size_t used = 0;
    uint64_t bits = 0;

    Sha256() {
        static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h, init, sizeof(h));
    }

    void compress() {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(block[4 * i]) << 24) | (uint32_t(block[4 * i + 1]) << 16) |
                   (uint32_t(block[4 * i + 2]) << 8) | uint32_t(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = k + s1 + ch + K[i] + w[i];
            uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = s0 + maj;
            k = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += k;
    }

    void update(const uint8_t* data, size_t length) {
        bits += uint64_t(length) * 8;
        while (length > 0) {
            size_t take = 64 - used < length ? 64 - used : length;
            memcpy(block + used, data, take);
            used += take;
            data += take;
            length -= take;
            if (used == 64) {
                compress();
                used = 0;
            }
        }
    }

    void finish(uint8_t out[32]) {
        uint64_t total = bits;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (used != 56) update(&zero, 1);
        uint8_t length[8];
        for (int i = 0; i < 8; i++) length[i] = uint8_t(total >> (56 - 8 * i));
        update(length, 8);
        for (int i = 0; i < 8; i++) {
            out[4 * i] = uint8_t(h[i] >> 24);
            out[4 * i + 1] = uint8_t(h[i] >> 16);
            out[4 * i + 2] = uint8_t(h[i] >> 8);
            out[4 * i + 3] = uint8_t(h[i]);
        }
    }
};

#if !defined(HYDRONODE_CRYPTO_MBEDTLS) && !defined(HYDRONODE_CRYPTO_BEARSSL)
/** HMAC (RFC 2104) over the portable SHA-256. */
void hmacPortable(const char* key, size_t keyLength, const char* part1, size_t length1, const char* part2,
                  size_t length2, uint8_t mac[32]) {
    uint8_t k[64] = {0};
    if (keyLength > 64) {
        sha256(reinterpret_cast<const uint8_t*>(key), keyLength, k);
    } else {
        memcpy(k, key, keyLength);
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    Sha256 inner;
    inner.update(pad, 64);
    if (part1) inner.update(reinterpret_cast<const uint8_t*>(part1), length1);
    if (part2) inner.update(reinterpret_cast<const uint8_t*>(part2), length2);
    uint8_t innerHash[32];
    inner.finish(innerHash);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    Sha256 outer;
    outer.update(pad, 64);
    outer.update(innerHash, 32);
    outer.finish(mac);
    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
}
#endif

}  // namespace

void sha256(const uint8_t* data, size_t length, uint8_t out[32]) {
    Sha256 s;
    s.update(data, length);
    s.finish(out);
}

size_t base64Encode(const uint8_t* data, size_t length, char* out, size_t size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = 4 * ((length + 2) / 3);
    if (size < needed + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < length) n |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < length) n |= data[i + 2];
        out[o++] = alphabet[(n >> 18) & 63];
        out[o++] = alphabet[(n >> 12) & 63];
        out[o++] = i + 1 < length ? alphabet[(n >> 6) & 63] : '=';
        out[o++] = i + 2 < length ? alphabet[n & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

bool signBase64(const char* key, size_t keyLength, const char* part1, size_t length1, const char* part2,
                size_t length2, char out[45]) {
    uint8_t mac[32];
#if defined(HYDRONODE_CRYPTO_MBEDTLS)
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    bool ok = mbedtls_md_setup(&ctx, info, 1) == 0 &&
              mbedtls_md_hmac_starts(&ctx, reinterpret_cast<const unsigned char*>(key), keyLength) == 0 &&
              (!part1 || mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(part1), length1) == 0) &&
              (!part2 || mbedtls_md_hmac_update(&ctx, reinterpret_cast<const unsigned char*>(part2), length2) == 0) &&
              mbedtls_md_hmac_finish(&ctx, mac) == 0;
    mbedtls_md_free(&ctx);
    if (!ok) {
        out[0] = '\0';
        return false;
    }
#elif defined(HYDRONODE_CRYPTO_BEARSSL)
    br_hmac_key_context keyContext;
    br_hmac_context ctx;
    br_hmac_key_init(&keyContext, &br_sha256_vtable, key, keyLength);
    br_hmac_init(&ctx, &keyContext, 0);
    if (part1) br_hmac_update(&ctx, part1, length1);
    if (part2) br_hmac_update(&ctx, part2, length2);
    br_hmac_out(&ctx, mac);
    memset(&keyContext, 0, sizeof(keyContext));
#else
    hmacPortable(key, keyLength, part1, length1, part2, length2, mac);
#endif
    size_t written = base64Encode(mac, sizeof(mac), out, 45);
    memset(mac, 0, sizeof(mac));
    return written == 44;
}

}  // namespace hydronode
