#include <WiFiManager.h>
#include <HydroNode.h>

#define RELAY_PIN D5
#define FAN_PIN   D6

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

    // *** WiFi using WiFiManager ***
    WiFiManager wm;
    String apName = hydro.getApName();
    bool res = wm.autoConnect(apName.c_str());
    if (!res) {
        Serial.println("WiFi failed, restarting");
        delay(3000);
        ESP.restart();
    }

    // *** Initialize HydroNode ***
    hydro.begin();

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