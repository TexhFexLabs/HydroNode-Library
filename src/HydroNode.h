#pragma once

#if !defined(ESP32) && !defined(ESP8266)
#error "HydroNode-Library requires an ESP32 or ESP8266 board (it needs TLS with certificate validation)."
#endif

#include <Arduino.h>
#include <map>
#include <vector>
#include <functional>
#include <ArduinoJson.h>
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif
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
     * Command callbacks. Register one per command name and value type; the
     * type must match what you pick in the HydroNode app. One name may carry
     * several types, each with its own callback:
     *
     *   hydro.onBool("lamp", [](bool on) { digitalWrite(LAMP_PIN, on); });
     *   hydro.onUInt32("lamp", [](uint32_t ms) { pulse(LAMP_PIN, ms); });
     *   hydro.onUInt32("co2_calibration", [](uint32_t v) { sensor.calibrate(v); });
     *
     * Commands arrive in the response of sendValue(). Before any callback
     * runs, the library answers the backend (signed): commands with a
     * matching callback are confirmed, all others are declined with a reason
     * (NO_HANDLER, TYPE_MISMATCH, INVALID_VALUE) that the app shows.
     */
    void onBool(const String& key, std::function<void(bool)> handler);
    void onInt32(const String& key, std::function<void(int32_t)> handler);
    void onUInt32(const String& key, std::function<void(uint32_t)> handler);
    void onInt64(const String& key, std::function<void(int64_t)> handler);
    void onUInt64(const String& key, std::function<void(uint64_t)> handler);
    void onString(const String& key, std::function<void(const String&)> handler);

    /**
     * Untyped callback that receives the raw JSON value of any type, e.g. for
     * commands that accept several types. Also keeps sketches written for
     * library versions up to 1.2.0 working:
     *   hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));
     */
    void on(const String& key, std::function<void(JsonVariant)> handler);

    /**
     * Fetches the time from NTP now. sendValue() syncs on its own; call this when the clock
     * stood still meanwhile, as it does in ESP8266 light sleep, so the next value is signed
     * with the right time. Needs WiFi. Returns false when no time server answered.
     */
    bool syncTime();

    /**
     * Milliseconds since 1970 from the last NTP sync (seconds resolution at the sync), 0
     * before the first one. Runs on millis() in between.
     */
    uint64_t epochMs() const;

    /**
     * Closes the TLS connection that sendValue() keeps open between calls. Values sent in a
     * row share one connection (one handshake); close it after the last one, before the board
     * sleeps or waits for a long time. The next sendValue() simply connects again.
     */
    void closeConnection();

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

    /**
     * Wraps a plain function into an untyped handler with ArduinoJson's lenient
     * conversion. Kept for sketches from version 1.2.0 and older; new code
     * should use the typed onBool(), onUInt32(), ... which reject wrong types.
     */
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
    Stream* debug_ = nullptr;

#if defined(ESP8266)
    BearSSL::WiFiClientSecure tls_;
#else
    WiFiClientSecure tls_;
#endif
    HttpClient http_;
    bool tlsReady_ = false;

    enum class ValueType : uint8_t { ANY, BOOL, INT32, UINT32, INT64, UINT64, STRING };

    struct Handler {
        ValueType type;
        std::function<void(JsonVariant)> fn;
    };

    std::map<String, std::vector<Handler>> handlers_;

    WiFiUDP ntpUDP_;
    NTPClient timeClient_;

    bool ensureTimeSynced(unsigned long& epochOut);
    bool forceSync();
    void noteSync();
    unsigned long syncedEpoch_ = 0;
    uint32_t syncedAtMs_ = 0;
    static bool isValidType(const char* type);
    String sign(const String& message);
    int postSigned(const char* path, const String& payload, unsigned long epoch, String& responseOut);
    void prepareTls(unsigned long epoch);
    void handleResponse(const String& response);
    void addHandler(const String& key, ValueType type, std::function<void(JsonVariant)> fn);
    const char* pickHandler(const String& key, const char* wireType, JsonVariant value, const Handler*& out) const;
    static const char* typeName(ValueType type);
    static const char* checkCommand(const Handler& handler, const char* wireType, JsonVariant value);
    bool sendAck(JsonArrayConst accepted, JsonArrayConst declined);
    void dbg(const String& msg);
};
