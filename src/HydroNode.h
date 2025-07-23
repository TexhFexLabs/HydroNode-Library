#pragma once
#include <Arduino.h>
#include <map>
#include <functional>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <ArduinoHttpClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

class HydroNode {
public:
    HydroNode(
        const char* sensorId,
        const char* secretKey,
        const char* host = "hydronode.texhfexlabs.de",
        const char* path = "/api/webhook/sensor-value"
    );

    void begin();
    void on(const String& key, std::function<void(JsonVariant)> handler);
    void sendValue(const char* type, float value);
    String getApName() const;
    void setJsonBufferSize(size_t size);
    template<typename T>
    static std::function<void(JsonVariant)> bindCallback(void (*fn)(T)) {
        return [fn](JsonVariant v) { fn(v.as<T>()); };
    }

private:
    const char* sensorId_;
    const char* secretKey_;
    const char* host_;
    const char* path_;
    size_t jsonBufferSize_ = 128;
    int httpsPort_ = 443;

    std::map<String, std::function<void(JsonVariant)>> handlers_;

    WiFiUDP ntpUDP_;
    NTPClient* timeClient_;

    void hmac_sha256(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t* out);
    String toBase64(uint8_t* input, size_t len);
    void handleResponse(const String& response);
};
