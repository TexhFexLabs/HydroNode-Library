#include "HydroNode.h"
#include <math.h>
#include <string.h>
#include <vector>
#include <time.h>
#include <sys/time.h>
#if defined(ESP8266)
#include <base64.h>
#include <bearssl/bearssl_hmac.h>
#include <user_interface.h>
#else
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
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
// mean the NTP sync never happened and the system clock is still at 1970.
static constexpr unsigned long MIN_VALID_EPOCH = 1600000000UL;

// SNTP (RFC 4330): one 48-byte request per server, an answer within a second or the next server.
static constexpr uint16_t NTP_PORT = 123;
static constexpr uint32_t NTP_TIMEOUT_MS = 1000;
static constexpr size_t NTP_PACKET_SIZE = 48;
// NTP counts seconds since 1900; the Unix epoch starts 70 years (2208988800 s) later.
static constexpr uint64_t NTP_UNIX_OFFSET = 2208988800ULL;
// After a sync without answer a valid clock is used as it is; the next try waits this long.
static constexpr uint32_t SYNC_RETRY_MS = 60000;

// Unix seconds of the last successful sync; the system clock itself is the time source. On the
// ESP32 both survive deep sleep (RTC memory and RTC timer), so a wake-up with a recent sync
// needs no NTP exchange. The ESP8266 clock starts at 1970 after every boot: synced per boot.
#if defined(ESP8266)
static int64_t lastSyncS = 0;
#else
static RTC_DATA_ATTR int64_t lastSyncS = 0;
#endif

HydroNode::HydroNode(const char* sensorId, const char* secretKey, const char* host, const char* path)
    : sensorId_(sensorId), secretKey_(secretKey), host_(host), path_(path),
      http_(tls_, host, 443)
{
}

void HydroNode::closeConnection() {
    http_.stop();
}

