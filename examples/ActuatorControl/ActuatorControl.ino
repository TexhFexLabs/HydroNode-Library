/*
 * HydroNode ActuatorControl: react to backend commands.
 *
 * Commands you queue in the HydroNode app travel back in the response of
 * the next sendValue() call. Each command has a name, a value type and a
 * value. Register one callback per name with the type you picked in the app:
 *
 *   App: name "pump", type UINT32, value 4000  ->  hydro.onUInt32("pump", ...)
 *   App: name "fan",  type BOOL,   value true  ->  hydro.onBool("fan", ...)
 *   App: name "co2_calibration", type UINT32, value 0x20124
 *                                              ->  hydro.onUInt32("co2_calibration", ...)
 *
 * A command without a matching callback, or with a different type, is not
 * run. The app shows it as declined with the reason.
 *
 * This example reports temperature and humidity every 10 seconds and
 * drives a pump relay and a fan from backend commands.
 *
 * Board: ESP32 (any variant)
 */
#include <HydroNode.h>

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";
const char* SECRET_KEY    = "your-secret-key-here";

#define PUMP_RELAY_PIN 5
#define FAN_PIN        6

// Upper limit for a single pump run. The value comes over the network, so
// the firmware decides what is physically safe, not the command. A typo in
// the app (40000 instead of 4000) must not flood your plants.
const int MAX_PUMP_MS = 10000;

HydroNode hydro(SENSOR_ID, SECRET_KEY);

// "pump" (UINT32): run the pump for the given milliseconds
void pumpCallback(uint32_t ms) {
    if (ms == 0) {
        return;
    }
    if (ms > (uint32_t)MAX_PUMP_MS) {
        Serial.printf("Command: pump %lu ms requested, limited to %d ms\n", (unsigned long)ms, MAX_PUMP_MS);
        ms = MAX_PUMP_MS;
    }
    Serial.printf("Command: pump for %lu ms\n", (unsigned long)ms);
    digitalWrite(PUMP_RELAY_PIN, HIGH);
    delay(ms);
    digitalWrite(PUMP_RELAY_PIN, LOW);
}

// "fan" (BOOL): true -> fan on, false -> fan off (active-low relay)
void fanCallback(bool on) {
    Serial.printf("Command: fan %s\n", on ? "on" : "off");
    digitalWrite(FAN_PIN, on ? LOW : HIGH);
}

float readTemperature() { return 21.5; }  // TODO: real measurement
float readHumidity()    { return 48.0; }  // TODO: real measurement

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    pinMode(PUMP_RELAY_PIN, OUTPUT);
    digitalWrite(PUMP_RELAY_PIN, LOW);
    pinMode(FAN_PIN, OUTPUT);
    digitalWrite(FAN_PIN, HIGH);

    while (!hydro.connectWiFi(WIFI_SSID, WIFI_PASSWORD)) {
        Serial.println("WiFi failed, retrying...");
    }
    hydro.begin();

    // One callback per command name. The type must match the app.
    hydro.onUInt32("pump", pumpCallback);
    hydro.onBool("fan", fanCallback);

    // Lambdas work too, e.g. a calibration register sent as 0x20124:
    hydro.onUInt32("co2_calibration", [](uint32_t value) {
        Serial.printf("Command: CO2 calibration 0x%lX\n", (unsigned long)value);
        // co2Sensor.setCalibration(value);
    });
}

void loop() {
    // The rate limit is per type, so both values may go out back to back.
    // Each call also picks up commands that are waiting for this sensor.
    hydro.sendValue("TEMPERATURE", readTemperature());
    hydro.sendValue("HUMIDITY", readHumidity());

    delay(10000);
}
