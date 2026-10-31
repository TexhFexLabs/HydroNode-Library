/*
 * HydroNode — Cellular_SendNow
 *
 * LTE-M / NB-IoT: every reading goes out right away. For places without WiFi: a field, an
 * allotment, a weather mast. The values arrive within seconds, like with WiFi.
 *
 * Want to save data and battery instead? Collect readings and send once an hour:
 * examples/Cellular_CollectHourly.
 *
 * Hardware
 *   ESP32 (any) + SIM7080G (LTE-M and NB-IoT), e.g. a SIM7080G breakout or the LilyGO
 *   T-SIM7080G-S3 board. Other modems: change TINY_GSM_MODEM_... (SIM7000, BG95, SARA-R4 for
 *   LTE-M/NB-IoT; A7670/SIM7600 for Cat-1, see Cellular_ModemTLS).
 *   A SIM card with data: IoT SIMs like 1NCE, Hologram, EMnify or any normal SIM with LTE-M.
 *
 * Wiring (breakout): modem TX -> MODEM_RX, modem RX -> MODEM_TX, PWRKEY -> MODEM_PWRKEY,
 *   GND, and a supply that delivers 2 A peaks (the modem draws bursts while sending).
 *   LilyGO T-SIM7080G-S3: RX 4, TX 5, PWRKEY 41, and enable the modem supply through its PMU
 *   (XPowersLib, see LilyGO's examples) before modemUp().
 *
 * Libraries (Arduino Library Manager):
 *   HydroNode-Library, TinyGSM (by Volodymyr Shymanskyy, LGPL-3.0), ESP_SSLClient (by Mobizt, MIT)
 *
 * Change: credentials, APN of your SIM, pins, LTE-M or NB-IoT (RADIO_MODE).
 *
 * How it works
 *   TinyGsmClient     plain TCP through the modem
 *   ESP_SSLClient     TLS on the ESP32, checks the HydroNode certificate (bundled roots)
 *   HydroNodeCellular the transport: link up = modem attached, time = the modem's clock
 *                     (NTP through the modem, else the operator's time), signal = CSQ in dBm
 *   HydroNodeClient   signs and sends, the same as with WiFi
 *   The TLS connection stays open between readings (keep-alive): no new handshake each time.
 *
 * Data use: one reading ~1 KB with an open connection, ~6 KB with a fresh TLS handshake.
 */
#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024  // the modem's answers, 1 KB keeps TLS records flowing
#include <TinyGsmClient.h>
#include <ESP_SSLClient.h>
#include <HydroNodeClient.h>
#include <HydroNodeCellular.h>
#include <HydroNodeCerts.h>

// --- 1. Your credentials -------------------------------------------------
const char* SENSOR_ID  = "your-sensor-id-here";
const char* SECRET_KEY = "your-secret-key-here";
const char* APN        = "iot.1nce.net";   // your SIM's APN
const char* SIM_PIN    = "";               // empty when the SIM has no PIN

// --- 2. Your hardware ----------------------------------------------------
const int MODEM_RX = 4;       // ESP32 pin that receives from the modem
const int MODEM_TX = 5;       // ESP32 pin that sends to the modem
const int MODEM_PWRKEY = 41;  // pulse low to switch the modem on

// 1 = LTE-M (Cat-M1, faster, works while moving), 2 = NB-IoT (deeper indoor coverage),
// 3 = both (the modem picks). Ask your SIM provider which one it supports where you are.
const uint8_t RADIO_MODE = 1;

const uint32_t INTERVAL_MS = 5UL * 60UL * 1000UL;  // one reading every 5 minutes

// --- 3. Network objects --------------------------------------------------
#define SerialAT Serial1
TinyGsm modem(SerialAT);
TinyGsmClient tcp(modem, 0);
ESP_SSLClient tls;
HydroNodeCellular<TinyGsm> lte(modem, tls, RADIO_MODE == 2 ? HydroNodeNet::NB_IOT : HydroNodeNet::LTE_M);
HydroNodeClient hydro(lte, SENSOR_ID, SECRET_KEY);

float readTemperature() { return 14.2; }  // TODO: real measurement
float readHumidity()    { return 81.0; }  // TODO

/** Switches the modem on, registers in the network and opens the data connection. */
bool modemUp() {
    if (modem.isGprsConnected()) return true;
    if (!modem.testAT(1000)) {
        // PWRKEY pulse: the SIM7080G switches on after ~1 s low.
        pinMode(MODEM_PWRKEY, OUTPUT);
        digitalWrite(MODEM_PWRKEY, LOW);
        delay(1200);
        digitalWrite(MODEM_PWRKEY, HIGH);
        if (!modem.testAT(10000)) return false;
    }
    modem.init(SIM_PIN);
    modem.setNetworkMode(38);           // LTE only (no 2G fallback)
    modem.setPreferredMode(RADIO_MODE); // LTE-M, NB-IoT or both
    Serial.println("Waiting for the network ...");
    if (!modem.waitForNetwork(180000L)) return false;  // NB-IoT may take a few minutes the first time
    return modem.gprsConnect(APN);
}

void setup() {
    Serial.begin(115200);
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    hydro.setDebug(Serial);
    hydro.setHttpTimeout(30000);  // radio round trips are slower than WiFi, NB-IoT most of all

    tls.setClient(&tcp);
    tls.setCACert(HYDRONODE_CA_BUNDLE);
    tls.setBufferSizes(4096, 1024);
    tls.setSessionTimeout(300);  // keep the TLS session for the next reading
    lte.onBeforeConnect([](uint32_t now) { tls.setX509Time(now); });

    if (!modemUp()) Serial.println("No network yet, trying again with the first reading");
}

void loop() {
    if (!modemUp()) {
        Serial.println("Still no network");
        delay(30000);
        return;
    }

    HydroNodeValue values[] = {
        {"TEMPERATURE", readTemperature()},
        {"HUMIDITY", readHumidity()},
    };
    int status = hydro.sendValues(values, 2);
    int dbm = 0;
    Serial.printf("HTTP %d, signal %s dBm\n", status, lte.signalDbm(dbm) ? String(dbm).c_str() : "?");
    delay(INTERVAL_MS);
}
