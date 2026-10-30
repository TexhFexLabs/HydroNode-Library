/*
 * HydroNode — Buffer_DeepSleep
 *
 * A WiFi battery sensor that measures every 5 minutes but switches WiFi on only every 6th time
 * (every 30 minutes). In between it measures in about 100 ms and sleeps again. WiFi is what
 * empties the battery: 6 times fewer connections, several times longer battery life. Every
 * reading keeps the time it was measured.
 *
 *   wake 1..5   measure, record() into RTC memory, deep sleep         (~0.1 s, no WiFi)
 *   wake 6      measure, record(), WiFi on, flush() all 6, deep sleep  (~3 s)
 *
 * Where the rounds survive deep sleep:
 *   ESP32    RTC_DATA_ATTR memory (8 KB): just put the buffer there, nothing else to do.
 *   ESP8266  512 bytes of RTC user memory: the sketch copies the buffer in and out
 *            (ESP.rtcUserMemoryRead/Write). Holds about 12 rounds of two values.
 *            The ESP8266 clock also restarts after deep sleep, so the sketch carries the time
 *            across (planned wake-up time in RTC memory). Good enough to date a reading; before
 *            sending, the library fetches the exact time anyway.
 *            The ESP8266 needs GPIO16 wired to RST to wake up from deep sleep.
 *
 * The very first start after power-on has no time yet: it goes online right away, which sets
 * the clock and dates the first reading.
 *
 * Board: ESP32 or ESP8266. Libraries: HydroNode-Library
 *
 * Change: credentials, MEASURE_EVERY_S, SEND_EVERY.
 */
#include <HydroNode.h>

// --- 1. Your credentials -------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";
const char* SECRET_KEY    = "your-secret-key-here";

const uint32_t MEASURE_EVERY_S = 300;  // measure every 5 minutes
const size_t SEND_EVERY = 6;           // WiFi every 6th measurement = every 30 minutes

// --- 2. The buffer in memory that survives deep sleep -----------------------
#if defined(ESP32)
RTC_DATA_ATTR uint8_t bufferMemory[2048];
#else
// ESP8266: RTC user memory is 512 bytes in 4-byte blocks. The library keeps its boot counter in
// the last 8 bytes, so the buffer gets the first 480.
static uint8_t bufferMemory[480];
#endif
HydroNodeBuffer buffer(bufferMemory, sizeof(bufferMemory));

#if defined(ESP8266)
#include <sys/time.h>
// RTC user memory blocks 120-121 (after the buffer, before the library's boot counter at 126).
struct ClockKeeper {
    uint32_t magic;
    uint32_t wakeAtS;  // Unix seconds this wake-up was planned for
};
static constexpr uint32_t CLOCK_BLOCK = 120;
static constexpr uint32_t CLOCK_MAGIC = 0x484E434B;  // "HNCK"

/** Sets the clock to the planned wake-up time when it is not set (after deep sleep). */
void restoreClock() {
    ClockKeeper keeper{};
    ESP.rtcUserMemoryRead(CLOCK_BLOCK, reinterpret_cast<uint32_t*>(&keeper), sizeof(keeper));
    if (keeper.magic != CLOCK_MAGIC || time(nullptr) > 1600000000) return;
    struct timeval tv = {(time_t)keeper.wakeAtS, 0};
    settimeofday(&tv, nullptr);
}

void keepClock() {
    uint64_t now = 0;
    // Only a set clock is worth keeping.
    if (time(nullptr) > 1600000000) now = (uint64_t)time(nullptr);
    ClockKeeper keeper{now ? CLOCK_MAGIC : 0, (uint32_t)(now + MEASURE_EVERY_S)};
    ESP.rtcUserMemoryWrite(CLOCK_BLOCK, reinterpret_cast<uint32_t*>(&keeper), sizeof(keeper));
}
#endif

HydroNode hydro(SENSOR_ID, SECRET_KEY);

float readTemperature() { return 18.7; }  // TODO: real measurement
float readBatteryVolts() { return 3.91; }  // TODO: e.g. analogRead on a divider

void restoreBuffer() {
#if defined(ESP8266)
    ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t*>(bufferMemory), sizeof(bufferMemory));
#endif
    buffer.begin();  // keeps the rounds when the content checks out, else starts empty
}

void keepBuffer() {
#if defined(ESP8266)
    ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(bufferMemory), sizeof(bufferMemory));
#endif
}

void sleepUntilNextReading() {
    Serial.flush();
#if defined(ESP32)
    esp_sleep_enable_timer_wakeup(uint64_t(MEASURE_EVERY_S) * 1000000ULL);
    esp_deep_sleep_start();
#else
    ESP.deepSleep(uint64_t(MEASURE_EVERY_S) * 1000000ULL);
#endif
}

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);
#if defined(ESP8266)
    restoreClock();
#endif
    restoreBuffer();
    hydro.attachBuffer(buffer);

    HydroNodeValue values[] = {
        {"TEMPERATURE", readTemperature()},
        {"BATTERY_VOLTAGE", readBatteryVolts()},
    };
    hydro.record(values, 2);

    // Every 6th reading, or now when the clock is not set yet (first start): going online sets it.
    bool due = hydro.flushDue(SEND_EVERY) || hydro.epochMs() == 0;
    if (due && hydro.connectWiFi(WIFI_SSID, WIFI_PASSWORD, 15000)) {
        int status = hydro.flush();
        Serial.printf("Sent: %d, %u rounds left\n", status, (unsigned)buffer.rounds());
        hydro.closeConnection();
    }
    keepBuffer();
#if defined(ESP8266)
    keepClock();
#endif
    sleepUntilNextReading();
}

void loop() {}
