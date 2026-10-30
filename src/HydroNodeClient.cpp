#include "HydroNodeClient.h"
#include "HydroNodeCrypto.h"
#include <math.h>
#include <string.h>
#include <vector>

#if defined(ESP32) || defined(ESP8266)
#define HYDRONODE_SYSTEM_CLOCK 1
#include <time.h>
#include <sys/time.h>
#endif
#if defined(ESP8266)
#include <user_interface.h>
#elif defined(ESP32)
#include <esp_system.h>
#include <Preferences.h>
#endif

// Header values are limited so a long flag list or many failing drivers cannot grow a request.
static constexpr size_t MAX_FIRMWARE_HEADER = 128;
static constexpr size_t MAX_STATUS_HEADER = 256;
static constexpr size_t MAX_READ_ERRORS = 16;

// ESP8266 boot counter: two words at the end of the 512-byte RTC user memory. They survive a
// restart and deep sleep, not a power cut; the reset reason tells the backend which one it was.
static constexpr uint32_t RTC_BOOT_SLOT = 126;
static constexpr uint32_t RTC_BOOT_MAGIC = 0x484E4254;  // "HNBT"

// A round older than this goes through the buffer instead of the live endpoint, whose signed
// timestamp has to be within 2 minutes of the server clock.
static constexpr uint32_t LIVE_MAX_AGE_MS = 90000;

/** Drops characters that would end a header line or a field of it. */
static String headerSafe(const String& value, bool allowSpaces) {
    String out;
    out.reserve(value.length());
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if (c == '\r' || c == '\n' || c == ';' || (c == ' ' && !allowSpaces)) continue;
        if (c < 0x20 || c == 0x7f) continue;
        out += c;
    }
    return out;
}

// Epoch sanity floor: any synced clock is past 2020-09-13. Values below
// mean the time sync never happened and the clock is still at 1970.
static constexpr uint32_t MIN_VALID_EPOCH = 1600000000UL;

// SNTP (RFC 4330): one 48-byte request per server, an answer within a second or the next server.
static constexpr uint16_t NTP_PORT = 123;
static constexpr uint32_t NTP_TIMEOUT_MS = 1000;
static constexpr size_t NTP_PACKET_SIZE = 48;
// NTP counts seconds since 1900; the Unix epoch starts 70 years (2208988800 s) later.
static constexpr uint64_t NTP_UNIX_OFFSET = 2208988800ULL;
// After a sync without answer a valid clock is used as it is; the next try waits this long.
static constexpr uint32_t SYNC_RETRY_MS = 60000;

// Unix seconds of the last successful sync. On the ESP32 the system clock and this survive deep
// sleep (RTC timer and RTC memory), so a wake-up with a recent sync needs no time exchange. The
// ESP8266 clock starts at 1970 after every boot: synced per boot. Other boards keep their own
// clock: the Unix ms of the last sync and the millis() it was taken at.
#if defined(ESP32)
static RTC_DATA_ATTR int64_t lastSyncS = 0;
static RTC_DATA_ATTR uint16_t runTagValue = 0;
#else
static int64_t lastSyncS = 0;
static uint16_t runTagValue = 0;
#endif
#if !defined(HYDRONODE_SYSTEM_CLOCK)
static uint64_t clockBaseMs = 0;
static uint32_t clockBaseMillis = 0;
#endif

static uint32_t randomWord() {
#if defined(ESP8266)
    return RANDOM_REG32;
#elif defined(ESP32)
    return esp_random();
#else
    static bool seeded = false;
    if (!seeded) {
        randomSeed(micros() ^ (uint32_t)analogRead(0));
        seeded = true;
    }
    return ((uint32_t)random(0x10000) << 16) ^ (uint32_t)random(0x10000) ^ micros();
#endif
}

HydroNodeClient::HydroNodeClient(HydroNodeTransport& transport, const char* sensorId, const char* secretKey,
                                 const char* host, const char* path, uint16_t port)
    : transport_(transport), sensorId_(sensorId), secretKey_(secretKey), host_(host), path_(path), port_(port) {}

HttpClient& HydroNodeClient::http() {
    Client& client = transport_.client();
    if (!http_ || httpOn_ != &client) {
        if (http_) http_->stop();
        http_.reset(new HttpClient(client, host_, port_));
        httpOn_ = &client;
    }
    return *http_;
}

void HydroNodeClient::closeConnection() {
    if (http_) http_->stop();
}

void HydroNodeClient::begin() {
    // Nothing to start: ensureTimeSynced() fetches the time before the first request.
}

String HydroNodeClient::getApName() const {
    String id(sensorId_);
    String suffix = id.length() >= 4 ? id.substring(id.length() - 4) : id;
    return "HydroNode-Setup-" + suffix;
}

// One name can carry several types ("relay" as BOOL to switch, as UINT32 to pulse). Registering
// the same name and type again replaces that callback only.
void HydroNodeClient::addHandler(const String& key, ValueType type, std::function<void(JsonVariant)> fn) {
    std::vector<Handler>& list = handlers_[key];
    for (Handler& h : list) {
        if (h.type == type) {
            h.fn = fn;
            return;
        }
    }
    list.push_back(Handler{type, fn});
}

void HydroNodeClient::on(const String& key, std::function<void(JsonVariant)> handler) {
    addHandler(key, ValueType::ANY, handler);
}

// Typed callbacks only run after checkCommand() has confirmed that the value
// fits the type, so the as<T>() conversions below never truncate or guess.
void HydroNodeClient::onBool(const String& key, std::function<void(bool)> handler) {
    addHandler(key, ValueType::BOOL, [handler](JsonVariant v) { handler(v.as<bool>()); });
}

void HydroNodeClient::onInt32(const String& key, std::function<void(int32_t)> handler) {
    addHandler(key, ValueType::INT32, [handler](JsonVariant v) { handler(v.as<int32_t>()); });
}

void HydroNodeClient::onUInt32(const String& key, std::function<void(uint32_t)> handler) {
    addHandler(key, ValueType::UINT32, [handler](JsonVariant v) { handler(v.as<uint32_t>()); });
}

void HydroNodeClient::onInt64(const String& key, std::function<void(int64_t)> handler) {
    addHandler(key, ValueType::INT64, [handler](JsonVariant v) { handler(v.as<int64_t>()); });
}

void HydroNodeClient::onUInt64(const String& key, std::function<void(uint64_t)> handler) {
    addHandler(key, ValueType::UINT64, [handler](JsonVariant v) { handler(v.as<uint64_t>()); });
}

void HydroNodeClient::onString(const String& key, std::function<void(const String&)> handler) {
    addHandler(key, ValueType::STRING, [handler](JsonVariant v) { handler(String(v.as<const char*>())); });
}

void HydroNodeClient::setJsonBufferSize(size_t size) {
    jsonBufferSize_ = size;
}

void HydroNodeClient::setHttpTimeout(uint32_t ms) {
    httpTimeoutMs_ = ms;
}

void HydroNodeClient::setDebug(Stream& stream) {
    debug_ = &stream;
}

