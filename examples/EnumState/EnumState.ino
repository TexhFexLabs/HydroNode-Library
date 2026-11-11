/*
 * HydroNode EnumState: a state the device sets and reports back.
 *
 * A heater with three modes: Off (0), Eco (1), Boost (2). The app sends the
 * mode as a command, the device switches and reports the mode it is in as a
 * value. HydroNode shows the mode as "Confirmed" once the reported value
 * matches what you picked, and draws it as a band under the charts.
 *
 * In the web app (once, after the device sent its first HEATER_MODE value):
 *   1. Open the data point HEATER_MODE (sensor page or project page).
 *   2. Settings, "Read as": Choice. Options 0 Off, 1 Eco, 2 Boost.
 *   3. Command: "Send a command when it is set", name heater_mode, type UINT32.
 *   Save. The mode now shows under Controls on the sensor page.
 * Or: Add, Actuator, pick this device, "Does the device report its state?"
 * Yes, pick HEATER_MODE.
 *
 * A command arrives in the response of the next sendValue(). The app only
 * sends the index of an option; the range check below guards against a value
 * typed by hand in the console. The device then keeps its mode and reports it,
 * so the band never shows a mode the heater is not in.
 *
 * Board: ESP32 (any variant)
 */
#include <HydroNode.h>

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";
const char* SECRET_KEY    = "your-secret-key-here";

#define HEATER_LOW_PIN  5   // relay for the small heating element
#define HEATER_HIGH_PIN 6   // relay for the large heating element

enum HeaterMode : uint32_t { HEATER_OFF = 0, HEATER_ECO = 1, HEATER_BOOST = 2 };
const uint32_t HEATER_MODE_COUNT = 3;

HydroNode hydro(SENSOR_ID, SECRET_KEY);

uint32_t heaterMode = HEATER_OFF;
// Set in the callback, reported from loop(): a command arrives while
// sendValue() runs, so the report waits for the next call.
bool reportMode = true;

void applyHeaterMode(uint32_t mode) {
    digitalWrite(HEATER_LOW_PIN,  mode == HEATER_ECO   || mode == HEATER_BOOST ? HIGH : LOW);
    digitalWrite(HEATER_HIGH_PIN, mode == HEATER_BOOST ? HIGH : LOW);
}

// "heater_mode" (UINT32): 0 Off, 1 Eco, 2 Boost
void heaterModeCallback(uint32_t mode) {
    if (mode >= HEATER_MODE_COUNT) {
        // Out of range: keep the current mode. The reported value stays the
        // old one, so the app never shows a mode the heater is not in.
        Serial.printf("Command: heater_mode %lu is not a mode, ignored\n", (unsigned long)mode);
        reportMode = true;
        return;
    }
    Serial.printf("Command: heater_mode %lu\n", (unsigned long)mode);
    heaterMode = mode;
    applyHeaterMode(heaterMode);
    reportMode = true;
}

float readWaterTemperature() { return 24.5; }  // TODO: real measurement

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    pinMode(HEATER_LOW_PIN, OUTPUT);
    pinMode(HEATER_HIGH_PIN, OUTPUT);
    applyHeaterMode(heaterMode);

    while (!hydro.connectWiFi(WIFI_SSID, WIFI_PASSWORD)) {
        Serial.println("WiFi failed, retrying...");
    }
    hydro.begin();

    // The type must match the binding in the app (UINT32 for a choice).
    hydro.onUInt32("heater_mode", heaterModeCallback);
}

unsigned long lastRound = 0;
unsigned long lastModeReport = 0;

void sendMode() {
    // One value per type every 10 s at most (backend rate limit).
    if (lastModeReport != 0 && millis() - lastModeReport < 10000) return;
    int status = hydro.sendValue("HEATER_MODE", (float)heaterMode);
    if (status >= 200 && status < 300) {
        lastModeReport = millis();
        reportMode = false;
    }
}

void loop() {
    // Report the mode right after it changed, so the app confirms within seconds.
    if (reportMode) {
        sendMode();
    }
    if (lastRound == 0 || millis() - lastRound >= 30000) {
        lastRound = millis();
        hydro.sendValue("WATER_TEMPERATURE", readWaterTemperature());
        reportMode = true;
        sendMode();
    }
    delay(1000);
}
