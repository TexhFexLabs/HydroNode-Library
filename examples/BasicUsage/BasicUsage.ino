#include <ESP8266WiFi.h>   // oder #include <WiFi.h> for ESP32
#include <HydroNode.h>

#define RELAY_PIN 5
#define FAN_PIN   6

// WIFI-Credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

HydroNode hydro(
    "your-sensor-id-here",
    "your-secret-key-here"
);

void pumpCallback(int ms) {
    digitalWrite(RELAY_PIN, HIGH);
    delay(ms);
    digitalWrite(RELAY_PIN, LOW);
}

void fanCallback(bool on) {
    digitalWrite(FAN_PIN, on ? LOW : HIGH);
}

void setup() {
    Serial.begin(115200);

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    pinMode(FAN_PIN, OUTPUT);
    digitalWrite(FAN_PIN, HIGH);

    // *** WIFI-Connect ***
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // *** Initialize HydroNode ***
    hydro.begin();
    hydro.setJsonBufferSize(256);

    // *** Register event handlers for backend commands ***
    hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));    // For {"pump": 4000}
    hydro.on("fan",  HydroNode::bindCallback<bool>(fanCallback));    // For {"fan": 1} or {"fan": 0}
    // Register any other handlers for backend response keys as needed!
}

void loop() {
    // Send example data
    hydro.sendValue("temperature", 21.5);
    delay(10000);
}