void HydroNodeClient::setTimeResyncInterval(uint32_t seconds) {
    resyncIntervalS_ = seconds;
}

void HydroNodeClient::setTimeServers(const char* first, const char* second, const char* third) {
    timeServers_[0] = first;
    timeServers_[1] = second;
    timeServers_[2] = third;
}

// --- Fleet and OTA hooks ---------------------------------------------------------------------

const char* HydroNodeClient::family() {
#if defined(ESP8266)
    return "esp8266";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    return "esp32c3";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    return "esp32c6";
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    return "esp32s2";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    return "esp32s3";
#elif defined(ESP32)
    return "esp32";
#elif defined(ARDUINO_ARCH_RP2040)
    return "rp2040";
#elif defined(ARDUINO_ARCH_SAMD)
    return "samd";
#elif defined(ARDUINO_ARCH_NRF52) || defined(NRF52)
    return "nrf52";
#elif defined(ARDUINO_ARCH_STM32)
    return "stm32";
#elif defined(ARDUINO_ARCH_RENESAS)
    return "renesas";
#elif defined(ARDUINO_ARCH_MBED)
    return "mbed";
#else
    return "arduino";
#endif
}

void HydroNodeClient::setFirmwareIdentity(const char* product, const char* version, const char* flags) {
    product_ = headerSafe(product ? product : "hydronode-lib", false);
    version_ = headerSafe(version ? version : LIB_VERSION, false);
    flags_ = headerSafe(flags ? flags : "", true);
    flags_.trim();
}

String HydroNodeClient::firmwareHeader() const {
    String value = product_ + "/" + version_ + " " + family();
    if (flags_.length() > 0) value += " " + flags_;
    if (value.length() > MAX_FIRMWARE_HEADER) value = value.substring(0, MAX_FIRMWARE_HEADER);
    return value;
}

void HydroNodeClient::reportReadError(const char* driverId) {
    if (!driverId || !*driverId || readErrors_.size() >= MAX_READ_ERRORS) return;
    String id;
    for (const char* c = driverId; *c && id.length() < 64; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '.' || *c == '-';
        if (ok) id += *c;
    }
    if (id.length() == 0) return;
    for (const String& known : readErrors_) {
        if (known == id) return;
    }
    readErrors_.push_back(id);
}

void HydroNodeClient::clearReadErrors() {
    readErrors_.clear();
}

void HydroNodeClient::setResetReason(const char* reason) {
    resetOverride_ = reason ? headerSafe(reason, false) : String();
}

const char* HydroNodeClient::chipResetReason() {
#if defined(ESP8266)
    const rst_info* info = ESP.getResetInfoPtr();
    switch (info ? info->reason : static_cast<uint32_t>(REASON_DEFAULT_RST)) {
        case REASON_DEFAULT_RST:      return "poweron";
        case REASON_WDT_RST:          return "watchdog";
        case REASON_EXCEPTION_RST:    return "panic";
        case REASON_SOFT_WDT_RST:     return "watchdog";
        case REASON_SOFT_RESTART:     return "software";
        case REASON_DEEP_SLEEP_AWAKE: return "deepsleep";
        case REASON_EXT_SYS_RST:      return "external";
        default:                      return "unknown";
    }
#elif defined(ESP32)
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_EXT:       return "external";
        default:                return "unknown";
    }
#else
    return "unknown";  // set it yourself with setResetReason() if your board tells you
#endif
}

/**
 * Counts cold starts: power-on, crash, watchdog, restart. Waking from deep sleep runs setup()
 * again but is no new boot, so it only reads the counter. Counted once per run of the sketch.
 * Boards other than the ESP have no place for it that survives a restart: no counter.
 */
uint32_t HydroNodeClient::bootCount() {
    if (bootCounted_) return bootCount_;
    bootCounted_ = true;
#if defined(ESP8266)
    bool cold = strcmp(chipResetReason(), "deepsleep") != 0;
    uint32_t slot[2] = {0, 0};
    ESP.rtcUserMemoryRead(RTC_BOOT_SLOT, slot, sizeof(slot));
    if (slot[0] != RTC_BOOT_MAGIC) {
        slot[0] = RTC_BOOT_MAGIC;
        slot[1] = 0;
    }
    if (cold) slot[1]++;
    ESP.rtcUserMemoryWrite(RTC_BOOT_SLOT, slot, sizeof(slot));
    bootCount_ = slot[1];
#elif defined(ESP32)
    bool cold = strcmp(chipResetReason(), "deepsleep") != 0;
    Preferences prefs;
    if (prefs.begin("hn-lib", false)) {
        bootCount_ = prefs.getUInt("boot", 0);
        if (cold) {
            bootCount_++;
            prefs.putUInt("boot", bootCount_);
        }
        prefs.end();
    }
#endif
    return bootCount_;
}

String HydroNodeClient::deviceStatusHeader() {
    String value;
#if defined(ESP32) || defined(ESP8266)
    value = "boot=" + String(bootCount()) + ";";
#endif
    value += "reset=";
    value += resetOverride_.length() > 0 ? resetOverride_ : String(chipResetReason());
    value += ";uptime=" + String(millis() / 1000UL);
    int dbm = 0;
    if (transport_.signalDbm(dbm)) value += ";rssi=" + String(dbm);
    value += ";net=";
    value += transport_.network();
    if (powerState_.length() > 0) value += ";pwr=" + powerState_;
    value += ";readErr=";
    bool first = true;
    for (const String& id : readErrors_) {
        if (value.length() + id.length() + 1 > MAX_STATUS_HEADER) break;
        if (!first) value += ",";
        value += id;
        first = false;
    }
    return value;
}

void HydroNodeClient::setExtraHeader(const char* name, const String& value) {
    if (!name || !*name) return;
    String key = headerSafe(name, false);
    key.replace(":", "");
    // Semicolons are part of the X-Ota-* values; only control characters (line breaks) go.
    String clean;
    for (size_t i = 0; i < value.length(); i++) {
        char c = value[i];
        if (c >= 0x20 && c != 0x7f) clean += c;
    }
    for (auto& header : extraHeaders_) {
        if (header.first.equalsIgnoreCase(key)) {
            header.second = clean;
            return;
        }
    }
    extraHeaders_.emplace_back(key, clean);
}

void HydroNodeClient::clearExtraHeader(const char* name) {
    if (!name) return;
    for (auto it = extraHeaders_.begin(); it != extraHeaders_.end(); ++it) {
        if (it->first.equalsIgnoreCase(name)) {
            extraHeaders_.erase(it);
            return;
        }
    }
}

void HydroNodeClient::onResponseKey(const char* key, std::function<void(JsonVariantConst)> handler) {
    if (!key || strcmp(key, "commands") == 0) return;
    responseHandlers_[String(key)] = handler;
}

/** Same reported values; the power state travels on its own. */
static bool sameText(const char* a, const char* b) {
    return strcmp(a ? a : "", b ? b : "") == 0;
}

