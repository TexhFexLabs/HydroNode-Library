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
    /** Library version, sent as `hydronode-lib/<version>` unless setFirmwareIdentity() says otherwise. */
    static constexpr const char* LIB_VERSION = "1.6.0";

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

    // --- Fleet and OTA hooks (1.6.0) ---------------------------------------------------------
    //
    // Every request carries two headers the HydroNode fleet view reads:
    //   X-Firmware:      hydronode-lib/1.6.0 esp32c3
    //   X-Device-Status: boot=12;reset=poweron;uptime=45;rssi=-61;net=wifi;readErr=
    // A sketch built on the library needs nothing for that. The universal HydroNode firmware uses
    // the rest of this block for updates over the air.

    /**
     * Replaces the default `hydronode-lib/<version>` in X-Firmware, e.g.
     *   hydro.setFirmwareIdentity("hydronode", "0.5.0", "ota cfg=14");
     * gives `hydronode/0.5.0 esp32c3 ota cfg=14`. The chip family is always added by the library.
     */
    void setFirmwareIdentity(const char* product, const char* version, const char* flags = nullptr);

    /** The X-Firmware value sent with every request (at most 128 characters). */
    String firmwareHeader() const;

    /**
     * The X-Device-Status value: boot counter (cold starts only), reset reason, uptime in seconds,
     * WiFi RSSI, network and the drivers that failed to read since clearReadErrors().
     */
    String deviceStatusHeader();

    /** Marks a driver whose reading failed this round, e.g. reportReadError("bme280"). */
    void reportReadError(const char* driverId);

    /** Starts a new round without read errors. */
    void clearReadErrors();

    /**
     * Reports this reset reason instead of the chip's own, e.g. "ota" after restarting into new
     * firmware. One of poweron, software, panic, watchdog, brownout, deepsleep, external, ota,
     * unknown. Pass nullptr to go back to the chip's reason.
     */
    void setResetReason(const char* reason);

    /**
     * An extra header sent with every request until cleared, e.g. X-Ota-State while new firmware
     * proves itself. Setting the same name again replaces the value.
     */
    void setExtraHeader(const char* name, const String& value);
    void clearExtraHeader(const char* name);

    /**
     * Callback for a key in the backend's answer to sendValue() other than "commands", e.g.
     * "ota" or "config". Runs after the commands of the same answer. Unknown keys are ignored.
     */
    void onResponseKey(const char* key, std::function<void(JsonVariantConst)> handler);

    /**
     * Reports the outcome of an update job, signed like every request:
     * POST /api/webhook/sensor-ota-ack {"job":"...","result":"downloaded","reason":"..."}.
     * result: downloaded, verified, failed or config_applied. Returns true on a 2xx answer.
     */
    bool sendOtaAck(const char* job, const char* result, const char* reason = nullptr);

    /** Gets the bytes of a download in order. Return false to stop. */
    using ChunkHandler = std::function<bool(const uint8_t* data, size_t length)>;

    struct DownloadResult {
        int status;    // HTTP status (200/206) or a negative ERR_* code
        size_t bytes;  // bytes handed to the callback in this call
        size_t total;  // full size from Content-Range/Content-Length, 0 if unknown
    };

    /**
     * Signed GET of a file from the backend, e.g. a firmware image: the signature covers the
     * path with query and the timestamp. With offset > 0 it resumes with "Range: bytes=<offset>-".
     * The bytes stream to `onChunk` in pieces of up to 1 KB; nothing is buffered as a whole.
     */
    DownloadResult downloadSigned(const char* pathAndQuery, size_t offset, ChunkHandler onChunk);

    /** Chip family as the backend knows it: esp32, esp32s2, esp32s3, esp32c3, esp32c6, esp8266. */
    static const char* family();

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
    const char* otaAckPath_ = "/api/webhook/sensor-ota-ack";
    String product_ = "hydronode-lib";
    String version_ = LIB_VERSION;
    String flags_;
    String resetOverride_;
    std::vector<String> readErrors_;
    std::vector<std::pair<String, String>> extraHeaders_;
    std::map<String, std::function<void(JsonVariantConst)>> responseHandlers_;
    uint32_t bootCount_ = 0;
    bool bootCounted_ = false;
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
    void sendDeviceHeaders();
    uint32_t bootCount();
    static const char* chipResetReason();
    void handleResponse(const String& response);
    void handleCommands(JsonArray commands);
    void addHandler(const String& key, ValueType type, std::function<void(JsonVariant)> fn);
    const char* pickHandler(const String& key, const char* wireType, JsonVariant value, const Handler*& out) const;
    static const char* typeName(ValueType type);
    static const char* checkCommand(const Handler& handler, const char* wireType, JsonVariant value);
    bool sendAck(JsonArrayConst accepted, JsonArrayConst declined);
    void dbg(const String& msg);
};
