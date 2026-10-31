#pragma once
// HMAC-SHA256 and Base64 for the request signature.
//
// ESP32 uses mbedTLS and the ESP8266 BearSSL, both already in the core (no extra flash). Every
// other board, and the native tests, use the small portable SHA-256 in HydroNodeCrypto.cpp.
// Define HYDRONODE_PORTABLE_CRYPTO to force the portable code on an ESP as well.
#include <stddef.h>
#include <stdint.h>

namespace hydronode {

/**
 * Writes Base64(HMAC-SHA256(key, message)) into `out` (44 characters and a terminator, see
 * SIGNATURE_SIZE in HydroNodeProtocol.h). The message may come in two parts, which are signed as
 * one text (payload + timestamp); pass nullptr/0 for the second part when there is none.
 * Returns false only when the crypto backend failed.
 */
bool signBase64(const char* key, size_t keyLength, const char* part1, size_t length1, const char* part2,
                size_t length2, char out[45]);

/** SHA-256 of `data` (portable implementation, used where the core has none). */
void sha256(const uint8_t* data, size_t length, uint8_t out[32]);

/** Base64 (standard alphabet, with padding) of `length` bytes. `out` needs 4*ceil(length/3)+1. */
size_t base64Encode(const uint8_t* data, size_t length, char* out, size_t size);

}  // namespace hydronode