void HydroNodeClient::setDeviceConfig(const HydroNodeDeviceConfig& config) {
    bool same = configSet_ && config_.intervalSeconds == config.intervalSeconds && config_.saveMv == config.saveMv &&
                config_.recoveryMv == config.recoveryMv && config_.standbyMv == config.standbyMv &&
                config_.resumeMv == config.resumeMv && config_.cells == config.cells &&
                config_.capacityMah == config.capacityMah && config_.revision == config.revision &&
                sameText(config_.source, config.source) && sameText(config_.gauge, config.gauge) &&
                sameText(config_.error, config.error);
    configSource_ = config.source ? config.source : "";
    configGauge_ = config.gauge ? config.gauge : "";
    configError_ = config.error ? config.error : "";
    config_ = config;
    config_.source = configSource_.length() > 0 ? configSource_.c_str() : nullptr;
    config_.gauge = configGauge_.length() > 0 ? configGauge_.c_str() : nullptr;
    config_.error = configError_.length() > 0 ? configError_.c_str() : nullptr;
    if (config.powerState) powerState_ = headerSafe(config.powerState, false);
    config_.powerState = powerState_.length() > 0 ? powerState_.c_str() : nullptr;
    configSet_ = true;
    if (!same) configDue_ = true;
}

void HydroNodeClient::setPowerState(const char* state) {
    powerState_ = state ? headerSafe(state, false) : String();
    config_.powerState = powerState_.length() > 0 ? powerState_.c_str() : nullptr;
}

void HydroNodeClient::onSettings(std::function<bool(const HydroNodeSettings&)> handler) {
    settingsHandler_ = handler;
    configDue_ = true;  // caps=settings has to reach HydroNode
}

String HydroNodeClient::deviceConfigHeader() const {
    if (!configSet_ && !settingsHandler_) return String();
    char buffer[hydronode::DEVICE_CONFIG_MAX + 1];
    hydronode::formatDeviceConfig(buffer, sizeof(buffer), config_, static_cast<bool>(settingsHandler_),
                                  rejectedRevision_, rejectError_);
    return String(buffer);
}

void HydroNodeClient::sendDeviceHeaders(HttpClient& http) {
    http.sendHeader("X-Firmware", firmwareHeader().c_str());
    http.sendHeader("X-Device-Status", deviceStatusHeader().c_str());
    configInFlight_ = false;
    if (configDue_) {
        String config = deviceConfigHeader();
        if (config.length() > 0) {
            http.sendHeader("X-Device-Config", config.c_str());
            configInFlight_ = true;
        }
    }
    for (const auto& header : extraHeaders_) {
        http.sendHeader(header.first.c_str(), header.second.c_str());
    }
}

/** HydroNode answered with 2xx: the X-Device-Config of that request arrived. */
void HydroNodeClient::deviceHeadersDelivered() {
    if (!configInFlight_) return;
    configInFlight_ = false;
    configDue_ = false;
    rejectedRevision_ = -1;
    rejectError_ = nullptr;
}

bool HydroNodeClient::sendOtaAck(const char* job, const char* result, const char* reason) {
    if (!job || !result) return false;
    if (!transport_.isUp()) {
        dbg("sendOtaAck: network down");
        return false;
    }
    uint32_t epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        dbg("sendOtaAck: time not synced");
        return false;
    }
    JsonDocument doc;
    doc["job"] = job;
    doc["result"] = result;
    if (reason && *reason) doc["reason"] = reason;
    String payload;
    serializeJson(doc, payload);

    String response;
    int statusCode = postSigned(otaAckPath_, payload, epoch, response);
    dbg("sendOtaAck: " + String(result) + " -> HTTP " + String(statusCode));
    return statusCode >= 200 && statusCode < 300;
}

HydroNodeClient::DownloadResult HydroNodeClient::downloadSigned(const char* pathAndQuery, size_t offset,
                                                                ChunkHandler onChunk) {
    DownloadResult result{ERR_CONNECTION_FAILED, 0, 0};
    if (!pathAndQuery || !onChunk) return result;
    if (!transport_.isUp()) {
        result.status = ERR_NETWORK_DOWN;
        return result;
    }
    uint32_t epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        result.status = ERR_TIME_NOT_SYNCED;
        return result;
    }
    // A GET has no body: the signature covers what is asked for (path and query) and when.
    char signature[hydronode::SIGNATURE_SIZE];
    if (!sign(String(pathAndQuery) + String(epoch), signature)) return result;

    for (int attempt = 0; attempt < 2; attempt++) {
        HttpClient& http = this->http();
        bool reused = transport_.client().connected();
        transport_.beforeConnect(epoch);
        http.connectionKeepAlive();
        http.setHttpResponseTimeout(httpTimeoutMs_);
        http.beginRequest();
        int started = http.get(pathAndQuery);
        if (started != 0) {
            http.stop();
            if (reused) continue;
            result.status = started < 0 ? started : ERR_CONNECTION_FAILED;
            return result;
        }
        http.sendHeader("X-Sensor-Id", sensorId_);
        http.sendHeader("X-Timestamp", String(epoch).c_str());
        http.sendHeader("X-Signature", signature);
        sendDeviceHeaders(http);
        if (offset > 0) http.sendHeader("Range", ("bytes=" + String((unsigned long)offset) + "-").c_str());
        http.endRequest();

        int status = http.responseStatusCode();
        if (status <= 0) {
            http.stop();
            if (reused) continue;
            result.status = status;
            return result;
        }
        result.status = status;
        if (status >= 200 && status < 300) deviceHeadersDelivered();

        long rangeTotal = -1;
        while (http.headerAvailable()) {
            String name = http.readHeaderName();
            String value = http.readHeaderValue();
            if (name.equalsIgnoreCase("Content-Range")) {
                int slash = value.lastIndexOf('/');
                if (slash >= 0 && value.substring(slash + 1) != "*") rangeTotal = value.substring(slash + 1).toInt();
            }
        }
        if (status != 200 && status != 206) {
            http.stop();
            dbg("downloadSigned: HTTP " + String(status));
            return result;
        }
        long length = http.contentLength();
        bool chunked = http.isResponseChunked();
        result.total = rangeTotal >= 0 ? (size_t)rangeTotal : (status == 200 && length >= 0 ? (size_t)length : 0);
        // A server that ignored the range sends the whole file: skip what we already have.
        size_t skip = (status == 200) ? offset : 0;

        uint8_t buffer[1024];
        size_t received = 0;
        uint32_t lastData = millis();
        while (length < 0 || received < (size_t)length) {
            if (chunked && http.endOfBodyReached()) break;
            int available = http.available();
            if (available <= 0) {
                if (!http.connected()) break;
                if (millis() - lastData > httpTimeoutMs_) {
                    http.stop();
                    dbg("downloadSigned: timeout after " + String((unsigned long)received) + " bytes");
                    result.status = ERR_CONNECTION_FAILED;
                    return result;
                }
                delay(1);
                continue;
            }
            size_t want = sizeof(buffer);
            if (length >= 0 && (size_t)length - received < want) want = (size_t)length - received;
            int n = 0;
            if (chunked) {
                // Chunked bodies are decoded byte by byte by the HTTP client.
                while ((size_t)n < want && http.available() > 0) {
                    int c = http.read();
                    if (c < 0) break;
                    buffer[n++] = (uint8_t)c;
                }
            } else {
                n = http.read(buffer, want);
            }
            if (n <= 0) continue;
            lastData = millis();
            received += (size_t)n;
            size_t start = 0;
            if (skip > 0) {
                start = skip < (size_t)n ? skip : (size_t)n;
                skip -= start;
            }
            if (start < (size_t)n) {
                if (!onChunk(buffer + start, (size_t)n - start)) {
                    http.stop();
                    return result;
                }
                result.bytes += (size_t)n - start;
            }
        }
        if (length < 0 && !chunked) http.stop();
        return result;
    }
    return result;
}

