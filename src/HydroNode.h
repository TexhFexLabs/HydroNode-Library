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
 *       delay(10000);   // backend accepts one value per type every 10 s
 *   }
 */
class HydroNode {
public:
    // Error codes returned by sendValue() (positive values are HTTP status codes).
    static constexpr int ERR_WIFI_DISCONNECTED = -1;  // WiFi not connected
    static constexpr int ERR_TIME_NOT_SYNCED   = -2;  // NTP sync failed (signature would be rejected)
    static constexpr int ERR_CONNECTION_FAILED = -3;  // TLS/TCP connection or HTTP transport error
    static constexpr int ERR_INVALID_TYPE      = -4;  // type is not A-Z, 0-9, _ starting with a letter
    static constexpr int ERR_INVALID_VALUE     = -5;  // value is NaN or infinite

    HydroNode(
        const char* sensorId,
        const char* secretKey,
        const char* host = "hydronode.tech",
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
     * @param type  Sensor type in upper case, e.g. "TEMPERATURE", "HUMIDITY",
     *              "SOIL_MOISTURE". Allowed: A-Z, 0-9 and _, starting with a
     *              letter, at most 64 characters.
     * @param value Measured value (transmitted with 2 decimal places).
     * @return HTTP status code (202 = accepted) or a negative ERR_* code.
     *
     * Note: the backend accepts one value per sensor and type every 10 seconds.
     * Different types may be sent right after each other.
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

    /**
     * Largest backend response in bytes the library will parse (default 8192).
     * Larger responses are ignored. The default fits the backend maximum of
     * 8 commands per delivery, so there is normally no need to change it.
     */
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
    size_t jsonBufferSize_ = 8192;
    uint32_t httpTimeoutMs_ = 10000;
    int httpsPort_ = 443;
    Stream* debug_ = nullptr;

    std::map<String, std::function<void(JsonVariant)>> handlers_;

    WiFiUDP ntpUDP_;
    NTPClient timeClient_;

    bool ensureTimeSynced(unsigned long& epochOut);
    static bool isValidType(const char* type);
    String sign(const String& message);
    int postSigned(const char* path, const String& payload, unsigned long epoch, String& responseOut);
    void handleResponse(const String& response);
    bool sendAck(const String& commandIdsJson);
    void dbg(const String& msg);
};
