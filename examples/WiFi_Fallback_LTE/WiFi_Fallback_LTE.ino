/*
 * HydroNode — WiFi_Fallback_LTE
 *
 * WiFi first, LTE-M when WiFi is gone. For a board at the edge of the WiFi range (garden shed,
 * barn) or one that must keep reporting when the router is down. WiFi costs nothing and uses less
 * power, so it stays preferred: as soon as WiFi is back, requests go over WiFi again.
 *
 *   HydroNodeWiFi wifi;                        first choice
 *   HydroNodeCellular<TinyGsm> lte(...);       second choice
 *   HydroNodeFailover net(wifi, lte);          picks per request: WiFi if up, else LTE
 *   HydroNodeClient hydro(net, ...);           does not notice the switch
 *
 * The fleet view in HydroNode shows which network the last request used (WiFi or LTE-M).
 *
 * Hardware: ESP32 + SIM7080G (see Cellular_SendNow for wiring and modem notes).
 * Libraries: HydroNode-Library, TinyGSM (LGPL-3.0), ESP_SSLClient (MIT)
 *
 * Change: credentials, APN, pins. LTE_STANDBY decides whether the modem stays attached while WiFi
 * works (fast switch, more power) or is only switched on when WiFi fails (slow switch, less power).
 */
#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#include <ESP_SSLClient.h>
#include <HydroNodeClient.h>
#include <HydroNodeWiFi.h>
#include <HydroNodeCellular.h>
#include <HydroNodeCerts.h>

// --- 1. Your credentials -------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";
const char* SECRET_KEY    = "your-secret-key-here";
const char* APN           = "iot.1nce.net";

// --- 2. Your hardware ----------------------------------------------------
const int MODEM_RX = 4;
const int MODEM_TX = 5;
const int MODEM_PWRKEY = 41;

// true: modem stays attached (switch within seconds). false: modem starts only when WiFi fails.
const bool LTE_STANDBY = false;

const uint32_t INTERVAL_MS = 60000;

// --- 3. Network objects --------------------------------------------------
#define SerialAT Serial1
TinyGsm modem(SerialAT);
TinyGsmClient tcp(modem, 0);
ESP_SSLClient tls;

HydroNodeWiFi wifi;
HydroNodeCellular<TinyGsm> lte(modem, tls, HydroNodeNet::LTE_M);
HydroNodeFailover net(wifi, lte);
HydroNodeClient hydro(net, SENSOR_ID, SECRET_KEY);

float readTemperature() { return 17.4; }  // TODO: real measurement

bool modemUp() {
    if (modem.isGprsConnected()) return true;
    if (!modem.testAT(1000)) {
        pinMode(MODEM_PWRKEY, OUTPUT);
        digitalWrite(MODEM_PWRKEY, LOW);
        delay(1200);
        digitalWrite(MODEM_PWRKEY, HIGH);
        if (!modem.testAT(10000)) return false;
    }
    modem.init();
    modem.setNetworkMode(38);
    modem.setPreferredMode(1);  // LTE-M
    return modem.waitForNetwork(180000L) && modem.gprsConnect(APN);
}

void setup() {
    Serial.begin(115200);
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    hydro.setDebug(Serial);
    hydro.setHttpTimeout(30000);

    tls.setClient(&tcp);
    tls.setCACert(HYDRONODE_CA_BUNDLE);
    tls.setBufferSizes(4096, 1024);
    lte.onBeforeConnect([](uint32_t now) { tls.setX509Time(now); });

    // WiFi reconnects on its own after the first connect (the ESP core retries in the background).
    WiFi.setAutoReconnect(true);
    wifi.connect(WIFI_SSID, WIFI_PASSWORD, 15000, &Serial);
    if (LTE_STANDBY) modemUp();
}

void loop() {
    // Bring LTE up only when it is needed (or always with LTE_STANDBY).
    if (!wifi.isUp() || LTE_STANDBY) modemUp();

    HydroNodeValue values[] = {{"TEMPERATURE", readTemperature()}};
    int status = hydro.sendValues(values, 1);
    Serial.printf("HTTP %d via %s\n", status, net.active().network());

    // Back on WiFi: switch the modem off again to save power.
    if (wifi.isUp() && !LTE_STANDBY && modem.isGprsConnected()) {
        tls.stop();  // the LTE connection; requests already run over WiFi again
        modem.gprsDisconnect();
        modem.poweroff();
    }
    delay(INTERVAL_MS);
}
