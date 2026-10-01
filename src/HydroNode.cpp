#include "HydroNode.h"
#include <math.h>
#include <string.h>
#include <vector>
#include <time.h>
#include <sys/time.h>
#if defined(ESP8266)
#include <base64.h>
#include <bearssl/bearssl_hmac.h>
#else
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#endif

// Epoch sanity floor: any synced clock is past 2020-09-13. Values below
// mean the NTP sync never happened and the system clock is still at 1970.
static constexpr unsigned long MIN_VALID_EPOCH = 1600000000UL;

HydroNode::HydroNode(const char* sensorId, const char* secretKey, const char* host, const char* path)
    : sensorId_(sensorId), secretKey_(secretKey), host_(host), path_(path),
      timeClient_(ntpUDP_, "pool.ntp.org")
{
}

void HydroNode::begin() {
    timeClient_.begin();
    timeClient_.setTimeOffset(0);
}

bool HydroNode::connectWiFi(const char* ssid, const char* password, uint32_t timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    dbg("WiFi: connecting to " + String(ssid));
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start >= timeoutMs) {
            dbg("WiFi: connect timeout");
            return false;
        }
        delay(250);
    }
    dbg("WiFi: connected, IP " + WiFi.localIP().toString());
    return true;
}

String HydroNode::getApName() const {
    String id(sensorId_);
    String suffix = id.substring(id.length() - 4);
    return "HydroNode-Setup-" + suffix;
}

void HydroNode::on(const String& key, std::function<void(JsonVariant)> handler) {
    handlers_[key] = Handler{ValueType::ANY, handler};
}

// Typed callbacks only run after checkCommand() has confirmed that the value
// fits the type, so the as<T>() conversions below never truncate or guess.
void HydroNode::onBool(const String& key, std::function<void(bool)> handler) {
    handlers_[key] = Handler{ValueType::BOOL, [handler](JsonVariant v) { handler(v.as<bool>()); }};
}

void HydroNode::onInt32(const String& key, std::function<void(int32_t)> handler) {
    handlers_[key] = Handler{ValueType::INT32, [handler](JsonVariant v) { handler(v.as<int32_t>()); }};
}

void HydroNode::onUInt32(const String& key, std::function<void(uint32_t)> handler) {
    handlers_[key] = Handler{ValueType::UINT32, [handler](JsonVariant v) { handler(v.as<uint32_t>()); }};
}

void HydroNode::onInt64(const String& key, std::function<void(int64_t)> handler) {
    handlers_[key] = Handler{ValueType::INT64, [handler](JsonVariant v) { handler(v.as<int64_t>()); }};
}

void HydroNode::onUInt64(const String& key, std::function<void(uint64_t)> handler) {
    handlers_[key] = Handler{ValueType::UINT64, [handler](JsonVariant v) { handler(v.as<uint64_t>()); }};
}

void HydroNode::onString(const String& key, std::function<void(const String&)> handler) {
    handlers_[key] = Handler{ValueType::STRING, [handler](JsonVariant v) { handler(String(v.as<const char*>())); }};
}

void HydroNode::setJsonBufferSize(size_t size) {
    jsonBufferSize_ = size;
}

void HydroNode::setHttpTimeout(uint32_t ms) {
    httpTimeoutMs_ = ms;
}

void HydroNode::setDebug(Stream& stream) {
    debug_ = &stream;
}

bool HydroNode::ensureTimeSynced(unsigned long& epochOut) {
    // The backend rejects timestamps outside a small replay window
    // (currently +/- 2 minutes), so an unsynced or stale clock means a
    // guaranteed 401. Retry the NTP sync a few times before giving up.
    timeClient_.update();
    for (int attempt = 0; attempt < 3 && timeClient_.getEpochTime() < MIN_VALID_EPOCH; attempt++) {
        dbg("NTP: forcing update (attempt " + String(attempt + 1) + ")");
        timeClient_.forceUpdate();
    }

    unsigned long epoch = timeClient_.getEpochTime();
    if (epoch < MIN_VALID_EPOCH) {
        return false;
    }

    // The TLS stack validates certificate notBefore/notAfter against the system
    // clock, which starts at 1970 after boot (NTPClient does not set it).
    // Sync it once from NTP so TLS certificate validation can succeed.
    if (time(nullptr) < (time_t)MIN_VALID_EPOCH) {
        struct timeval tv = { (time_t)epoch, 0 };
        settimeofday(&tv, nullptr);
    }

    epochOut = epoch;
    return true;
}

