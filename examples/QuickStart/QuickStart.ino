/*
 * HydroNode QuickStart — the smallest complete sketch.
 *
 * Fill in the four values below, wire up your sensor in readMySensor(),
 * flash, done. The library handles WiFi, NTP time sync, TLS and
 * HMAC-SHA256 signing for you.
 *
 * Board: ESP32 (any variant)
 */
#include <HydroNode.h>

// --- 1. Your credentials -------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";    // from the HydroNode app
const char* SECRET_KEY    = "your-secret-key-here";   // from the HydroNode app

HydroNode hydro(SENSOR_ID, SECRET_KEY);

// --- 2. Your sensor ------------------------------------------------------
// Replace this with your actual sensor reading (DHT22, BME280, analog probe, ...).
float readMySensor() {
    return 21.5;  // TODO: real measurement
}

// --- 3. Setup & loop — nothing to change below ---------------------------
void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);  // optional: log what the library does

    while (!hydro.connectWiFi(WIFI_SSID, WIFI_PASSWORD)) {
        Serial.println("WiFi failed, retrying...");
    }
    hydro.begin();
}

void loop() {
    int status = hydro.sendValue("TEMPERATURE", readMySensor());
    if (status != 202) {
        Serial.printf("Send failed (code %d)\n", status);
    }
    // Backend enforces min. 9 s between submissions per sensor.
    delay(10000);
}
