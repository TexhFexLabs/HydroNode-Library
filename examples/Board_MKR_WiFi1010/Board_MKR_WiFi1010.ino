/*
 * HydroNode — Board_MKR_WiFi1010
 *
 * HydroNode on a board that is no ESP: Arduino MKR WiFi 1010 or Nano 33 IoT (SAMD21 with a
 * u-blox NINA WiFi module). The NINA module does TLS and the time itself, so the sketch only
 * hands its client to HydroNodeClientTransport.
 *
 * The same pattern fits other boards; only the include and the client change:
 *   Arduino UNO R4 WiFi ....... #include <WiFiS3.h>       WiFiSSLClient tls;   WiFi.getTime()
 *   Arduino Nano RP2040 Connect #include <WiFiNINA.h>     WiFiSSLClient tls;   WiFi.getTime()
 *   Raspberry Pi Pico W ....... #include <WiFi.h>         WiFiClientSecure tls; tls.setCACert(...)
 *                                                          and net.setUdp(udp) for SNTP
 *   Arduino MKR NB 1500 ....... #include <MKRNB.h>        NBSSLClient tls;     nbAccess.getTime()
 * Everything else (values, commands, settings) works exactly like on the ESP32.
 *
 * Libraries: HydroNode-Library, WiFiNINA (by Arduino)
 *
 * Root certificates: the NINA firmware brings its own list. If the TLS connection fails, update
 * the module firmware (Arduino IDE: Tools > Firmware Updater) and add hydronode.tech there.
 */
#include <WiFiNINA.h>
#include <HydroNodeClient.h>

// --- 1. Your credentials -------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";
const char* SECRET_KEY    = "your-secret-key-here";

// --- 2. Transport and client ---------------------------------------------
WiFiSSLClient tls;  // TLS inside the NINA module
HydroNodeClientTransport net(tls, HydroNodeNet::WIFI);
HydroNodeClient hydro(net, SENSOR_ID, SECRET_KEY);

const uint32_t INTERVAL_MS = 60000;

float readLight() { return analogRead(A1) * (100.0f / 1023.0f); }  // TODO: your sensor

void connectWiFi() {
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print("Connecting to ");
        Serial.println(WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        delay(5000);
    }
}

void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) {
    }
    hydro.setDebug(Serial);

    // What the transport cannot know by itself:
    net.onIsUp([] { return WiFi.status() == WL_CONNECTED; });
    net.onSignal([](int& dbm) {
        dbm = WiFi.RSSI();
        return dbm != 0;
    });
    // The NINA module keeps the time (it asks NTP itself): seconds since 1970, 0 while unknown.
    net.onNetworkTime([]() -> uint64_t { return uint64_t(WiFi.getTime()) * 1000ULL; });

    connectWiFi();
}

void loop() {
    connectWiFi();
    HydroNodeValue values[] = {{"LIGHT", readLight()}};
    int status = hydro.sendValues(values, 1);
    Serial.print("HTTP ");
    Serial.println(status);
    delay(INTERVAL_MS);
}
