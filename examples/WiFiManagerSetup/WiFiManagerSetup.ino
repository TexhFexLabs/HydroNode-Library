/*
 * HydroNode WiFiManagerSetup — captive-portal WiFi configuration.
 *
 * No hardcoded WiFi credentials: on first boot the device opens an access
 * point called "HydroNode-Setup-XXXX". Connect to it with your phone and
 * enter your WiFi credentials in the portal — they are stored in flash
 * and reused on every subsequent boot.
 *
 * Requires the WiFiManager library (tzapu), install via Library Manager.
 *
 * Board: ESP32 (any variant)
 */
#include <WiFiManager.h>
#include <HydroNode.h>

const char* SENSOR_ID  = "your-sensor-id-here";    // from the HydroNode app
const char* SECRET_KEY = "your-secret-key-here";   // from the HydroNode app

HydroNode hydro(SENSOR_ID, SECRET_KEY);

// Replace with your actual sensor reading.
float readMySensor() {
    return 21.5;  // TODO: real measurement
}

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    // Captive portal: AP name is derived from the sensor id so multiple
    // devices next to each other stay distinguishable.
    WiFiManager wm;
    if (!wm.autoConnect(hydro.getApName().c_str())) {
        Serial.println("WiFi setup failed, restarting...");
        delay(3000);
        ESP.restart();
    }

    hydro.begin();
}

void loop() {
    int status = hydro.sendValue("TEMPERATURE", readMySensor());
    if (status != 202) {
        Serial.printf("Send failed (code %d)\n", status);
    }
    delay(10000);
}
