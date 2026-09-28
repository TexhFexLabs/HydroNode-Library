/*
 * HydroNode ActuatorControl: react to backend commands.
 *
 * Commands you queue in the HydroNode app travel back in the response of
 * the next sendValue() call:
 *
 *   {"commands":[{"id":"...","command":"pump","value":4000},
 *                {"id":"...","command":"fan","value":true}]}
 *
 * The library confirms receipt to the backend and then calls the handler
 * you registered with hydro.on(...) for each command name.
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

// {"command":"pump","value":4000} -> run the pump for 4000 ms
void pumpCallback(int ms) {
    if (ms <= 0) {
        return;
    }
    if (ms > MAX_PUMP_MS) {
        Serial.printf("Command: pump %d ms requested, limited to %d ms\n", ms, MAX_PUMP_MS);
        ms = MAX_PUMP_MS;
    }
    Serial.printf("Command: pump for %d ms\n", ms);
    digitalWrite(PUMP_RELAY_PIN, HIGH);
    delay(ms);
    digitalWrite(PUMP_RELAY_PIN, LOW);
}

// {"command":"fan","value":true} -> fan on, false -> fan off (active-low relay)
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

    // Register a handler per command name. Any JSON type works:
    // int, bool, float, String. bindCallback converts automatically.
    hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));
    hydro.on("fan",  HydroNode::bindCallback<bool>(fanCallback));
}

void loop() {
    // The rate limit is per type, so both values may go out back to back.
    // Each call also picks up commands that are waiting for this sensor.
    hydro.sendValue("TEMPERATURE", readTemperature());
    hydro.sendValue("HUMIDITY", readHumidity());

    delay(10000);
}
