/*
 * HydroNode PowerThresholds: a battery board that saves itself, like the HydroNode station.
 *
 * Wakes from deep sleep, reads the battery through a voltage divider, and lets
 * HydroNodeBatteryGuard decide:
 *
 *   NORMAL    send, sleep the interval
 *   SAVE      send, sleep twice the interval
 *   RECOVERY  no WiFi, outputs off, check the battery every 60 s
 *   STANDBY   no WiFi, check once an hour
 *
 * Before it turns WiFi off it sends one last round with pwr=recovery, so HydroNode shows
 * "Low battery standby" instead of "Offline". The board reports its interval and thresholds
 * (X-Device-Config) and takes changed ones from the sensor settings in HydroNode: onSettings()
 * stores them, they survive a power cut.
 *
 * Wiring: battery + -> 100k -> ADC pin -> 100k -> GND (halves the voltage). On the ESP8266
 * the ADC is A0; a NodeMCU already divides by 3.2, so add 100k in series (about 4.2 V full scale)
 * and set DIVIDER below. Deep sleep on the ESP8266 needs GPIO16 (D0) wired to RST.
 *
 * Board: ESP32 (any variant) or ESP8266
 */
#include <HydroNode.h>
#if defined(ESP32)
#include <Preferences.h>
#include <esp_sleep.h>
#else
#include <EEPROM.h>
#endif

// --- 1. Your credentials -------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";    // from the HydroNode app
const char* SECRET_KEY    = "your-secret-key-here";   // from the HydroNode app

// --- 2. Your battery -----------------------------------------------------
#if defined(ESP32)
const int BATTERY_PIN = 1;        // an ADC1 pin (ADC2 does not work while WiFi runs)
const float DIVIDER = 2.0f;       // 100k / 100k
#else
const int BATTERY_PIN = A0;
const float DIVIDER = 4.2f;       // NodeMCU divider plus 100k in series: 1023 = 4.2 V
#endif
const uint8_t CELLS = 1;          // cells in series
const uint32_t DEFAULT_INTERVAL_S = 300;

HydroNode hydro(SENSOR_ID, SECRET_KEY);
HydroNodeBatteryGuard guard;

// What HydroNode may change, kept in flash (Preferences on the ESP32, EEPROM on the ESP8266).
struct Stored {
    uint32_t magic;
    uint16_t revision;
    uint32_t intervalSeconds;
    HydroNodeThresholds thresholds;
};
static constexpr uint32_t STORED_MAGIC = 0x504F5752;  // "POWR"
Stored stored;

// What survives deep sleep: the guard and a clock that counts the sleep as well.
struct Rtc {
    uint32_t magic;
    uint32_t clockSeconds;
    HydroNodeBatteryGuard::Memory guard;
};
static constexpr uint32_t RTC_MAGIC = 0x52544331;  // "RTC1"
#if defined(ESP32)
RTC_DATA_ATTR Rtc rtc;
#else
Rtc rtc;  // slot 0 of the RTC user memory; the library uses the last 8 bytes
#endif

void loadStored() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("power", true);
    size_t n = prefs.getBytes("stored", &stored, sizeof(stored));
    prefs.end();
    if (n != sizeof(stored)) stored.magic = 0;
#else
    EEPROM.begin(sizeof(Stored));
    EEPROM.get(0, stored);
    EEPROM.end();
#endif
    if (stored.magic != STORED_MAGIC) {
        stored = {STORED_MAGIC, 0, DEFAULT_INTERVAL_S, HydroNodeBatteryGuard::defaults(HydroNodeBatteryGuard::LIPO, CELLS)};
    }
}

void saveStored() {
#if defined(ESP32)
    Preferences prefs;
    prefs.begin("power", false);
    prefs.putBytes("stored", &stored, sizeof(stored));
    prefs.end();
#else
    EEPROM.begin(sizeof(Stored));
    EEPROM.put(0, stored);
    EEPROM.commit();
    EEPROM.end();
#endif
}

void loadRtc() {
#if defined(ESP8266)
    ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t*>(&rtc), sizeof(rtc));
#endif
    if (rtc.magic != RTC_MAGIC) {
        rtc.magic = RTC_MAGIC;
        rtc.clockSeconds = 0;
        guard.reset();
    } else {
        guard.restore(rtc.guard);
    }
}