// --- Time ------------------------------------------------------------------------------------

uint64_t HydroNodeClient::epochMs() const {
#if defined(HYDRONODE_SYSTEM_CLOCK)
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec < (time_t)MIN_VALID_EPOCH) return 0;
    return uint64_t(tv.tv_sec) * 1000ULL + uint64_t(tv.tv_usec) / 1000ULL;
#else
    if (clockBaseMs == 0) return 0;
    return clockBaseMs + (uint32_t)(millis() - clockBaseMillis);
#endif
}

/** The board clock while it is not set: what a round stored before the first sync is dated with. */
uint32_t HydroNodeClient::unsyncedNow() const {
#if defined(HYDRONODE_SYSTEM_CLOCK)
    return (uint32_t)time(nullptr);
#else
    return millis() / 1000UL;
#endif
}

/** Tells this run of the board from the last one, so unsynced rounds of a past run are not misdated. */
uint16_t HydroNodeClient::runTag() {
    while (runTagValue == 0) runTagValue = (uint16_t)randomWord();
    return runTagValue;
}

/** Sets the clock; rounds stored before the first sync of this run get their real time. */
bool HydroNodeClient::setClockMs(uint64_t unixMs, const char* source) {
    if (unixMs < uint64_t(MIN_VALID_EPOCH) * 1000ULL) return false;
    bool wasValid = epochMs() != 0;
    int64_t before = (int64_t)epochMs();
    int64_t unsyncedBefore = (int64_t)unsyncedNow();
#if defined(HYDRONODE_SYSTEM_CLOCK)
    struct timeval tv;
    tv.tv_sec = (time_t)(unixMs / 1000ULL);
    tv.tv_usec = (suseconds_t)((unixMs % 1000ULL) * 1000ULL);
    settimeofday(&tv, nullptr);
#else
    clockBaseMs = unixMs;
    clockBaseMillis = millis();
#endif
    lastSyncS = (int64_t)(unixMs / 1000ULL);
    syncFailed_ = false;
    String drift = before > 0 ? ", clock was off by " + String((long)((int64_t)unixMs - before)) + " ms" : String();
    dbg("time: synced via " + String(source) + drift);
    if (buffer_ && shiftHook_ && !wasValid) {
        size_t moved = shiftHook_(*buffer_, runTag(), (int64_t)(unixMs / 1000ULL) - unsyncedBefore);
        if (moved) dbg("buffer: dated " + String((unsigned)moved) + " rounds measured before the time sync");
    }
    return true;
}

/** Reads a 64-bit NTP timestamp (seconds since 1900, 32-bit fraction) as microseconds since 1970. */
static uint64_t ntpToUnixUs(const uint8_t* p) {
    uint32_t seconds = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    uint32_t fraction = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | p[7];
    uint64_t s = seconds;
    // The 32-bit seconds roll over in February 2036: small values belong to the next era.
    if (s < NTP_UNIX_OFFSET) s += 0x100000000ULL;
    return (s - NTP_UNIX_OFFSET) * 1000000ULL + ((uint64_t(fraction) * 1000000ULL) >> 32);
}

/**
 * One SNTP exchange with `server`. On a valid answer it sets the clock with the server's
 * transmit time plus half the network round trip, to the millisecond.
 */
bool HydroNodeClient::sntpQuery(UDP& udp, const char* server) {
    uint8_t request[NTP_PACKET_SIZE] = {0};
    request[0] = 0x23;  // leap indicator 0, version 4, mode 3 (client)
    // The transmit timestamp comes back as "originate" and tells the answer to this request from
    // a late one to an earlier request. A random nonce, not the time: the clock may be at 1970.
    uint32_t nonce[2] = {randomWord(), randomWord()};
    memcpy(request + 40, nonce, sizeof(nonce));

    if (!udp.begin(0) && !udp.begin(4123)) return false;  // any free local port
    bool ok = false;
    if (udp.beginPacket(server, NTP_PORT) && udp.write(request, sizeof(request)) == sizeof(request)) {
        uint32_t sentUs = micros();
        if (udp.endPacket()) {
            uint32_t startMs = millis();
            while (!ok && millis() - startMs < NTP_TIMEOUT_MS) {
                int size = udp.parsePacket();
                if (size <= 0) {
                    delay(1);
                    continue;
                }
                uint32_t receivedUs = micros();
                uint8_t reply[NTP_PACKET_SIZE];
                int got = udp.read(reply, sizeof(reply));
                while (udp.available() > 0) udp.read();  // extension fields, MAC
                if (got < (int)sizeof(reply) || memcmp(reply + 24, request + 40, 8) != 0) continue;
                uint8_t leap = reply[0] >> 6, mode = reply[0] & 0x07, stratum = reply[1];
                if (leap == 3 || mode != 4 || stratum == 0 || stratum > 15) {
                    dbg("NTP: " + String(server) + " is not synchronized");
                    break;
                }
                uint64_t serverReceived = ntpToUnixUs(reply + 32);
                uint64_t serverSent = ntpToUnixUs(reply + 40);
                if (serverSent < uint64_t(MIN_VALID_EPOCH) * 1000000ULL || serverSent < serverReceived) break;
                // Network time = round trip minus the time the server held the request.
                int64_t network = int64_t(receivedUs - sentUs) - int64_t(serverSent - serverReceived);
                if (network < 0) network = 0;
                uint64_t nowUs = serverSent + uint64_t(network / 2) + (micros() - receivedUs);
                String source = String(server) + " (round trip " + String((long)((receivedUs - sentUs) / 1000UL)) + " ms)";
                ok = setClockMs(nowUs / 1000ULL, source.c_str());
            }
        }
    }
    udp.stop();
    return ok;
}

bool HydroNodeClient::forceSync() {
    // 1. A source of the sketch (RTC chip, GPS).
    if (timeProvider_ && setClockMs(timeProvider_(), "time provider")) return true;
    if (transport_.isUp()) {
        // 2. SNTP. One unanswered UDP packet must not cost the reading: try the next server
        //    instead of the same one again.
        UDP* udp = transport_.udp();
        if (udp) {
            for (const char* server : timeServers_) {
                if (!server) continue;
                if (sntpQuery(*udp, server)) return true;
                dbg("NTP: no answer from " + String(server));
            }
        }
        // 3. The network's own clock (modem).
        if (setClockMs(transport_.networkTimeMs(), transport_.network())) return true;
    }
    syncFailed_ = true;
    syncFailedAtMs_ = millis();
    return false;
}