int HydroNode::sendValue(const char* type, float value) {
    // Checked before any network work: the backend rejects both with 400,
    // so sending them would only cost a TLS handshake.
    if (!isValidType(type)) {
        dbg("sendValue: invalid type '" + String(type ? type : "") + "' (use A-Z, 0-9, _ like TEMPERATURE)");
        return ERR_INVALID_TYPE;
    }
    if (!isfinite(value)) {
        dbg("sendValue: value is NaN or infinite, check the sensor reading");
        return ERR_INVALID_VALUE;
    }

    if (WiFi.status() != WL_CONNECTED) {
        dbg("sendValue: WiFi not connected");
        return ERR_WIFI_DISCONNECTED;
    }

    unsigned long epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        dbg("sendValue: NTP time not synced, aborting (backend would reject the signature)");
        return ERR_TIME_NOT_SYNCED;
    }

    // Payload format is part of the HMAC contract with the backend —
    // key order, 2 decimal places and zero whitespace must not change.
    String payload = "{\"sensorId\":\"" + String(sensorId_) + "\",\"type\":\"" + type + "\",\"value\":" + String(value, 2) + ",\"timestamp\":" + String(epoch) + "}";

    String response;
    int statusCode = postSigned(path_, payload, epoch, response);
    if (statusCode <= 0) {
        dbg("sendValue: transport error " + String(statusCode));
        return ERR_CONNECTION_FAILED;
    }

    dbg("sendValue: " + String(type) + "=" + String(value, 2) + " -> HTTP " + String(statusCode));

    if (statusCode == 202 && response.length() > 0) {
        handleResponse(response);
    }
    return statusCode;
}

/**
 * Response format:
 *   {"commands":[{"id":"...","command":"lamp","type":"BOOL","value":true}]}
 * "type" is missing for untyped commands from older app versions.
 *
 * The answer to the backend (confirmed and declined IDs) goes out BEFORE any
 * callback runs, so a long-running handler (e.g. a pump with delay) cannot
 * push it outside the backend's replay window.
 */
void HydroNode::handleResponse(const String& response) {
    if (response.length() > jsonBufferSize_) {
        dbg("handleResponse: response of " + String(response.length()) + " bytes exceeds the limit, consider setJsonBufferSize()");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        dbg("handleResponse: JSON parse failed (" + String(err.c_str()) + ")");
        return;
    }

    JsonArray commands = doc["commands"].as<JsonArray>();
    if (commands.isNull() || commands.size() == 0) {
        return;
    }

    // 1. Decide per command: run it, or decline it with a reason.
    JsonDocument answer;
    JsonArray accepted = answer["accepted"].to<JsonArray>();
    JsonArray declined = answer["declined"].to<JsonArray>();
    std::vector<std::pair<JsonObject, const Handler*>> toRun;

    for (JsonObject cmd : commands) {
        const char* id = cmd["id"];
        const char* key = cmd["command"];
        if (!id || !key) continue;

        const char* reason = nullptr;
        auto it = handlers_.find(String(key));
        if (it == handlers_.end()) {
            reason = "NO_HANDLER";
        } else {
            reason = checkCommand(it->second, cmd["type"], cmd["value"]);
        }

        if (reason) {
            dbg("command '" + String(key) + "' declined: " + reason);
            JsonObject entry = declined.add<JsonObject>();
            entry["id"] = id;
            entry["reason"] = reason;
        } else {
            accepted.add(id);
            toRun.emplace_back(cmd, &it->second);
        }
    }

    // 2. Tell the backend, then run the accepted callbacks in delivery order.
    if (accepted.size() > 0 || declined.size() > 0) {
        sendAck(accepted, declined);
    }
    for (auto& entry : toRun) {
        dbg("command: " + String(entry.first["command"].as<const char*>()));
        entry.second->fn(entry.first["value"]);
    }
}

/**
 * Returns nullptr if the handler can take the command, otherwise the decline
 * reason. A typed command must name exactly the handler's type. An untyped
 * command (older app versions) is accepted when the value itself fits.
 */
const char* HydroNode::checkCommand(const Handler& handler, const char* wireType, JsonVariant value) {
    static const char* const NAMES[] = {"ANY", "BOOL", "INT32", "UINT32", "INT64", "UINT64", "STRING"};

    if (handler.type == ValueType::ANY) {
        return nullptr;
    }
    const char* expected = NAMES[static_cast<uint8_t>(handler.type)];
    if (wireType && strcmp(wireType, expected) != 0) {
        return "TYPE_MISMATCH";
    }

    bool fits = false;
    switch (handler.type) {
        case ValueType::BOOL:   fits = value.is<bool>(); break;
        case ValueType::INT32:  fits = value.is<int32_t>(); break;
        case ValueType::UINT32: fits = value.is<uint32_t>(); break;
        case ValueType::INT64:  fits = value.is<int64_t>(); break;
        case ValueType::UINT64: fits = value.is<uint64_t>(); break;
        case ValueType::STRING: fits = value.is<const char*>(); break;
        case ValueType::ANY:    fits = true; break;
    }
    if (fits) {
        return nullptr;
    }
    // With a matching declared type the value itself is wrong; without a
    // declared type we cannot tell, so it counts as the wrong type.
    return wireType ? "INVALID_VALUE" : "TYPE_MISMATCH";
}

