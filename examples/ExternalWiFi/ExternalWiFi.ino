/*
 * HydroNode ExternalWiFi — bring your own WiFi handling.
 *
 * The library never touches WiFi unless you call connectWiFi(), so it
 * composes cleanly with whatever connection management your project
 * already has (custom reconnect logic, ESP-IDF events, ethernet bridges,
 * enterprise WPA2, ...). This example shows a typical pattern: own
 * connect + reconnect logic, HydroNode only used for sending.
 *
 * Board: ESP32 (any variant)
 */
#include <WiFi.h>
#include <HydroNode.h>

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";
const char* SECRET_KEY    = "your-secret-key-here";

HydroNode hydro(SENSOR_ID, SECRET_KEY);

float readMySensor() {
    return 21.5;  // TODO: real measurement
}

bool wifiEnsureConnected() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.print("WiFi: (re)connecting");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 30000) {
            Serial.println(" timeout");
            return false;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf(" connected (%s)\n", WiFi.localIP().toString().c_str());
    return true;
}

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    wifiEnsureConnected();
    hydro.begin();
}

void loop() {
    if (!wifiEnsureConnected()) {
        delay(5000);
        return;
    }

    // sendValue() returns the HTTP status code or a negative error code,
    // so you can build your own retry/backoff strategy around it.
    int status = hydro.sendValue("TEMPERATURE", readMySensor());
    switch (status) {
        case 202:
            break;  // accepted
        case HydroNode::ERR_TIME_NOT_SYNCED:
            Serial.println("NTP sync failed — check internet access");
            break;
        case HydroNode::ERR_CONNECTION_FAILED:
            Serial.println("Could not reach the backend");
            break;
        case 401:
            Serial.println("Rejected — check sensor id / secret key");
            break;
        case 429:
            Serial.println("Too fast — min. 9 s between submissions");
            break;
        default:
            Serial.printf("Unexpected result: %d\n", status);
    }

    delay(10000);
}
