/*
 * HydroNode ActuatorControl — react to backend commands.
 *
 * Each sendValue() call also picks up pending commands: when the backend
 * response contains JSON like {"pump": 4000, "fan": 1}, the matching
 * callbacks registered with hydro.on(...) are invoked automatically.
 *
 * This example sends alternating sensor values (respecting the 9 s
 * per-sensor rate limit) and drives a pump relay and a fan from
 * backend commands.
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

HydroNode hydro(SENSOR_ID, SECRET_KEY);

// {"pump": 4000} -> run the pump for 4000 ms
void pumpCallback(int ms) {
    Serial.printf("Command: pump for %d ms\n", ms);
    digitalWrite(PUMP_RELAY_PIN, HIGH);
    delay(ms);
    digitalWrite(PUMP_RELAY_PIN, LOW);
}

// {"fan": 1} -> fan on, {"fan": 0} -> fan off (active-low relay)
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

    // Register handlers for backend response keys. Any type works:
    // int, bool, float, String — bindCallback converts automatically.
    hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));
    hydro.on("fan",  HydroNode::bindCallback<bool>(fanCallback));
}

void loop() {
    // Alternate the reported type each cycle — the backend allows only
    // one submission per sensor per 9 s, regardless of type.
    static bool toggle = false;
    if (toggle) {
        hydro.sendValue("TEMPERATURE", readTemperature());
    } else {
        hydro.sendValue("HUMIDITY", readHumidity());
    }
    toggle = !toggle;

    delay(10000);
}
