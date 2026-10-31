/*
 * HydroNode — Cellular_CollectHourly
 *
 * LTE-M / NB-IoT on a battery: measure every 6 minutes, send the 10 readings once an hour in ONE
 * request. Every reading keeps the time it was measured, so the chart in HydroNode looks exactly
 * as if each one had been sent right away. Compare with Cellular_SendNow, which sends every reading.
 *
 *   Every 6 min   wake up, measure, store the round, deep sleep again    (no radio, ~1 s awake)
 *   Every hour    modem on, all stored rounds in one request, modem off  (~20 s awake)
 *
 * Why: the modem is by far the most power hungry part. Registering in the network and opening
 * TLS costs the same for one reading as for ten. One connection an hour instead of ten saves
 * roughly 80 % of the radio energy and most of the data volume (~1.5 KB instead of ~10 KB).
 *
 * What changes compared to sending right away:
 *   - Values arrive up to an hour late. Commands from HydroNode arrive with the hourly send too.
 *   - HydroNode learns the hourly rhythm: Delayed and Offline count from the deliveries.
 *   - If a send fails (no coverage), the rounds simply wait for the next hour. The buffer holds
 *     about a day (4 KB of RTC memory); when it is full the oldest rounds go first.
 *
 * Hardware: ESP32 + SIM7080G, wiring as in Cellular_SendNow. A battery and, ideally, a modem
 * supply you can switch off (the SIM7080G draws ~1 mA even when powered down via AT).
 * Libraries: HydroNode-Library, TinyGSM (LGPL-3.0), ESP_SSLClient (MIT)
 *
 * Change: credentials, APN, pins, MEASURE_EVERY_S, ROUNDS_PER_SEND.
 */
#define TINY_GSM_MODEM_SIM7080
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#include <ESP_SSLClient.h>
#include <HydroNodeClient.h>
#include <HydroNodeCellular.h>
#include <HydroNodeCerts.h>

// --- 1. Your credentials -------------------------------------------------
const char* SENSOR_ID  = "your-sensor-id-here";
const char* SECRET_KEY = "your-secret-key-here";
const char* APN        = "iot.1nce.net";

// --- 2. Your hardware ----------------------------------------------------
const int MODEM_RX = 4;
const int MODEM_TX = 5;
const int MODEM_PWRKEY = 41;

// --- 3. The rhythm ---------------------------------------------------------
const uint32_t MEASURE_EVERY_S = 360;  // one reading every 6 minutes
const size_t ROUNDS_PER_SEND = 10;     // send when 10 readings are stored ...
const uint32_t SEND_AT_LATEST_S = 3600;  // ... or the oldest is an hour old (e.g. after a failure)
const uint32_t RETRY_AFTER_S = 3600;     // after a failed send, wait this long before the next try

// When the last send failed (Unix seconds, 0 = it did not). In RTC memory like the buffer, so a
// basement without coverage does not switch the modem on at every wake-up.
RTC_DATA_ATTR uint32_t lastFailureS = 0;

// --- 4. The buffer -----------------------------------------------------------
// RTC memory keeps its content through deep sleep (not through a power cut). 4 KB hold about
// a hundred rounds of two values: a day without coverage.
RTC_DATA_ATTR uint8_t bufferMemory[4096];
HydroNodeBuffer buffer(bufferMemory, sizeof(bufferMemory));

// --- 5. Network objects --------------------------------------------------
#define SerialAT Serial1
TinyGsm modem(SerialAT);
TinyGsmClient tcp(modem, 0);
ESP_SSLClient tls;
HydroNodeCellular<TinyGsm> lte(modem, tls, HydroNodeNet::LTE_M);
HydroNodeClient hydro(lte, SENSOR_ID, SECRET_KEY);

float readTemperature() { return 12.3; }  // TODO: real measurement
float readSoilMoisture() { return 37.0; }  // TODO

bool modemUp() {
    pinMode(MODEM_PWRKEY, OUTPUT);
    digitalWrite(MODEM_PWRKEY, LOW);
    delay(1200);
    digitalWrite(MODEM_PWRKEY, HIGH);
    if (!modem.testAT(10000)) return false;
    modem.init();
    modem.setNetworkMode(38);
    modem.setPreferredMode(1);  // LTE-M; 2 = NB-IoT
    return modem.waitForNetwork(180000L) && modem.gprsConnect(APN);
}

void modemDown() {
    hydro.closeConnection();
    modem.gprsDisconnect();
    modem.poweroff();
}

void setup() {
    Serial.begin(115200);
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    hydro.setDebug(Serial);
    hydro.setHttpTimeout(30000);
    hydro.setRoundsPerRequest(ROUNDS_PER_SEND);

    tls.setClient(&tcp);
    tls.setCACert(HYDRONODE_CA_BUNDLE);
    tls.setBufferSizes(4096, 2048);
    lte.onBeforeConnect([](uint32_t now) { tls.setX509Time(now); });

    // Keeps the rounds of the last wake-ups when the content checks out (it does after deep sleep).
    buffer.begin();
    hydro.attachBuffer(buffer);

    // First start after power-on: the clock is not set yet. Rounds would still be stored and dated
    // later, but setting the clock now keeps it simple. The ESP32 clock then keeps running through
    // deep sleep; the library syncs again every 30 minutes of sending.
    if (hydro.epochMs() == 0 && modemUp()) {
        hydro.syncTime();
        modemDown();
    }

    // 1. Measure and store. No radio.
    HydroNodeValue values[] = {
        {"TEMPERATURE", readTemperature()},
        {"SOIL_MOISTURE", readSoilMoisture()},
    };
    hydro.record(values, 2);
    Serial.printf("%u rounds stored, %u %% of the buffer\n", (unsigned)buffer.rounds(), buffer.fillPercent());

    // 2. Once an hour (or when enough is stored): one request for all of them.
    uint32_t nowS = (uint32_t)(hydro.epochMs() / 1000ULL);
    bool waiting = lastFailureS != 0 && nowS - lastFailureS < RETRY_AFTER_S;
    if (!waiting && hydro.flushDue(ROUNDS_PER_SEND, SEND_AT_LATEST_S)) {
        int status = modemUp() ? hydro.flush() : HydroNodeClient::ERR_NETWORK_DOWN;
        // 202 = all sent. Anything else: the rounds stay in the buffer for the next try.
        lastFailureS = status == 202 ? 0 : nowS;
        Serial.printf("Send: %d, %u rounds left\n", status, (unsigned)buffer.rounds());
        modemDown();
    }

    // 3. Sleep until the next reading.
    Serial.flush();
    esp_sleep_enable_timer_wakeup(uint64_t(MEASURE_EVERY_S) * 1000000ULL);
    esp_deep_sleep_start();
}

void loop() {
    // Never reached: every wake-up runs setup() and sleeps again.
}
