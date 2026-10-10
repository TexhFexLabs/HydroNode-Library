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
#include <WiFiUdp.h>
#include "HydroNodeCerts.h"
#include "HydroNodeDeviceConfig.h"
#include "HydroNodeBatteryGuard.h"

/** One measurement for HydroNode::sendValues(): sensor type (e.g. "TEMPERATURE") and value. */
struct HydroNodeValue {
    const char* type;
    float value;
};

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
 *
 * Several values of one reading go out in one request:
 *
 *   HydroNodeValue v[] = {{"TEMPERATURE", t}, {"HUMIDITY", h}};
 *   hydro.sendValues(v, 2);
 */
class HydroNode {
public:
    /** Library version, sent as `hydronode-lib/<version>` unless setFirmwareIdentity() says otherwise. */
    static constexpr const char* LIB_VERSION = "1.8.0";

    // Error codes returned by sendValue() (positive values are HTTP status codes).
    static constexpr int ERR_WIFI_DISCONNECTED = -1;  // WiFi not connected
    static constexpr int ERR_TIME_NOT_SYNCED   = -2;  // no valid time, NTP failed (signature would be rejected)
    static constexpr int ERR_CONNECTION_FAILED = -3;  // TLS/TCP connection or HTTP transport error
    static constexpr int ERR_INVALID_TYPE      = -4;  // type is not A-Z, 0-9, _ starting with a letter
    static constexpr int ERR_INVALID_VALUE     = -5;  // value is NaN or infinite

    /** Most values sendValues() puts into one request; further ones are skipped (ERR_INVALID_VALUE). */
    static constexpr size_t MAX_VALUES_PER_REQUEST = 64;

    HydroNode(
        const char* sensorId,
        const char* secretKey,
        const char* host = "hydronode.tech",
        const char* path = "/api/webhook/sensor-value"
    );

    /**
     * Call once in setup(). Since 1.7.0 there is nothing to start: the time is fetched on demand
     * before the first request. Kept so existing sketches stay unchanged.
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
     * Sends all values in one request: they arrive together and carry one timestamp, the time they were
     * measured (`ageMs` milliseconds before the call; 0 = now). Returns the HTTP status (202 when at
     * least one value was accepted) or a negative ERR_*. `codes` (optional, `count` entries) gets per
     * value 202, the status the server gave that value (429 rate limit, 400 invalid, 503), a local
     * ERR_INVALID_TYPE / ERR_INVALID_VALUE (value skipped, not sent), or the request's status.
     *
     *   HydroNodeValue v[] = {{"TEMPERATURE", t}, {"HUMIDITY", h}};
     *   int codes[2];
     *   hydro.sendValues(v, 2, codes);
     *
     * Each type may appear once (later duplicates: ERR_INVALID_TYPE), at most MAX_VALUES_PER_REQUEST
     * values per call. Commands and other answer keys arrive as with sendValue(). A backend older
     * than the batch endpoint gets one sendValue() per value instead (signed with the current time).
     */
    int sendValues(const HydroNodeValue* values, size_t count, int* codes = nullptr, uint32_t ageMs = 0);

    /**
     * Command callbacks. Register one per command name and value type; the
     * type must match what you pick in the HydroNode app. One name may carry
     * several types, each with its own callback:
     *
     *   hydro.onBool("lamp", [](bool on) { digitalWrite(LAMP_PIN, on); });
     *   hydro.onUInt32("lamp", [](uint32_t ms) { pulse(LAMP_PIN, ms); });
     *   hydro.onUInt32("co2_calibration", [](uint32_t v) { sensor.calibrate(v); });
     *
     * Commands arrive in the response of sendValue() and sendValues(). Before any callback
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
     * Fetches the time from NTP now and sets the system clock (millisecond precision). Sending
     * syncs on its own when the clock is not valid or the last sync is older than the resync
     * interval; call this when the clock stood still meanwhile, as it does in ESP8266 light
     * sleep, so the next value is signed with the right time. Needs WiFi. Returns false when no
     * time server answered.
     */
    bool syncTime();

    /**
     * Milliseconds since 1970 from the system clock, 0 while it is not valid (no sync yet).
     * On the ESP32 the clock keeps running through deep sleep, on the ESP8266 it starts again
     * at every boot.
     */
    uint64_t epochMs() const;

    /**
     * How old the last time sync may get before a request fetches the time again (default 1800 s).
     * The clock drifts slowly, mostly in ESP32 deep sleep; a timestamp the backend rejects
     * triggers a resync and one retry anyway. 0 syncs before every request.
     */
    void setTimeResyncInterval(uint32_t seconds);