bool HydroNodeClient::syncTime() {
    if (!transport_.isUp() && !timeProvider_) return false;
    return forceSync();
}

bool HydroNodeClient::ensureTimeSynced(uint32_t& epochOut) {
    // The backend rejects timestamps outside a small replay window (currently +/- 2 minutes), so
    // an unsynced clock means a guaranteed 401. A recent sync is good enough, also one from before
    // a deep sleep on the ESP32: no time exchange, no extra radio time. The TLS stack checks
    // certificate dates against the same clock, so it must be valid before connecting.
    uint64_t nowMs = epochMs();
    bool valid = nowMs != 0;
    int64_t age = int64_t(nowMs / 1000ULL) - lastSyncS;
    bool due = !valid || lastSyncS == 0 || age < 0 || age >= (int64_t)resyncIntervalS_;
    // A valid clock without an answer is still used; no new try for a while, each costs seconds.
    bool waiting = valid && syncFailed_ && millis() - syncFailedAtMs_ < SYNC_RETRY_MS;
    if (due && !waiting) {
        if (!forceSync() && valid) dbg("time: no source answered, keeping the current clock");
        nowMs = epochMs();
    }
    if (nowMs == 0) return false;
    epochOut = (uint32_t)(nowMs / 1000ULL);
    return true;
}

/**
 * The backend answers a timestamp outside its window with 401 "Invalid timestamp": the clock
 * drifted (ESP32 deep sleep) or stood still (ESP8266 light sleep). Fetches the time again;
 * true when the request should go out once more, re-signed with the fresh time.
 */
bool HydroNodeClient::resyncAfterRejection(int statusCode, const String& response) {
    if (statusCode != 401 || !response.startsWith("Invalid timestamp")) return false;
    dbg("backend rejected the timestamp, syncing the time again");
    return forceSync();
}

/** Unix seconds of a measurement taken `ageMs` before now. */
uint32_t HydroNodeClient::measuredAt(uint32_t ageMs) const {
    uint64_t now = epochMs();
    return (uint32_t)((now - (ageMs < now ? ageMs : now)) / 1000ULL);
}

// --- Sending ---------------------------------------------------------------------------------

/** 5xx, 429, no network, no time, no connection: worth trying again later. */
bool HydroNodeClient::keepForLater(int status) {
    return status == ERR_NETWORK_DOWN || status == ERR_TIME_NOT_SYNCED || status == ERR_CONNECTION_FAILED ||
           status == 429 || status >= 500;
}

bool HydroNodeClient::storeRound(const HydroNodeValue* values, const size_t* index, size_t count, uint32_t ageMs) {
    if (!buffer_ || count == 0) return false;
    HydroNodeValue picked[HydroNodeBuffer::MAX_VALUES_PER_ROUND];
    size_t n = 0;
    for (size_t k = 0; k < count && n < HydroNodeBuffer::MAX_VALUES_PER_ROUND; k++) {
        picked[n++] = values[index ? index[k] : k];
    }
    bool stored;
    if (epochMs() != 0) {
        stored = buffer_->add(measuredAt(ageMs), picked, n, true, runTag());
    } else {
        uint32_t now = unsyncedNow(), age = ageMs / 1000UL;
        stored = buffer_->add(age < now ? now - age : 0, picked, n, false, runTag());
    }
    if (stored) {
        dbg("buffer: stored a round of " + String((unsigned)n) + " values (" + String((unsigned)buffer_->rounds()) +
            " rounds, " + String(buffer_->fillPercent()) + " % full)");
    }
    return stored;
}

/** Flushes the stored rounds after a request that went through. */
void HydroNodeClient::afterSend(int status) {
    if (status < 200 || status >= 300 || !buffer_ || !flushHook_ || !flushAfterSend_ || flushing_) return;
    flushHook_(*this);
}

int HydroNodeClient::sendValue(const char* type, float value) {
    // Checked before any network work: the backend rejects both with 400,
    // so sending them would only cost a TLS handshake.
    if (!hydronode::isValidType(type)) {
        dbg("sendValue: invalid type '" + String(type ? type : "") + "' (use A-Z, 0-9, _ like TEMPERATURE)");
        return ERR_INVALID_TYPE;
    }
    if (!isfinite(value)) {
        dbg("sendValue: value is NaN or infinite, check the sensor reading");
        return ERR_INVALID_VALUE;
    }
    HydroNodeValue one{type, value};
    auto keep = [&](int code) {
        if (storeHook_ && storeOnFailure_ && keepForLater(code) && storeHook_(*this, &one, nullptr, 1, 0)) return ERR_STORED;
        return code;
    };

    if (!transport_.isUp()) {
        dbg("sendValue: network down");
        return keep(ERR_NETWORK_DOWN);
    }

    uint32_t epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        dbg("sendValue: time not synced, aborting (backend would reject the signature)");
        return keep(ERR_TIME_NOT_SYNCED);
    }

    String payload;
    hydronode::appendValuePayload(payload, sensorId_, type, value, epoch);
    String response;
    int statusCode = postSigned(path_, payload, epoch, response);
    if (resyncAfterRejection(statusCode, response) && ensureTimeSynced(epoch)) {
        payload = String();
        hydronode::appendValuePayload(payload, sensorId_, type, value, epoch);
        statusCode = postSigned(path_, payload, epoch, response);
    }
    if (statusCode <= 0) {
        dbg("sendValue: transport error " + String(statusCode));
        return keep(ERR_CONNECTION_FAILED);
    }

    dbg("sendValue: " + String(type) + "=" + String(value, 2) + " -> HTTP " + String(statusCode));

    JsonDocument doc;
    if (statusCode == 202 && parseResponse(response, doc)) {
        handleResponse(doc);
    }
    if (keepForLater(statusCode)) return keep(statusCode);
    afterSend(statusCode);
    return statusCode;
}

/** A backend without the batch endpoint: one request per value, as before 1.7.0. */
int HydroNodeClient::sendEach(const HydroNodeValue* values, const std::vector<size_t>& sent, int* codes) {
    int best = ERR_CONNECTION_FAILED;
    for (size_t i : sent) {
        int code = sendValue(values[i].type, values[i].value);
        if (codes) codes[i] = code;
        if (code >= 200 && code < 300) best = code;
        else if (best < 200 || best >= 300) best = code;
    }
    return best;
}

