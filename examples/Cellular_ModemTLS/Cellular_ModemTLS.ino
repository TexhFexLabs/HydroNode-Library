/*
 * HydroNode — Cellular_ModemTLS
 *
 * LTE Cat-1 with the modem doing TLS itself (A7670E/G/SA, the LilyGO T-A7670 board). The
 * microcontroller only sees plain bytes: no TLS library, ~40 KB less RAM. Good for small boards
 * or when the RAM is needed elsewhere.
 *
 * The other way, TLS on the microcontroller (ESP_SSLClient), works with every modem and every
 * modem firmware: examples/Cellular_SendNow. Pick that one if this one does not connect.
 *
 * Hardware
 *   ESP32 + A7670 (or SIM7600, SIM7080G, SIM7000 with SSL firmware, BG96: change the
 *   TINY_GSM_MODEM_... line). LilyGO T-A7670: RX 27, TX 26, PWRKEY 4, POWER_ON 12.
 *
 * Libraries: HydroNode-Library, TinyGSM (LGPL-3.0)
 *
 * Certificates: the modem has to trust the root certificate of hydronode.tech. The sketch
 * uploads the bundled roots into the modem's file system once (UPLOAD_CERTIFICATE) and turns on
 * the server check (authmode 1). Without that check the modem would accept any server: the
 * HMAC signature still keeps your values from being changed or forged, but anyone in between
 * could read them. Keep the check on.
 */
#define TINY_GSM_MODEM_A7672X
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#include <HydroNodeClient.h>
#include <HydroNodeCellular.h>
#include <HydroNodeCerts.h>

// --- 1. Your credentials -------------------------------------------------
const char* SENSOR_ID  = "your-sensor-id-here";
const char* SECRET_KEY = "your-secret-key-here";
const char* APN        = "internet";   // your SIM's APN

// --- 2. Your hardware (LilyGO T-A7670) -------------------------------------
const int MODEM_RX = 27;
const int MODEM_TX = 26;
const int MODEM_PWRKEY = 4;
const int MODEM_POWER_ON = 12;

// Upload the root certificates into the modem. Needed once per modem (they stay in its flash);
// leaving it on costs a few seconds per boot.
const bool UPLOAD_CERTIFICATE = true;
const char* CERT_FILE = "hydronode.pem";

const uint32_t INTERVAL_MS = 2UL * 60UL * 1000UL;

// --- 3. Network objects --------------------------------------------------
#define SerialAT Serial1
TinyGsm modem(SerialAT);
TinyGsmClientSecure tls(modem, 0);   // TLS inside the modem
HydroNodeCellular<TinyGsm> lte(modem, tls, HydroNodeNet::LTE);
HydroNodeClient hydro(lte, SENSOR_ID, SECRET_KEY);

float readPower() { return 412.0; }  // TODO: real measurement, e.g. an energy meter in watts

bool modemUp() {
    if (modem.isGprsConnected()) return true;
    if (!modem.testAT(1000)) {
        pinMode(MODEM_POWER_ON, OUTPUT);
        digitalWrite(MODEM_POWER_ON, HIGH);
        pinMode(MODEM_PWRKEY, OUTPUT);
        digitalWrite(MODEM_PWRKEY, HIGH);
        delay(300);
        digitalWrite(MODEM_PWRKEY, LOW);
        if (!modem.testAT(15000)) return false;
    }
    modem.init();
    if (!modem.waitForNetwork(120000L)) return false;
    return modem.gprsConnect(APN);
}

void setup() {
    Serial.begin(115200);
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    hydro.setDebug(Serial);
    hydro.setHttpTimeout(30000);

    if (!modemUp()) Serial.println("No network yet");

    if (UPLOAD_CERTIFICATE) {
        modem.deleteCertificate(CERT_FILE);
        bool ok = modem.addCertificate(CERT_FILE, HYDRONODE_CA_BUNDLE, strlen(HYDRONODE_CA_BUNDLE));
        Serial.println(ok ? "Certificate stored in the modem" : "Certificate upload failed");
    }
    tls.setCertificate(CERT_FILE);
    // Check the server certificate against it (authmode 1 = server authentication).
    modem.sendAT("+CSSLCFG=\"authmode\",0,1");
    modem.waitResponse();
}

void loop() {
    if (!modemUp()) {
        delay(30000);
        return;
    }
    HydroNodeValue values[] = {{"POWER", readPower()}};
    Serial.printf("HTTP %d\n", hydro.sendValues(values, 1));
    delay(INTERVAL_MS);
}
