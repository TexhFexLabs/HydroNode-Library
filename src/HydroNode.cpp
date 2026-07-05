#include "HydroNode.h"
#include <base64.hpp>
#include <SHA256.h>
#include <time.h>
#include <sys/time.h>

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
    String msg = payload + String(epoch);

    uint8_t hmacResult[32];
    hmac_sha256(
        (const uint8_t*)secretKey_, strlen(secretKey_),
        (const uint8_t*)msg.c_str(), msg.length(),
        hmacResult
    );
    String signature = toBase64(hmacResult, 32);

    WiFiClientSecure client;
    client.setCACert(HYDRONODE_CA_BUNDLE);

    HttpClient http(client, host_, httpsPort_);
    http.setHttpResponseTimeout(httpTimeoutMs_);
    http.beginRequest();
    http.post(path_);
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("X-Sensor-Id", sensorId_);
    http.sendHeader("X-Timestamp", String(epoch));
    http.sendHeader("X-Signature", signature);
    http.sendHeader("Content-Length", payload.length());
    http.beginBody();
    http.print(payload);
    http.endRequest();

    int statusCode = http.responseStatusCode();
    String response = http.responseBody();
    http.stop();

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

void HydroNode::handleResponse(const String& response) {
    DynamicJsonDocument doc(jsonBufferSize_);
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        dbg("handleResponse: JSON parse failed (" + String(err.c_str()) + "), consider setJsonBufferSize()");
        return;
    }

    for (JsonPair kv : doc.as<JsonObject>()) {
        String key = kv.key().c_str();
        auto it = handlers_.find(key);
        if (it != handlers_.end()) {
            it->second(kv.value());
        }
    }
}

void HydroNode::hmac_sha256(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t* out) {
    const uint8_t blocksize = 64;
    uint8_t keyblock[blocksize] = {0};
    if (keylen > blocksize) {
        SHA256 hasher;
        hasher.reset();
        hasher.update(key, keylen);
        hasher.finalize(keyblock, 32);
    } else {
        memcpy(keyblock, key, keylen);
    }
    uint8_t o_key_pad[blocksize], i_key_pad[blocksize];
    for (uint8_t i = 0; i < blocksize; i++) {
        o_key_pad[i] = keyblock[i] ^ 0x5c;
        i_key_pad[i] = keyblock[i] ^ 0x36;
    }
    SHA256 inner;
    inner.reset();
    inner.update(i_key_pad, blocksize);
    inner.update(data, datalen);
    uint8_t innerhash[32];
    inner.finalize(innerhash, 32);
    SHA256 outer;
    outer.reset();
    outer.update(o_key_pad, blocksize);
    outer.update(innerhash, 32);
    outer.finalize(out, 32);
}

String HydroNode::toBase64(const uint8_t* input, size_t len) {
    unsigned char encoded[48] = {0};
    encode_base64(const_cast<uint8_t*>(input), len, encoded);
    return String((char*)encoded);
}

void HydroNode::dbg(const String& msg) {
    if (debug_) {
        debug_->println("[HydroNode] " + msg);
    }
}