void saveRtc() {
    rtc.guard = guard.memory();
#if defined(ESP8266)
    ESP.rtcUserMemoryWrite(0, reinterpret_cast<uint32_t*>(&rtc), sizeof(rtc));
#endif
}

/** Pack millivolts; false when the reading makes no sense (pin floating, divider broken). */
bool readBatteryMv(uint16_t& mv) {
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
#if defined(ESP32)
        sum += analogReadMilliVolts(BATTERY_PIN);
#else
        sum += analogRead(BATTERY_PIN) * 1000UL / 1023UL;
#endif
    }
    float value = (sum / 8.0f) * DIVIDER;
    mv = static_cast<uint16_t>(value);
    return value > 1000.0f && value < 5000.0f * CELLS;
}

/** Switch relays, pumps and LEDs off here: nothing may run while the battery recovers. */
void outputsOff() {}

bool takeSettings(const HydroNodeSettings& s) {
    stored.revision = s.revision;
    stored.intervalSeconds = s.intervalSeconds;
    stored.thresholds = {s.saveMv, s.recoveryMv, s.standbyMv, s.resumeMv};
    // The library checked the gaps; the range depends on the chemistry, check it here.
    if (HydroNodeBatteryGuard::validate(stored.thresholds, CELLS, HydroNodeBatteryGuard::LIPO) != 0) {
        loadStored();
        return false;
    }
    saveStored();
    guard.setThresholds(stored.thresholds);
    Serial.printf("New settings, revision %u\n", s.revision);
    return true;
}

void sendRound(uint16_t mv, bool valid) {
    hydro.setDebug(Serial);
    if (!hydro.connectWiFi(WIFI_SSID, WIFI_PASSWORD, 15000)) {
        Serial.println("WiFi failed, trying again next round");
        return;
    }
    hydro.begin();
    HydroNodeDeviceConfig config;
    config.intervalSeconds = stored.intervalSeconds;
    config.saveMv = stored.thresholds.saveMv;
    config.recoveryMv = stored.thresholds.recoveryMv;
    config.standbyMv = stored.thresholds.standbyMv;
    config.resumeMv = stored.thresholds.resumeMv;
    config.source = "bat";
    config.gauge = "adc";
    config.cells = CELLS;
    config.powerState = guard.stateName();
    config.revision = stored.revision;
    hydro.setDeviceConfig(config);
    hydro.onSettings(takeSettings);

    if (valid) {
        HydroNodeValue values[] = {{"BATTERY_VOLTAGE", mv / 1000.0f}};
        hydro.sendValues(values, 1);
    } else {
        hydro.reportReadError("battery");
        HydroNodeValue values[] = {{"RSSI", static_cast<float>(WiFi.RSSI())}};
        hydro.sendValues(values, 1);
    }
    hydro.closeConnection();
    WiFi.disconnect(true);
}

void sleepFor(uint32_t seconds) {
    rtc.clockSeconds += millis() / 1000UL + seconds;
    saveRtc();
    Serial.printf("%s, sleeping %lu s\n", guard.stateName(), static_cast<unsigned long>(seconds));
    Serial.flush();
    uint64_t us = static_cast<uint64_t>(seconds) * 1000000ULL;
#if defined(ESP32)
    esp_sleep_enable_timer_wakeup(us);
    esp_deep_sleep_start();
#else
    if (us > ESP.deepSleepMax()) us = ESP.deepSleepMax();
    ESP.deepSleep(us);
#endif
}

void setup() {
    Serial.begin(115200);
    loadStored();
    guard.setThresholds(stored.thresholds);
    loadRtc();

    uint16_t mv = 0;
    bool valid = readBatteryMv(mv);
    bool wasRunning = guard.radioAllowed();
    guard.update(mv, valid, rtc.clockSeconds + millis() / 1000UL);

    if (!guard.radioAllowed()) outputsOff();
    // Running, or just stopped: one last round tells HydroNode why the board goes quiet.
    if (guard.radioAllowed() || (wasRunning && guard.changed())) sendRound(mv, valid);
    sleepFor(guard.sleepSeconds(stored.intervalSeconds));
}

void loop() {}
