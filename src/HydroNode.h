#pragma once

#if !defined(ESP32)
#error "HydroNode-Library requires an ESP32 board (it uses WiFiClientSecure::setCACert for TLS)."
#endif

#include <Arduino.h>
#include <map>
#include <functional>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoHttpClient.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "HydroNodeCerts.h"

/**
 * HydroNode — Arduino client for the HydroNode IoT backend.
 *
 * Sends HMAC-SHA256-signed sensor values over HTTPS and dispatches
 * backend commands (JSON response keys) to registered callbacks.
 *
 * Typical usage:
 *
 *   HydroNode hydro("sensor-id", "secret-key");
 *
 *   void setup() {
 *       hydro.connectWiFi("ssid", "password");   // or manage WiFi yourself
 *       hydro.begin();
 *   }
 *
 *   void loop() {
 *       hydro.sendValue("TEMPERATURE", 21.5);
 *       delay(10000);   // backend enforces min. 9 s between submissions
 *   }
 */
class HydroNode {
public:
    // Error codes returned by sendValue() (positive values are HTTP status codes).
    static constexpr int ERR_WIFI_DISCONNECTED = -1;  // WiFi not connected
    static constexpr int ERR_TIME_NOT_SYNCED   = -2;  // NTP sync failed (signature would be rejected)
    static constexpr int ERR_CONNECTION_FAILED = -3;  // TLS/TCP connection or HTTP transport error

    HydroNode(
        const char* sensorId,
        const char* secretKey,
        const char* host = "hydronode.texhfexlabs.de",
        const char* path = "/api/webhook/sensor-value"
    );

    /**
     * Initialize the NTP client. Call once in setup(), after WiFi is connected.
     */
    void begin();

    /**
     * Convenience WiFi setup: connects in station mode and blocks until
     * connected or the timeout expires. Entirely optional — if you manage
     * WiFi yourself (WiFiManager, ESP-IDF, custom reconnect logic), simply
     * don't call it.
     *
     * @return true if connected, false on timeout.
     */
    bool connectWiFi(const char* ssid, const char* password, uint32_t timeoutMs = 30000);

    /**
     * Send one measurement to the backend.
     *
     * @param type  Sensor type, e.g. "TEMPERATURE", "HUMIDITY", "SOIL_MOISTURE".
     * @param value Measured value (transmitted with 2 decimal places).
     * @return HTTP status code (202 = accepted) or a negative ERR_* code.
     *
     * Note: the backend rate-limits to one submission per sensor per 9 seconds.
     * Space out consecutive calls by at least 10 seconds.
     */
    int sendValue(const char* type, float value);

    /**
     * Register a callback for a backend command key, e.g.
     * hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));
     *
     * Commands are delivered in the response of sendValue() as
     * {"commands":[{"id":"...","command":"pump","value":4000}]}.
     * The library acknowledges receipt to the backend (signed) before
     * dispatching, so the HydroNode app shows commands as confirmed.
     */
    void on(const String& key, std::function<void(JsonVariant)> handler);

    /** Captive-portal AP name for WiFiManager: "HydroNode-Setup-<last 4 of sensor id>". */
    String getApName() const;

    /** Buffer size for parsing backend responses (default 1024 bytes). */
    void setJsonBufferSize(size_t size);

    /** HTTP response timeout in milliseconds (default 10000). */
    void setHttpTimeout(uint32_t ms);

    /** Enable debug logging, e.g. hydro.setDebug(Serial). */
    void setDebug(Stream& stream);

    /** Wraps a plain function into a JsonVariant handler with automatic type conversion. */
    template<typename T>
    static std::function<void(JsonVariant)> bindCallback(void (*fn)(T)) {
        return [fn](JsonVariant v) { fn(v.as<T>()); };
    }

private:
    const char* sensorId_;
    const char* secretKey_;
    const char* host_;
    const char* path_;
    const char* ackPath_ = "/api/webhook/sensor-command-ack";
    size_t jsonBufferSize_ = 1024;
    uint32_t httpTimeoutMs_ = 10000;
    int httpsPort_ = 443;
    Stream* debug_ = nullptr;

    std::map<String, std::function<void(JsonVariant)>> handlers_;

    WiFiUDP ntpUDP_;
    NTPClient timeClient_;

    bool ensureTimeSynced(unsigned long& epochOut);
    void hmac_sha256(const uint8_t* key, size_t keylen, const uint8_t* data, size_t datalen, uint8_t* out);
    String toBase64(const uint8_t* input, size_t len);
    String sign(const String& message);
    int postSigned(const char* path, const String& payload, unsigned long epoch, String& responseOut);
    void handleResponse(const String& response);
    bool sendAck(const String& commandIdsJson);
    void dbg(const String& msg);
};