bool HydroNode::sendAck(JsonArrayConst accepted, JsonArrayConst declined) {
    JsonDocument doc;
    doc["sensorId"] = sensorId_;
    doc["commandIds"] = accepted;
    if (declined.size() > 0) {
        doc["declined"] = declined;
    }
    String payload;
    serializeJson(doc, payload);

    unsigned long epoch = timeClient_.getEpochTime();
    String response;
    int statusCode = postSigned(ackPath_, payload, epoch, response);
    if (statusCode != 200) {
        dbg("sendAck: failed with " + String(statusCode));
        return false;
    }
    dbg("sendAck: " + String(accepted.size()) + " confirmed, " + String(declined.size()) + " declined");
    return true;
}

/** POST a payload with HMAC headers (signature = HMAC(payload + epoch)). */
int HydroNode::postSigned(const char* path, const String& payload, unsigned long epoch, String& responseOut) {
    String signature = sign(payload + String(epoch));
    if (signature.length() == 0) {
        return ERR_CONNECTION_FAILED;
    }

#if defined(ESP8266)
    // BearSSL: parse the root bundle once, it stays valid for the lifetime of the sketch.
    static BearSSL::X509List trust(HYDRONODE_CA_BUNDLE);
    BearSSL::WiFiClientSecure client;
    client.setTrustAnchors(&trust);
    client.setX509Time(epoch);
#else
    WiFiClientSecure client;
    client.setCACert(HYDRONODE_CA_BUNDLE);
#endif

    HttpClient http(client, host_, httpsPort_);
    http.setHttpResponseTimeout(httpTimeoutMs_);
    http.beginRequest();
    http.post(path);
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("X-Sensor-Id", sensorId_);
    http.sendHeader("X-Timestamp", String(epoch));
    http.sendHeader("X-Signature", signature);
    http.sendHeader("Content-Length", payload.length());
    http.beginBody();
    http.print(payload);
    http.endRequest();

    int statusCode = http.responseStatusCode();
    responseOut = http.responseBody();
    http.stop();
    return statusCode;
}

bool HydroNode::isValidType(const char* type) {
    // Mirrors the backend rule [A-Z][A-Z0-9_]{0,63}.
    if (!type || type[0] < 'A' || type[0] > 'Z') return false;
    size_t len = 1;
    for (const char* c = type + 1; *c; c++, len++) {
        bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!ok || len >= 64) return false;
    }
    return true;
}

/** Base64(HMAC-SHA256(secretKey, message)), computed with the TLS library built into the core. */
String HydroNode::sign(const String& message) {
    uint8_t mac[32];
#if defined(ESP8266)
    br_hmac_key_context key;
    br_hmac_context ctx;
    br_hmac_key_init(&key, &br_sha256_vtable, secretKey_, strlen(secretKey_));
    br_hmac_init(&ctx, &key, 0);
    br_hmac_update(&ctx, message.c_str(), message.length());
    br_hmac_out(&ctx, mac);
    String encoded = base64::encode(mac, sizeof(mac), false);
    memset(mac, 0, sizeof(mac));
    memset(&key, 0, sizeof(key));
    return encoded;
#else
    const mbedtls_md_info_t* sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = mbedtls_md_hmac(
        sha256,
        (const unsigned char*)secretKey_, strlen(secretKey_),
        (const unsigned char*)message.c_str(), message.length(),
        mac
    );
    if (rc != 0) {
        dbg("sign: HMAC failed (" + String(rc) + ")");
        return String();
    }

    unsigned char encoded[45];  // 32 bytes -> 44 Base64 characters + terminator
    size_t written = 0;
    rc = mbedtls_base64_encode(encoded, sizeof(encoded), &written, mac, sizeof(mac));
    memset(mac, 0, sizeof(mac));
    if (rc != 0) {
        dbg("sign: Base64 failed (" + String(rc) + ")");
        return String();
    }
    return String((const char*)encoded);
#endif
}

void HydroNode::dbg(const String& msg) {
    if (debug_) {
        debug_->println("[HydroNode] " + msg);
    }
}
