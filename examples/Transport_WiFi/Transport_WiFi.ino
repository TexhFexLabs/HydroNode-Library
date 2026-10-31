/*
 * HydroNode — Transport_WiFi
 *
 * The same as QuickStart, written the 1.9.0 way: the network (transport) and the HydroNode
 * client are two separate objects. Start here if you want to understand how the library is
 * built, or if you plan to swap WiFi for Ethernet or LTE later: only the transport lines change.
 *
 *   HydroNodeWiFi wifi;                                  <- HOW the board reaches the internet
 *   HydroNodeClient hydro(wifi, SENSOR_ID, SECRET_KEY);  <- WHAT it sends (always the same)
 *
 * Swap the first line for another transport and keep everything else:
 *   HydroNodeEsp32Ethernet eth;                 examples/Ethernet_ESP32
 *   HydroNodeClientTransport net(tls, ...);     examples/Ethernet_W5500, Board_MKR_WiFi1010
 *   HydroNodeCellular<TinyGsm> lte(modem, ...); examples/Cellular_SendNow
 *
 * Board:     ESP32 (any variant) or ESP8266
 * Libraries: HydroNode-Library (installs ArduinoJson and ArduinoHttpClient with it)
 *
 * Change: the four values in block 1 and readTemperature()/readHumidity().
 */
#include <HydroNodeClient.h>
#include <HydroNodeWiFi.h>

// --- 1. Your credentials -------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";    // HydroNode: sensor settings, Connection
const char* SECRET_KEY    = "your-secret-key-here";   // same place, keep it private

// --- 2. Transport and client ---------------------------------------------
// The transport owns the TLS client (with the HydroNode root certificates) and the UDP socket
// for the time sync. The client signs every request and talks to the server through it.
HydroNodeWiFi wifi;
HydroNodeClient hydro(wifi, SENSOR_ID, SECRET_KEY);

// How often to measure. HydroNode accepts one value per type every 10 seconds.
const uint32_t INTERVAL_MS = 60000;

// --- 3. Your sensor --------------------------------------------------------
float readTemperature() { return 21.5; }  // TODO: real measurement (BME280, SHT4x, DS18B20 ...)
float readHumidity()    { return 48.0; }  // TODO

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);  // prints every step: WiFi, time sync, HTTP status

    // connect() is a helper. A link you bring up yourself (WiFi.begin(), WiFiManager) works the same:
    // the transport only asks WiFi.status() before each request.
    while (!wifi.connect(WIFI_SSID, WIFI_PASSWORD, 30000, &Serial)) {
        Serial.println("WiFi failed, retrying ...");
    }
}

void loop() {
    // All values of one reading in one request: one TLS round trip, one shared timestamp.
    HydroNodeValue values[] = {
        {"TEMPERATURE", readTemperature()},
        {"HUMIDITY", readHumidity()},
    };
    int codes[2];
    int status = hydro.sendValues(values, 2, codes);

    // 202 = accepted. Negative codes come from the board itself:
    //   -1 network down, -2 no valid time, -3 TLS/TCP error, -4 bad type name, -5 NaN/inf value
    if (status != 202) {
        Serial.printf("Send failed: %d (temperature %d, humidity %d)\n", status, codes[0], codes[1]);
    }
    delay(INTERVAL_MS);
}