int HydroNodeClient::sendValues(const HydroNodeValue* values, size_t count, int* codes, uint32_t ageMs) {
    // Checked before any network work, like sendValue(): invalid entries are skipped, the rest go.
    std::vector<size_t> sent;
    sent.reserve(count);
    for (size_t i = 0; i < count; i++) {
        int local = 0;
        if (!hydronode::isValidType(values[i].type)) {
            local = ERR_INVALID_TYPE;
        } else if (!isfinite(values[i].value) || sent.size() >= MAX_VALUES_PER_REQUEST) {
            local = ERR_INVALID_VALUE;
        } else {
            for (size_t k : sent) {
                if (strcmp(values[k].type, values[i].type) == 0) local = ERR_INVALID_TYPE;  // once per type
            }
        }
        if (codes) codes[i] = local;
        if (local) dbg("sendValues: skipping '" + String(values[i].type ? values[i].type : "") + "' (" + String(local) + ")");
        else sent.push_back(i);
    }
    if (sent.empty()) return ERR_INVALID_VALUE;

    auto fail = [&](int code) {
        if (codes) {
            for (size_t i : sent) codes[i] = code;
        }
        return code;
    };
    // A round that cannot go now waits in the buffer (when there is one).
    auto keep = [&](int code) {
        if (storeHook_ && storeOnFailure_ && keepForLater(code) && storeHook_(*this, values, sent.data(), sent.size(), ageMs)) {
            return fail(ERR_STORED);
        }
        return fail(code);
    };
    if (!transport_.isUp()) {
        dbg("sendValues: network down");
        return keep(ERR_NETWORK_DOWN);
    }
    // Too old for the live endpoint: it goes the buffered way, which keeps the measuring time.
    if (storeHook_ && ageMs > LIVE_MAX_AGE_MS) {
        if (!storeHook_(*this, values, sent.data(), sent.size(), ageMs)) return fail(ERR_INVALID_VALUE);
        int status = flushHook_(*this);
        return fail(status == 0 ? ERR_STORED : status);
    }
    if (batchUnsupported_) return sendEach(values, sent, codes);
    uint32_t epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        dbg("sendValues: time not synced, aborting (backend would reject the signature)");
        return keep(ERR_TIME_NOT_SYNCED);
    }

    epoch = measuredAt(ageMs);
    String payload;
    hydronode::appendRoundPayload(payload, sensorId_, epoch, values, sent.data(), sent.size());
    String response;
    int statusCode = postSigned(batchPath_, payload, epoch, response);
    if (resyncAfterRejection(statusCode, response)) {
        epoch = measuredAt(ageMs);
        payload = String();
        hydronode::appendRoundPayload(payload, sensorId_, epoch, values, sent.data(), sent.size());
        statusCode = postSigned(batchPath_, payload, epoch, response);
    }
    if (statusCode == 404 || statusCode == 405) {
        dbg("sendValues: the backend has no batch endpoint, sending one by one");
        batchUnsupported_ = true;
        return sendEach(values, sent, codes);
    }
    if (statusCode <= 0) {
        dbg("sendValues: transport error " + String(statusCode));
        return keep(ERR_CONNECTION_FAILED);
    }
    dbg("sendValues: " + String((unsigned)sent.size()) + " values -> HTTP " + String(statusCode));
    if (keepForLater(statusCode)) return keep(statusCode);

    // Every value shares the request's status, except the ones the answer lists as rejected.
    fail(statusCode);
    JsonDocument doc;
    if (parseResponse(response, doc)) {
        for (JsonObjectConst rejected : doc["rejected"].as<JsonArrayConst>()) {
            const char* type = rejected["type"];
            int status = rejected["status"] | 0;
            if (!type || !codes) continue;
            for (size_t i : sent) {
                if (strcmp(values[i].type, type) == 0) codes[i] = status;
            }
        }
        if (statusCode >= 200 && statusCode < 300) handleResponse(doc);
    }
    afterSend(statusCode);
    return statusCode;
}

// --- Buffer ----------------------------------------------------------------------------------

void HydroNodeClient::attachBuffer(HydroNodeBuffer& buffer) {
    buffer_ = &buffer;
    storeHook_ = [](HydroNodeClient& c, const HydroNodeValue* v, const size_t* i, size_t n, uint32_t age) {
        return c.storeRound(v, i, n, age);
    };
    flushHook_ = [](HydroNodeClient& c) { return c.buffer_->empty() ? 0 : c.flush(); };
    shiftHook_ = [](HydroNodeBuffer& b, uint16_t tag, int64_t delta) { return b.shiftUnsynced(tag, delta); };
    // Rounds a past run stored before its clock was set cannot be dated any more.
    size_t stale = buffer_->dropStaleUnsynced(runTag());
    if (stale) dbg("buffer: dropped " + String((unsigned)stale) + " undated rounds of an earlier run");
}

void HydroNodeClient::setRoundsPerRequest(size_t rounds) {
    if (rounds < 1) rounds = 1;
    roundsPerRequest_ = rounds > MAX_ROUNDS_PER_REQUEST ? MAX_ROUNDS_PER_REQUEST : rounds;
}

int HydroNodeClient::record(const HydroNodeValue* values, size_t count, uint32_t ageMs) {
    if (!buffer_) return ERR_NO_BUFFER;
    if (!values || count == 0) return ERR_INVALID_VALUE;
    if (!storeRound(values, nullptr, count > HydroNodeBuffer::MAX_VALUES_PER_ROUND ? HydroNodeBuffer::MAX_VALUES_PER_ROUND : count, ageMs)) {
        return ERR_INVALID_VALUE;
    }
    HydroNodeBuffer::Round last;
    return buffer_->round(buffer_->rounds() - 1, last) ? last.count : 0;
}

bool HydroNodeClient::flushDue(size_t minRounds, uint32_t maxAgeSeconds) const {
    if (!buffer_ || buffer_->empty()) return false;
    if (minRounds > 0 && buffer_->rounds() >= minRounds) return true;
    if (maxAgeSeconds == 0) return false;
    HydroNodeBuffer::Round oldest;
    uint64_t now = epochMs();
    if (now == 0 || !buffer_->round(0, oldest) || !oldest.synced) return false;
    return now / 1000ULL >= uint64_t(oldest.time) + maxAgeSeconds;
}

int HydroNodeClient::flush(size_t maxRequests) {
    if (!buffer_) return ERR_NO_BUFFER;
    if (buffer_->empty()) return 0;
    if (!transport_.isUp()) {
        dbg("flush: network down, " + String((unsigned)buffer_->rounds()) + " rounds wait");
        return ERR_NETWORK_DOWN;
    }
    uint32_t epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        dbg("flush: time not synced");
        return ERR_TIME_NOT_SYNCED;
    }
    buffer_->dropStaleUnsynced(runTag());
    flushing_ = true;
    int status = 0;
    size_t requests = 0;
    while (!buffer_->empty() && (maxRequests == 0 || requests < maxRequests)) {
        size_t done = 0;
        status = flushOnce(done);
        requests++;
        if (status < 200 || status >= 300 || done == 0) break;
    }
    flushing_ = false;
    return status;
}

/**
 * One request to /api/webhook/sensor-rounds with the oldest stored rounds:
 *
 *   {"sensorId":"<id>","timestamp":<sent>,"rounds":[
 *     {"timestamp":<measured>,"signature":"<round signature>","values":[{"type":"T","value":21.50}]}]}
 *
 * Every round carries its own signature (the one-round payload with its measuring time), the
 * request is signed over the list of these signatures with the time it is sent. `done` gets the
 * number of rounds that left the buffer.
 */