    /**
     * Closes the TLS connection that sendValue() and sendValues() keep open between calls. Values sent in a
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
    //   X-Firmware:      hydronode-lib/1.8.0 esp32c3
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
     * WiFi RSSI, network, the battery state from setPowerState() (pwr=) and the drivers that
     * failed to read since clearReadErrors().
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
     * Callback for a key in the backend's answer to sendValue() or sendValues() other than
     * "commands", e.g.
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

    // --- Device settings (1.8.0) -----------------------------------------------------------
    //
    // Send interval and battery thresholds the board runs. HydroNode shows them in the sensor
    // settings ("On the device") and can send changed values back:
    //   X-Device-Config: v=1 rev=7 int=300 save=3500 rec=3300 sby=3200 res=3600 src=bat cells=1 pwr=normal
    // goes out with the first request after boot, after setDeviceConfig() with other values and
    // after onSettings() took or refused a change. See examples/PowerThresholds.

    /**
     * The values this board runs. Strings are copied. Sent with the next request when they
     * differ from the last ones (always with the first request after boot).
     */
    void setDeviceConfig(const HydroNodeDeviceConfig& config);

    /** The values last set or taken from HydroNode. */
    const HydroNodeDeviceConfig& deviceConfig() const { return config_; }

    /**
     * Battery state: normal, save, recovery or standby (HydroNodeBatteryGuard::stateName()).
     * Goes out as pwr= in X-Device-Status with every request and in X-Device-Config.
     * nullptr leaves it out.
     */
    void setPowerState(const char* state);

    /**
     * Lets HydroNode change the values above: the answer key "settings" arrives as
     * HydroNodeSettings (pack mV, values it leaves out keep the current ones). Return true when the
     * board took them (store them, e.g. in Preferences): the library keeps them with their
     * revision and reports them with the next request. Return false to refuse; HydroNode shows
     * the change as failed. Settings that break the rules (interval 10 to 604800 s, gaps
     * between the thresholds) are refused before the callback. One revision runs once.
     * Registering a callback adds caps=settings to X-Device-Config.
     */
    void onSettings(std::function<bool(const HydroNodeSettings&)> handler);

    /** The X-Device-Config value the next request would carry. */
    String deviceConfigHeader() const;

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
    const char* batchPath_ = "/api/webhook/sensor-values";
    const char* ackPath_ = "/api/webhook/sensor-command-ack";
    const char* otaAckPath_ = "/api/webhook/sensor-ota-ack";
    String product_ = "hydronode-lib";
    String version_ = LIB_VERSION;
    String flags_;
    String resetOverride_;
    std::vector<String> readErrors_;
    std::vector<std::pair<String, String>> extraHeaders_;
    std::map<String, std::function<void(JsonVariantConst)>> responseHandlers_;
    HydroNodeDeviceConfig config_;
    String configSource_;
    String configGauge_;
    String configError_;
    String powerState_;
    bool configSet_ = false;
    bool configDue_ = true;          // send X-Device-Config with the next request
    bool configInFlight_ = false;    // the request being sent carries it
    int32_t rejectedRevision_ = -1;  // rej= with the next X-Device-Config
    const char* rejectError_ = nullptr;
    int32_t lastSettingsRevision_ = -1;
    std::function<bool(const HydroNodeSettings&)> settingsHandler_;
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
    bool batchUnsupported_ = false;  // the backend answered the batch path with 404/405

    enum class ValueType : uint8_t { ANY, BOOL, INT32, UINT32, INT64, UINT64, STRING };

    struct Handler {
        ValueType type;
        std::function<void(JsonVariant)> fn;
    };

    std::map<String, std::vector<Handler>> handlers_;

    WiFiUDP ntpUDP_;
    uint32_t resyncIntervalS_ = 1800;
    bool syncFailed_ = false;      // the last sync attempt got no answer ...
    uint32_t syncFailedAtMs_ = 0;  // ... at this millis(), see ensureTimeSynced()

    bool ensureTimeSynced(unsigned long& epochOut);
    bool forceSync();
    bool sntpQuery(const char* server);
    bool resyncAfterRejection(int statusCode, const String& response);
    unsigned long measuredAt(uint32_t ageMs) const;
    static bool isValidType(const char* type);
    String sign(const String& message);
    String valuePayload(const char* type, float value, unsigned long epoch) const;
    String batchPayload(const HydroNodeValue* values, const std::vector<size_t>& sent, unsigned long epoch) const;
    int sendEach(const HydroNodeValue* values, const std::vector<size_t>& sent, int* codes);
    int postSigned(const char* path, const String& payload, unsigned long epoch, String& responseOut);
    void prepareTls();
    void sendDeviceHeaders();
    uint32_t bootCount();
    static const char* chipResetReason();
    bool parseResponse(const String& response, JsonDocument& doc);
    void handleResponse(JsonDocument& doc);
    void handleCommands(JsonArray commands);
    void handleSettings(JsonVariantConst settings);
    void deviceHeadersDelivered();
    void addHandler(const String& key, ValueType type, std::function<void(JsonVariant)> fn);
    const char* pickHandler(const String& key, const char* wireType, JsonVariant value, const Handler*& out) const;
    static const char* typeName(ValueType type);
    static const char* checkCommand(const Handler& handler, const char* wireType, JsonVariant value);
    bool sendAck(JsonArrayConst accepted, JsonArrayConst declined);
    void dbg(const String& msg);
};
