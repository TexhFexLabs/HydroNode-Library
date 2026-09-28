#include "HydroNode.h"
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

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
    handlers_[key] = handler;
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

    // mbedTLS validates certificate notBefore/notAfter against the system
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
 * Response format: {"commands":[{"id":"...","command":"pump","value":4000}]}
 * Receipt is acknowledged to the backend BEFORE dispatching, so a
 * long-running handler (e.g. pump with delay) cannot push the ACK
 * outside the backend's replay window.
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

    // 1. Acknowledge receipt of everything we successfully parsed
    String idsJson;
    for (JsonObject cmd : commands) {
        const char* id = cmd["id"];
        if (!id) continue;
        if (idsJson.length() > 0) idsJson += ",";
        idsJson += "\"" + String(id) + "\"";
    }
    if (idsJson.length() > 0) {
        sendAck(idsJson);
    }

    // 2. Dispatch to registered handlers
    for (JsonObject cmd : commands) {
        const char* key = cmd["command"];
        if (!key) continue;
        auto it = handlers_.find(String(key));
        if (it != handlers_.end()) {
            dbg("command: " + String(key));
            it->second(cmd["value"]);
        } else {
            dbg("command: no handler registered for '" + String(key) + "'");
        }
    }
}

bool HydroNode::sendAck(const String& commandIdsJson) {
    unsigned long epoch = timeClient_.getEpochTime();
    String payload = "{\"sensorId\":\"" + String(sensorId_) + "\",\"commandIds\":[" + commandIdsJson + "]}";

    String response;
    int statusCode = postSigned(ackPath_, payload, epoch, response);
    if (statusCode != 200) {
        dbg("sendAck: failed with " + String(statusCode));
        return false;
    }
    dbg("sendAck: confirmed");
    return true;
}

/** POST a payload with HMAC headers (signature = HMAC(payload + epoch)). */
int HydroNode::postSigned(const char* path, const String& payload, unsigned long epoch, String& responseOut) {
    String signature = sign(payload + String(epoch));
    if (signature.length() == 0) {
        return ERR_CONNECTION_FAILED;
    }

    WiFiClientSecure client;
    client.setCACert(HYDRONODE_CA_BUNDLE);

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

/** Base64(HMAC-SHA256(secretKey, message)), computed with the mbedTLS built into the ESP32 core. */
String HydroNode::sign(const String& message) {
    uint8_t mac[32];
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
}

void HydroNode::dbg(const String& msg) {
    if (debug_) {
        debug_->println("[HydroNode] " + msg);
    }
}