int HydroNodeClient::flushOnce(size_t& done) {
    done = 0;
    size_t total = buffer_->rounds();
    size_t chunk = 0, valueCount = 0;
    HydroNodeBuffer::Round r;
    while (chunk < total && chunk < roundsPerRequest_ && buffer_->round(chunk, r)) {
        if (!r.synced) break;  // measured before the clock was set and not dated yet
        if (chunk > 0 && valueCount + r.count > MAX_VALUES_PER_ROUNDS_REQUEST) break;
        valueCount += r.count;
        chunk++;
    }
    if (chunk == 0) {
        if (buffer_->round(0, r) && !r.synced) {
            dbg("flush: dropping a round that could not be dated");
            buffer_->remove(0);
            done = 1;
            return 202;
        }
        return 0;
    }

    std::vector<String> signatures(chunk);
    String body;
    body.reserve(64 + chunk * 80 + valueCount * 36);
    HydroNodeValue values[HydroNodeBuffer::MAX_VALUES_PER_ROUND];
    for (size_t i = 0; i < chunk; i++) {
        buffer_->round(i, r);
        for (size_t k = 0; k < r.count; k++) HydroNodeBuffer::value(r, k, values[k]);
        String canonical;
        hydronode::appendRoundPayload(canonical, sensorId_, r.time, values, nullptr, r.count);
        char signature[hydronode::SIGNATURE_SIZE];
        canonical += String(r.time);
        if (!sign(canonical, signature)) return ERR_CONNECTION_FAILED;
        signatures[i] = signature;
        if (i) body += ',';
        body += "{\"timestamp\":";
        hydronode::appendNumber(body, r.time);
        body += ",\"signature\":\"";
        body += signature;
        body += "\",\"values\":[";
        for (size_t k = 0; k < r.count; k++) {
            if (k) body += ',';
            body += "{\"type\":\"";
            body += values[k].type;
            body += "\",\"value\":";
            hydronode::appendValue(body, values[k].value);
            body += '}';
        }
        body += "]}";
    }
    std::vector<const char*> list(chunk);
    for (size_t i = 0; i < chunk; i++) list[i] = signatures[i].c_str();

    String response;
    int status = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t sentAt = 0;
        if (!ensureTimeSynced(sentAt)) return ERR_TIME_NOT_SYNCED;
        String request;
        request.reserve(body.length() + 72);
        request += "{\"sensorId\":\"";
        request += sensorId_;
        request += "\",\"timestamp\":";
        hydronode::appendNumber(request, sentAt);
        request += ",\"rounds\":[";
        request += body;
        request += "]}";
        String envelope;
        hydronode::appendRoundsEnvelope(envelope, sensorId_, sentAt, list.data(), chunk);
        envelope += String(sentAt);
        char signature[hydronode::SIGNATURE_SIZE];
        if (!sign(envelope, signature)) return ERR_CONNECTION_FAILED;
        status = postWithSignature(roundsPath_, request, sentAt, signature, response);
        if (attempt == 0 && resyncAfterRejection(status, response)) continue;
        break;
    }
    dbg("flush: " + String((unsigned)chunk) + " rounds, " + String((unsigned)valueCount) + " values -> HTTP " +
        String(status));

    if (status <= 0) return ERR_CONNECTION_FAILED;
    if (status == 404 || status == 405) {
        dbg("flush: this HydroNode server does not take stored rounds yet, they stay in the buffer");
        return status;
    }
    if (status == 400) {
        // The request itself is broken; sending it again cannot help.
        dbg("flush: HydroNode refused the rounds (" + response + "), dropping them");
        buffer_->removeFirst(chunk);
        done = chunk;
        return status;
    }
    if (status < 200 || status >= 300) return status;  // 401, 403, 429, 5xx: keep everything

    JsonDocument doc;
    std::vector<bool> retry(chunk, false);
    bool parsed = parseResponse(response, doc);
    if (parsed) {
        for (JsonVariantConst index : doc["retry"].as<JsonArrayConst>()) {
            int i = index | -1;
            if (i >= 0 && (size_t)i < chunk) retry[i] = true;
        }
    }
    for (size_t i = chunk; i-- > 0;) {
        if (retry[i]) continue;
        buffer_->remove(i);
        done++;
    }
    if (done < chunk) dbg("flush: " + String((unsigned)(chunk - done)) + " rounds stay for a later try");
    if (parsed) handleResponse(doc);
    return status;
}

