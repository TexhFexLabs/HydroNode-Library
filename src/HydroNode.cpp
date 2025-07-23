#include "HydroNode.h"
#include <base64.hpp>
#include <SHA256.h>

HydroNode::HydroNode(const char* sensorId, const char* secretKey, const char* host, const char* path)
    : sensorId_(sensorId), secretKey_(secretKey), host_(host), path_(path)
{
    timeClient_ = new NTPClient(ntpUDP_, "pool.ntp.org");
}

void HydroNode::begin() {
    timeClient_->begin();
    timeClient_->setTimeOffset(0);
}

String HydroNode::getApName() const {
    String id(sensorId_);
    String suffix = id.substring(id.length() - 4);
    return "HydroNode-Setup-" + suffix;
}

void HydroNode::on(const String& key, std::function<void(JsonVariant)> handler) {
    handlers_[key] = handler;
}

void HydroNode::sendValue(const char* type, float value) {
    if (!timeClient_->update()) timeClient_->forceUpdate();
    unsigned long epoch = timeClient_->getEpochTime();

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
    //client.setInsecure();

    HttpClient http(client, host_, httpsPort_);
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

    if (statusCode == 200 && response.length() > 0) {
        handleResponse(response);
    }
}

void HydroNode::setJsonBufferSize(size_t size) {
    jsonBufferSize_ = size;
}

void HydroNode::handleResponse(const String& response) {
    DynamicJsonDocument doc(jsonBufferSize_);
    DeserializationError err = deserializeJson(doc, response);
    if (err) return;

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

String HydroNode::toBase64(uint8_t* input, size_t len) {
    unsigned char encoded[48] = {0};
    encode_base64(input, len, encoded);
    return String((char*)encoded);
}