void HydroNode::begin() {
    // Nothing to start: ensureTimeSynced() fetches the time before the first request.
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

// One name can carry several types ("relay" as BOOL to switch, as UINT32 to pulse). Registering
// the same name and type again replaces that callback only.
void HydroNode::addHandler(const String& key, ValueType type, std::function<void(JsonVariant)> fn) {
    std::vector<Handler>& list = handlers_[key];
    for (Handler& h : list) {
        if (h.type == type) {
            h.fn = fn;
            return;
        }
    }
    list.push_back(Handler{type, fn});
}

void HydroNode::on(const String& key, std::function<void(JsonVariant)> handler) {
    addHandler(key, ValueType::ANY, handler);
}

// Typed callbacks only run after checkCommand() has confirmed that the value
// fits the type, so the as<T>() conversions below never truncate or guess.
void HydroNode::onBool(const String& key, std::function<void(bool)> handler) {
    addHandler(key, ValueType::BOOL, [handler](JsonVariant v) { handler(v.as<bool>()); });
}

void HydroNode::onInt32(const String& key, std::function<void(int32_t)> handler) {
    addHandler(key, ValueType::INT32, [handler](JsonVariant v) { handler(v.as<int32_t>()); });
}

void HydroNode::onUInt32(const String& key, std::function<void(uint32_t)> handler) {
    addHandler(key, ValueType::UINT32, [handler](JsonVariant v) { handler(v.as<uint32_t>()); });
}

void HydroNode::onInt64(const String& key, std::function<void(int64_t)> handler) {
    addHandler(key, ValueType::INT64, [handler](JsonVariant v) { handler(v.as<int64_t>()); });
}

void HydroNode::onUInt64(const String& key, std::function<void(uint64_t)> handler) {
    addHandler(key, ValueType::UINT64, [handler](JsonVariant v) { handler(v.as<uint64_t>()); });
}

void HydroNode::onString(const String& key, std::function<void(const String&)> handler) {
    addHandler(key, ValueType::STRING, [handler](JsonVariant v) { handler(String(v.as<const char*>())); });
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

void HydroNode::setTimeResyncInterval(uint32_t seconds) {
    resyncIntervalS_ = seconds;
}

// --- Fleet and OTA hooks ---------------------------------------------------------------------

const char* HydroNode::family() {
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
#else
    return "esp32";
#endif
}

void HydroNode::setFirmwareIdentity(const char* product, const char* version, const char* flags) {
    product_ = headerSafe(product ? product : "hydronode-lib", false);
    version_ = headerSafe(version ? version : LIB_VERSION, false);
    flags_ = headerSafe(flags ? flags : "", true);
    flags_.trim();
}

String HydroNode::firmwareHeader() const {
    String value = product_ + "/" + version_ + " " + family();
    if (flags_.length() > 0) value += " " + flags_;
    if (value.length() > MAX_FIRMWARE_HEADER) value = value.substring(0, MAX_FIRMWARE_HEADER);
    return value;
}

void HydroNode::reportReadError(const char* driverId) {
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

void HydroNode::clearReadErrors() {
    readErrors_.clear();
}

void HydroNode::setResetReason(const char* reason) {
    resetOverride_ = reason ? headerSafe(reason, false) : String();
}

const char* HydroNode::chipResetReason() {
#if defined(ESP8266)
    const rst_info* info = ESP.getResetInfoPtr();
    switch (info ? info->reason : REASON_DEFAULT_RST) {
        case REASON_DEFAULT_RST:      return "poweron";
        case REASON_WDT_RST:          return "watchdog";
        case REASON_EXCEPTION_RST:    return "panic";
        case REASON_SOFT_WDT_RST:     return "watchdog";
        case REASON_SOFT_RESTART:     return "software";
        case REASON_DEEP_SLEEP_AWAKE: return "deepsleep";
        case REASON_EXT_SYS_RST:      return "external";
        default:                      return "unknown";
    }
#else
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
#endif
}

/**
 * Counts cold starts: power-on, crash, watchdog, restart. Waking from deep sleep runs setup()
 * again but is no new boot, so it only reads the counter. Counted once per run of the sketch.
 */
uint32_t HydroNode::bootCount() {
    if (bootCounted_) return bootCount_;
    bootCounted_ = true;
    bool cold = strcmp(chipResetReason(), "deepsleep") != 0;
#if defined(ESP8266)
    uint32_t slot[2] = {0, 0};
    ESP.rtcUserMemoryRead(RTC_BOOT_SLOT, slot, sizeof(slot));
    if (slot[0] != RTC_BOOT_MAGIC) {
        slot[0] = RTC_BOOT_MAGIC;
        slot[1] = 0;
    }
    if (cold) slot[1]++;
    ESP.rtcUserMemoryWrite(RTC_BOOT_SLOT, slot, sizeof(slot));
    bootCount_ = slot[1];
#else
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

String HydroNode::deviceStatusHeader() {
    String value = "boot=" + String(bootCount());
    value += ";reset=";
    value += resetOverride_.length() > 0 ? resetOverride_ : String(chipResetReason());
    value += ";uptime=" + String(millis() / 1000UL);
    if (WiFi.status() == WL_CONNECTED) value += ";rssi=" + String(WiFi.RSSI());
    value += ";net=wifi;readErr=";
    bool first = true;
    for (const String& id : readErrors_) {
        if (value.length() + id.length() + 1 > MAX_STATUS_HEADER) break;
        if (!first) value += ",";
        value += id;
        first = false;
    }
    return value;
}

void HydroNode::setExtraHeader(const char* name, const String& value) {
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

void HydroNode::clearExtraHeader(const char* name) {
    if (!name) return;
    for (auto it = extraHeaders_.begin(); it != extraHeaders_.end(); ++it) {
        if (it->first.equalsIgnoreCase(name)) {
            extraHeaders_.erase(it);
            return;
        }
    }
}

void HydroNode::onResponseKey(const char* key, std::function<void(JsonVariantConst)> handler) {
    if (!key || strcmp(key, "commands") == 0) return;
    responseHandlers_[String(key)] = handler;
}

void HydroNode::sendDeviceHeaders() {
    http_.sendHeader("X-Firmware", firmwareHeader().c_str());
    http_.sendHeader("X-Device-Status", deviceStatusHeader().c_str());
    for (const auto& header : extraHeaders_) {
        http_.sendHeader(header.first.c_str(), header.second.c_str());
    }
}

bool HydroNode::sendOtaAck(const char* job, const char* result, const char* reason) {
    if (!job || !result) return false;
    if (WiFi.status() != WL_CONNECTED) {
        dbg("sendOtaAck: WiFi not connected");
        return false;
    }
    unsigned long epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        dbg("sendOtaAck: NTP time not synced");
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

HydroNode::DownloadResult HydroNode::downloadSigned(const char* pathAndQuery, size_t offset, ChunkHandler onChunk) {
    DownloadResult result{ERR_CONNECTION_FAILED, 0, 0};
    if (!pathAndQuery || !onChunk) return result;
    if (WiFi.status() != WL_CONNECTED) {
        result.status = ERR_WIFI_DISCONNECTED;
        return result;
    }
    unsigned long epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        result.status = ERR_TIME_NOT_SYNCED;
        return result;
    }
    // A GET has no body: the signature covers what is asked for (path and query) and when.
    String signature = sign(String(pathAndQuery) + String(epoch));
    if (signature.length() == 0) return result;

    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = tls_.connected();
        prepareTls();
        http_.connectionKeepAlive();
        http_.setHttpResponseTimeout(httpTimeoutMs_);
        http_.beginRequest();
        int started = http_.get(pathAndQuery);
        if (started != 0) {
            http_.stop();
            if (reused) continue;
            result.status = started < 0 ? started : ERR_CONNECTION_FAILED;
            return result;
        }
        http_.sendHeader("X-Sensor-Id", sensorId_);
        http_.sendHeader("X-Timestamp", String(epoch).c_str());
        http_.sendHeader("X-Signature", signature.c_str());
        sendDeviceHeaders();
        if (offset > 0) http_.sendHeader("Range", ("bytes=" + String((unsigned long)offset) + "-").c_str());
        http_.endRequest();

        int status = http_.responseStatusCode();
        if (status <= 0) {
            http_.stop();
            if (reused) continue;
            result.status = status;
            return result;
        }
        result.status = status;

        long rangeTotal = -1;
        while (http_.headerAvailable()) {
            String name = http_.readHeaderName();
            String value = http_.readHeaderValue();
            if (name.equalsIgnoreCase("Content-Range")) {
                int slash = value.lastIndexOf('/');
                if (slash >= 0 && value.substring(slash + 1) != "*") rangeTotal = value.substring(slash + 1).toInt();
            }
        }
        if (status != 200 && status != 206) {
            http_.stop();
            dbg("downloadSigned: HTTP " + String(status));
            return result;
        }
        long length = http_.contentLength();
        bool chunked = http_.isResponseChunked();
        result.total = rangeTotal >= 0 ? (size_t)rangeTotal : (status == 200 && length >= 0 ? (size_t)length : 0);
        // A server that ignored the range sends the whole file: skip what we already have.
        size_t skip = (status == 200) ? offset : 0;

        uint8_t buffer[1024];
        size_t received = 0;
        uint32_t lastData = millis();
        while (length < 0 || received < (size_t)length) {
            if (chunked && http_.endOfBodyReached()) break;
            int available = http_.available();
            if (available <= 0) {
                if (!http_.connected()) break;
                if (millis() - lastData > httpTimeoutMs_) {
                    http_.stop();
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
                while ((size_t)n < want && http_.available() > 0) {
                    int c = http_.read();
                    if (c < 0) break;
                    buffer[n++] = (uint8_t)c;
                }
            } else {
                n = http_.read(buffer, want);
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
                    http_.stop();
                    return result;
                }
                result.bytes += (size_t)n - start;
            }
        }
        if (length < 0 && !chunked) http_.stop();
        return result;
    }
    return result;
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
 * One SNTP exchange with `server`. On a valid answer it sets the system clock with the
 * server's transmit time plus half the network round trip, to the millisecond.
 */
bool HydroNode::sntpQuery(const char* server) {
    uint8_t request[NTP_PACKET_SIZE] = {0};
    request[0] = 0x23;  // leap indicator 0, version 4, mode 3 (client)
    // The transmit timestamp comes back as "originate" and tells the answer to this request from
    // a late one to an earlier request. A random nonce, not the time: the clock may be at 1970.
#if defined(ESP8266)
    uint32_t nonce[2] = {RANDOM_REG32, RANDOM_REG32};
#else
    uint32_t nonce[2] = {esp_random(), esp_random()};
#endif
    memcpy(request + 40, nonce, sizeof(nonce));

    if (!ntpUDP_.begin(0)) return false;  // any free local port
    bool ok = false;
    if (ntpUDP_.beginPacket(server, NTP_PORT) && ntpUDP_.write(request, sizeof(request)) == sizeof(request)) {
        uint32_t sentUs = micros();
        if (ntpUDP_.endPacket()) {
            uint32_t startMs = millis();
            while (!ok && millis() - startMs < NTP_TIMEOUT_MS) {
                int size = ntpUDP_.parsePacket();
                if (size <= 0) {
                    delay(1);
                    continue;
                }
                uint32_t receivedUs = micros();
                uint8_t reply[NTP_PACKET_SIZE];
                int got = ntpUDP_.read(reply, sizeof(reply));
                while (ntpUDP_.available() > 0) ntpUDP_.read();  // extension fields, MAC
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

                int64_t before = (int64_t)epochMs();
                struct timeval tv;
                tv.tv_sec = (time_t)(nowUs / 1000000ULL);
                tv.tv_usec = (suseconds_t)(nowUs % 1000000ULL);
                settimeofday(&tv, nullptr);
                lastSyncS = (int64_t)tv.tv_sec;
                String drift = before > 0 ? ", clock was off by " + String((long)(int64_t(nowUs / 1000ULL) - before)) + " ms" : String();
                dbg("NTP: synced via " + String(server) + " (round trip " + String((long)((receivedUs - sentUs) / 1000UL)) + " ms" + drift + ")");
                ok = true;
            }
        }
    }
    ntpUDP_.stop();
    return ok;
}

bool HydroNode::forceSync() {
    // One unanswered UDP packet must not cost the reading: try the next server instead of the
    // same one again.
    static const char* const servers[] = {"pool.ntp.org", "time.google.com", "time.cloudflare.com"};
    for (const char* server : servers) {
        if (sntpQuery(server)) {
            syncFailed_ = false;
            return true;
        }
        dbg("NTP: no answer from " + String(server));
    }
    syncFailed_ = true;
    syncFailedAtMs_ = millis();
    return false;
}

bool HydroNode::syncTime() {
    if (WiFi.status() != WL_CONNECTED) return false;
    return forceSync();
}

uint64_t HydroNode::epochMs() const {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec < (time_t)MIN_VALID_EPOCH) return 0;
    return uint64_t(tv.tv_sec) * 1000ULL + uint64_t(tv.tv_usec) / 1000ULL;
}

bool HydroNode::ensureTimeSynced(unsigned long& epochOut) {
    // The backend rejects timestamps outside a small replay window (currently +/- 2 minutes), so
    // an unsynced clock means a guaranteed 401. A recent sync is good enough, also one from before
    // a deep sleep on the ESP32: no NTP exchange, no extra radio time. The TLS stack checks
    // certificate dates against the same system clock, so it must be valid before connecting.
    time_t now = time(nullptr);
    bool valid = now >= (time_t)MIN_VALID_EPOCH;
    int64_t age = int64_t(now) - lastSyncS;
    bool due = !valid || lastSyncS == 0 || age < 0 || age >= (int64_t)resyncIntervalS_;
    // A valid clock without NTP answer is still used; no new try for a while, each costs seconds.
    bool waiting = valid && syncFailed_ && millis() - syncFailedAtMs_ < SYNC_RETRY_MS;
    if (due && !waiting) {
        if (!forceSync() && valid) dbg("NTP: keeping the current clock");
        now = time(nullptr);
    }
    if (now < (time_t)MIN_VALID_EPOCH) {
        return false;
    }
    epochOut = (unsigned long)now;
    return true;
}

/**
 * The backend answers a timestamp outside its window with 401 "Invalid timestamp": the clock
 * drifted (ESP32 deep sleep) or stood still (ESP8266 light sleep). Fetches the time again;
 * true when the request should go out once more, re-signed with the fresh time.
 */
bool HydroNode::resyncAfterRejection(int statusCode, const String& response) {
    if (statusCode != 401 || !response.startsWith("Invalid timestamp")) return false;
    dbg("backend rejected the timestamp, syncing the time again");
    return forceSync();
}

/** Unix seconds of a measurement taken `ageMs` before now. */
unsigned long HydroNode::measuredAt(uint32_t ageMs) const {
    uint64_t now = epochMs();
    return (unsigned long)((now - (ageMs < now ? ageMs : now)) / 1000ULL);
}

/** Part of the HMAC contract with the backend: key order, 2 decimal places, no whitespace. */
String HydroNode::valuePayload(const char* type, float value, unsigned long epoch) const {
    return "{\"sensorId\":\"" + String(sensorId_) + "\",\"type\":\"" + type + "\",\"value\":" + String(value, 2) + ",\"timestamp\":" + String(epoch) + "}";
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

    String response;
    int statusCode = postSigned(path_, valuePayload(type, value, epoch), epoch, response);
    if (resyncAfterRejection(statusCode, response) && ensureTimeSynced(epoch)) {
        statusCode = postSigned(path_, valuePayload(type, value, epoch), epoch, response);
    }
    if (statusCode <= 0) {
        dbg("sendValue: transport error " + String(statusCode));
        return ERR_CONNECTION_FAILED;
    }

    dbg("sendValue: " + String(type) + "=" + String(value, 2) + " -> HTTP " + String(statusCode));

    JsonDocument doc;
    if (statusCode == 202 && parseResponse(response, doc)) {
        handleResponse(doc);
    }
    return statusCode;
}

/** Part of the HMAC contract with the backend like valuePayload(): the backend rebuilds this text. */
String HydroNode::batchPayload(const HydroNodeValue* values, const std::vector<size_t>& sent, unsigned long epoch) const {
    String out;
    out.reserve(72 + sent.size() * 48);
    out += "{\"sensorId\":\"";
    out += sensorId_;
    out += "\",\"timestamp\":";
    out += String(epoch);
    out += ",\"values\":[";
    for (size_t k = 0; k < sent.size(); k++) {
        if (k) out += ',';
        out += "{\"type\":\"";
        out += values[sent[k]].type;
        out += "\",\"value\":";
        out += String(values[sent[k]].value, 2);
        out += '}';
    }
    out += "]}";
    return out;
}

/** A backend without the batch endpoint: one request per value, as before 1.7.0. */
int HydroNode::sendEach(const HydroNodeValue* values, const std::vector<size_t>& sent, int* codes) {
    int best = ERR_CONNECTION_FAILED;
    for (size_t i : sent) {
        int code = sendValue(values[i].type, values[i].value);
        if (codes) codes[i] = code;
        if (code >= 200 && code < 300) best = code;
        else if (best < 200 || best >= 300) best = code;
    }
    return best;
}

int HydroNode::sendValues(const HydroNodeValue* values, size_t count, int* codes, uint32_t ageMs) {
    // Checked before any network work, like sendValue(): invalid entries are skipped, the rest go.
    std::vector<size_t> sent;
    sent.reserve(count);
    for (size_t i = 0; i < count; i++) {
        int local = 0;
        if (!isValidType(values[i].type)) {
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
    if (WiFi.status() != WL_CONNECTED) {
        dbg("sendValues: WiFi not connected");
        return fail(ERR_WIFI_DISCONNECTED);
    }
    if (batchUnsupported_) return sendEach(values, sent, codes);
    unsigned long epoch = 0;
    if (!ensureTimeSynced(epoch)) {
        dbg("sendValues: NTP time not synced, aborting (backend would reject the signature)");
        return fail(ERR_TIME_NOT_SYNCED);
    }

    epoch = measuredAt(ageMs);
    String response;
    int statusCode = postSigned(batchPath_, batchPayload(values, sent, epoch), epoch, response);
    if (resyncAfterRejection(statusCode, response)) {
        epoch = measuredAt(ageMs);
        statusCode = postSigned(batchPath_, batchPayload(values, sent, epoch), epoch, response);
    }
    if (statusCode == 404 || statusCode == 405) {
        dbg("sendValues: the backend has no batch endpoint, sending one by one");
        batchUnsupported_ = true;
        return sendEach(values, sent, codes);
    }
    if (statusCode <= 0) {
        dbg("sendValues: transport error " + String(statusCode));
        return fail(ERR_CONNECTION_FAILED);
    }
    dbg("sendValues: " + String((unsigned)sent.size()) + " values -> HTTP " + String(statusCode));

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
    return statusCode;
}

/** Parses a JSON answer; plain-text answers (errors) and empty ones are no document. */
bool HydroNode::parseResponse(const String& response, JsonDocument& doc) {
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
void HydroNode::handleResponse(JsonDocument& doc) {
    JsonArray commands = doc["commands"].as<JsonArray>();
    if (!commands.isNull() && commands.size() > 0) {
        handleCommands(commands);
    }

    // Further keys (ota, config, ...) after the commands: an update may restart the board.
    for (const auto& entry : responseHandlers_) {
        JsonVariantConst value = doc[entry.first];
        if (!value.isNull()) {
            dbg("response key: " + entry.first);
            entry.second(value);
        }
    }
}

void HydroNode::handleCommands(JsonArray commands) {

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
const char* HydroNode::pickHandler(const String& key, const char* wireType, JsonVariant value, const Handler*& out) const {
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

const char* HydroNode::typeName(ValueType type) {
    static const char* const NAMES[] = {"ANY", "BOOL", "INT32", "UINT32", "INT64", "UINT64", "STRING"};
    return NAMES[static_cast<uint8_t>(type)];
}

/**
 * Returns nullptr if the handler can take the command, otherwise the decline
 * reason. A typed command must name exactly the handler's type. An untyped
 * command (older app versions) is accepted when the value itself fits.
 */
const char* HydroNode::checkCommand(const Handler& handler, const char* wireType, JsonVariant value) {
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

bool HydroNode::sendAck(JsonArrayConst accepted, JsonArrayConst declined) {
    JsonDocument doc;
    doc["sensorId"] = sensorId_;
    doc["commandIds"] = accepted;
    if (declined.size() > 0) {
        doc["declined"] = declined;
    }
    String payload;
    serializeJson(doc, payload);

    unsigned long epoch = (unsigned long)time(nullptr);
    String response;
    int statusCode = postSigned(ackPath_, payload, epoch, response);
    if (statusCode != 200) {
        dbg("sendAck: failed with " + String(statusCode));
        return false;
    }
    dbg("sendAck: " + String(accepted.size()) + " confirmed, " + String(declined.size()) + " declined");
    return true;
}

/** POST a payload with HMAC headers (signature = HMAC(payload + epoch)). */
int HydroNode::postSigned(const char* path, const String& payload, unsigned long epoch, String& responseOut) {
    String signature = sign(payload + String(epoch));
    if (signature.length() == 0) {
        return ERR_CONNECTION_FAILED;
    }

    // One TLS connection carries all values of a burst (HTTP keep-alive): the handshake costs
    // seconds on an ESP8266, a request on an open connection a fraction of that. A connection the
    // server closed in the meantime fails on first use; then the request goes out once more on a
    // fresh one.
    for (int attempt = 0; attempt < 2; attempt++) {
        bool reused = tls_.connected();
        prepareTls();
        http_.connectionKeepAlive();
        http_.setHttpResponseTimeout(httpTimeoutMs_);
        http_.beginRequest();
        int started = http_.post(path);
        if (started != 0) {
            http_.stop();
            if (reused) continue;
            return started < 0 ? started : ERR_CONNECTION_FAILED;
        }
        http_.sendHeader("Content-Type", "application/json");
        http_.sendHeader("X-Sensor-Id", sensorId_);
        http_.sendHeader("X-Timestamp", String(epoch));
        http_.sendHeader("X-Signature", signature);
        sendDeviceHeaders();
        http_.sendHeader("Content-Length", payload.length());
        http_.beginBody();
        http_.print(payload);
        http_.endRequest();

        int statusCode = http_.responseStatusCode();
        if (statusCode <= 0) {
            http_.stop();
            if (reused) continue;
            return statusCode;
        }
        responseOut = http_.responseBody();
        return statusCode;
    }
    return ERR_CONNECTION_FAILED;
}

void HydroNode::prepareTls() {
#if defined(ESP8266)
    // BearSSL: parse the roots once, they stay valid for the lifetime of the sketch. ECDSA roots
    // only: the RSA-4096 ones would take ~6 KB of a heap the handshake needs in full. The session
    // lets the next connection resume instead of running the full handshake again (when the
    // server still knows it).
    static BearSSL::X509List trust(HYDRONODE_CA_BUNDLE_EC);
    static BearSSL::Session session;
    if (!tlsReady_) {
        tls_.setTrustAnchors(&trust);
        tls_.setSession(&session);
        tlsReady_ = true;
    }
    tls_.setX509Time(time(nullptr));  // the clock ensureTimeSynced() made valid
#else
    if (!tlsReady_) {
        tls_.setCACert(HYDRONODE_CA_BUNDLE);
        tlsReady_ = true;
    }
#endif
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

/** Base64(HMAC-SHA256(secretKey, message)), computed with the TLS library built into the core. */
String HydroNode::sign(const String& message) {
    uint8_t mac[32];
#if defined(ESP8266)
    br_hmac_key_context key;
    br_hmac_context ctx;
    br_hmac_key_init(&key, &br_sha256_vtable, secretKey_, strlen(secretKey_));
    br_hmac_init(&ctx, &key, 0);
    br_hmac_update(&ctx, message.c_str(), message.length());
    br_hmac_out(&ctx, mac);
    String encoded = base64::encode(mac, sizeof(mac), false);
    memset(mac, 0, sizeof(mac));
    memset(&key, 0, sizeof(key));
    return encoded;
#else
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
#endif
}

void HydroNode::dbg(const String& msg) {
    if (debug_) {
        debug_->println("[HydroNode] " + msg);
    }
}