/** Parses a JSON answer; plain-text answers (errors) and empty ones are no document. */
bool HydroNodeClient::parseResponse(const String& response, JsonDocument& doc) {
    if (response.length() == 0 || response[0] != '{') return false;
    if (response.length() > jsonBufferSize_) {
        dbg("handleResponse: response of " + String(response.length()) + " bytes exceeds the limit, consider setJsonBufferSize()");
        return false;
    }
    DeserializationError err = deserializeJson(doc, response);
    if (err) {
        dbg("handleResponse: JSON parse failed (" + String(err.c_str()) + ")");
        return false;
    }
    return true;
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
void HydroNodeClient::handleResponse(JsonDocument& doc) {
    JsonArray commands = doc["commands"].as<JsonArray>();
    if (!commands.isNull() && commands.size() > 0) {
        handleCommands(commands);
    }

    // Settings before the further keys: an update may restart the board.
    JsonVariantConst settings = doc["settings"];
    if (!settings.isNull()) handleSettings(settings);

    // Further keys (ota, config, ...) after the commands: an update may restart the board.
    for (const auto& entry : responseHandlers_) {
        JsonVariantConst value = doc[entry.first];
        if (!value.isNull()) {
            dbg("response key: " + entry.first);
            entry.second(value);
        }
    }
}

/**
 * Answer key "settings" (only for boards with onSettings()):
 *   {"rev":8,"int":300,"save":3500,"rec":3300,"sby":3200,"res":3600,"mah":2000}
 * The outcome goes out as X-Device-Config with the next request: the new values and rev=8,
 * or the old ones and rej=8.
 */
void HydroNodeClient::handleSettings(JsonVariantConst value) {
    if (!settingsHandler_ || !value.is<JsonObjectConst>()) return;
    JsonVariantConst rev = value["rev"];
    if (!rev.is<uint16_t>()) {
        dbg("settings: no revision, ignored");
        return;
    }
    int32_t revision = rev.as<uint16_t>();
    if (revision == lastSettingsRevision_) {
        // Offered again: the last report did not arrive. Report the outcome once more.
        configDue_ = true;
        return;
    }
    lastSettingsRevision_ = revision;

    HydroNodeSettings settings;
    settings.revision = static_cast<uint16_t>(revision);
    settings.intervalSeconds = value["int"] | config_.intervalSeconds;
    settings.saveMv = value["save"] | config_.saveMv;
    settings.recoveryMv = value["rec"] | config_.recoveryMv;
    settings.standbyMv = value["sby"] | config_.standbyMv;
    settings.resumeMv = value["res"] | config_.resumeMv;
    settings.capacityMah = value["mah"] | config_.capacityMah;

    configDue_ = true;
    if (!hydronode::settingsValid(settings)) {
        dbg("settings: revision " + String(revision) + " breaks the rules, refused");
        rejectedRevision_ = revision;
        rejectError_ = "invalid";
        return;
    }
    if (!settingsHandler_(settings)) {
        dbg("settings: revision " + String(revision) + " refused by the sketch");
        rejectedRevision_ = revision;
        rejectError_ = nullptr;
        return;
    }
    dbg("settings: revision " + String(revision) + " taken");
    config_.revision = settings.revision;
    config_.intervalSeconds = settings.intervalSeconds;
    config_.saveMv = settings.saveMv;
    config_.recoveryMv = settings.recoveryMv;
    config_.standbyMv = settings.standbyMv;
    config_.resumeMv = settings.resumeMv;
    config_.capacityMah = settings.capacityMah;
    configSet_ = true;
    rejectedRevision_ = -1;
    rejectError_ = nullptr;
}

void HydroNodeClient::handleCommands(JsonArray commands) {

    // 1. Decide per command: run it, or decline it with a reason.
    JsonDocument answer;
    JsonArray accepted = answer["accepted"].to<JsonArray>();
    JsonArray declined = answer["declined"].to<JsonArray>();
    std::vector<std::pair<JsonObject, const Handler*>> toRun;

    for (JsonObject cmd : commands) {
        const char* id = cmd["id"];
        const char* key = cmd["command"];
        if (!id || !key) continue;

        const Handler* handler = nullptr;
        const char* reason = pickHandler(String(key), cmd["type"], cmd["value"], handler);

        if (reason) {
            dbg("command '" + String(key) + "' declined: " + reason);
            JsonObject entry = declined.add<JsonObject>();
            entry["id"] = id;
            entry["reason"] = reason;
        } else {
            accepted.add(id);
            toRun.emplace_back(cmd, handler);
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
 * Finds the callback for a command. A typed command goes to the callback of that
 * type, or to an untyped on() callback; an untyped command (older app versions) to
 * the first callback whose type fits the value. Returns nullptr and sets `out`, or
 * the decline reason.
 */
const char* HydroNodeClient::pickHandler(const String& key, const char* wireType, JsonVariant value,
                                         const Handler*& out) const {
    auto it = handlers_.find(key);
    if (it == handlers_.end() || it->second.empty()) {
        return "NO_HANDLER";
    }
    const std::vector<Handler>& list = it->second;
    if (wireType) {
        const Handler* any = nullptr;
        for (const Handler& h : list) {
            if (h.type == ValueType::ANY) any = &h;
            if (h.type != ValueType::ANY && strcmp(typeName(h.type), wireType) == 0) {
                out = &h;
                return checkCommand(h, wireType, value);
            }
        }
        if (any) {
            out = any;
            return nullptr;
        }
        return "TYPE_MISMATCH";
    }
    const char* first = nullptr;
    for (const Handler& h : list) {
        const char* reason = checkCommand(h, wireType, value);
        if (!reason) {
            out = &h;
            return nullptr;
        }
        if (!first) first = reason;
    }
    return first;
}

const char* HydroNodeClient::typeName(ValueType type) {
    static const char* const NAMES[] = {"ANY", "BOOL", "INT32", "UINT32", "INT64", "UINT64", "STRING"};
    return NAMES[static_cast<uint8_t>(type)];
}

/**
 * Returns nullptr if the handler can take the command, otherwise the decline
 * reason. A typed command must name exactly the handler's type. An untyped
 * command (older app versions) is accepted when the value itself fits.
 */
const char* HydroNodeClient::checkCommand(const Handler& handler, const char* wireType, JsonVariant value) {
    if (handler.type == ValueType::ANY) {
        return nullptr;
    }
    const char* expected = typeName(handler.type);
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

bool HydroNodeClient::sendAck(JsonArrayConst accepted, JsonArrayConst declined) {
    JsonDocument doc;
    doc["sensorId"] = sensorId_;
    doc["commandIds"] = accepted;
    if (declined.size() > 0) {
        doc["declined"] = declined;
    }
    String payload;
    serializeJson(doc, payload);

    uint32_t epoch = (uint32_t)(epochMs() / 1000ULL);
    String response;
    int statusCode = postSigned(ackPath_, payload, epoch, response);
    if (statusCode != 200) {
        dbg("sendAck: failed with " + String(statusCode));
        return false;
    }
    dbg("sendAck: " + String(accepted.size()) + " confirmed, " + String(declined.size()) + " declined");
    return true;
}

bool HydroNodeClient::sign(const String& message, char out[45]) {
    if (!hydronode::signBase64(secretKey_, strlen(secretKey_), message.c_str(), message.length(), nullptr, 0, out)) {
        dbg("sign: HMAC failed");
        return false;
    }
    return true;
}

/** POST a payload with HMAC headers (signature = HMAC(payload + epoch)). */
int HydroNodeClient::postSigned(const char* path, const String& payload, uint32_t epoch, String& responseOut) {
    char signature[hydronode::SIGNATURE_SIZE];
    if (!sign(payload + String(epoch), signature)) return ERR_CONNECTION_FAILED;
    return postWithSignature(path, payload, epoch, signature, responseOut);
}

int HydroNodeClient::postWithSignature(const char* path, const String& payload, uint32_t epoch,
                                       const char* signature, String& responseOut) {
    // One TLS connection carries all values of a burst (HTTP keep-alive): the handshake costs
    // seconds on an ESP8266 and on a cellular link, a request on an open connection a fraction of
    // that. A connection the server closed in the meantime fails on first use; then the request
    // goes out once more on a fresh one.
    for (int attempt = 0; attempt < 2; attempt++) {
        HttpClient& http = this->http();
        bool reused = transport_.client().connected();
        uint64_t nowMs = epochMs();
        transport_.beforeConnect(nowMs ? (uint32_t)(nowMs / 1000ULL) : epoch);
        http.connectionKeepAlive();
        http.setHttpResponseTimeout(httpTimeoutMs_);
        http.beginRequest();
        int started = http.post(path);
        if (started != 0) {
            http.stop();
            if (reused) continue;
            return started < 0 ? started : ERR_CONNECTION_FAILED;
        }
        http.sendHeader("Content-Type", "application/json");
        http.sendHeader("X-Sensor-Id", sensorId_);
        http.sendHeader("X-Timestamp", String(epoch).c_str());
        http.sendHeader("X-Signature", signature);
        sendDeviceHeaders(http);
        http.sendHeader("Content-Length", (int)payload.length());
        http.beginBody();
        http.print(payload);
        http.endRequest();

        int statusCode = http.responseStatusCode();
        if (statusCode <= 0) {
            http.stop();
            if (reused) continue;
            return statusCode;
        }
        if (statusCode >= 200 && statusCode < 300) deviceHeadersDelivered();
        responseOut = http.responseBody();
        return statusCode;
    }
    return ERR_CONNECTION_FAILED;
}

void HydroNodeClient::dbg(const String& msg) {
    if (debug_) {
        debug_->println("[HydroNode] " + msg);
    }
}
