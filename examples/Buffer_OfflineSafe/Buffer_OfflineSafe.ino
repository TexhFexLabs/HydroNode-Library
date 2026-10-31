/*
 * HydroNode — Buffer_OfflineSafe
 *
 * Never lose a reading. The board sends every minute over WiFi as usual; when WiFi or the
 * internet is gone (router restart, outage), the readings wait in a buffer and go out, with the
 * time they were measured, as soon as a send works again. The buffer is also saved to flash, so
 * even a power cut in the middle of an outage loses nothing.
 *
 * The only difference to a normal sketch is attachBuffer(): sendValues() then stores a round it
 * could not send (it returns ERR_STORED, -7) and sends the stored ones after the next success.
 *
 *   Online      sendValues() -> 202, nothing stored
 *   Offline     sendValues() -> -7 (stored), buffer grows: 1, 2, 3 ... rounds
 *   Back        sendValues() -> 202, then the stored rounds follow in one request each 12
 *
 * Board: ESP32 or ESP8266 (LittleFS is part of both cores).
 * Libraries: HydroNode-Library
 *
 * Change: credentials, INTERVAL_MS. SAVE_TO_FLASH = false keeps the buffer in RAM only (fewer
 * flash writes; a power cut then loses the stored rounds).
 */
#include <HydroNode.h>
#include <LittleFS.h>

// --- 1. Your credentials -------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";
const char* SECRET_KEY    = "your-secret-key-here";

const uint32_t INTERVAL_MS = 60000;
const bool SAVE_TO_FLASH = true;
const char* BUFFER_FILE = "/hydronode-buffer.bin";

// --- 2. Client and buffer ------------------------------------------------
HydroNode hydro(SENSOR_ID, SECRET_KEY);

// 8 KB in RAM: about 200 rounds of two values, more than three hours of outage at one round per
// minute. When it is full the oldest rounds make room (DROP_NEWEST keeps the old ones instead).
static uint8_t bufferMemory[8192];
HydroNodeBuffer buffer(bufferMemory, sizeof(bufferMemory), HydroNodeBuffer::DROP_OLDEST);
size_t savedRounds = 0;

float readTemperature() { return 23.1; }  // TODO: real measurement
float readCO2() { return 640; }            // TODO

/** Loads the buffer from flash (after a power cut). begin() checks the content and keeps it only when it is intact. */
void loadBuffer() {
    if (!SAVE_TO_FLASH) return;
    File file = LittleFS.open(BUFFER_FILE, "r");
    if (!file) return;
    file.read(buffer.data(), buffer.size());
    file.close();
}

/** Writes the buffer when the number of stored rounds changed (not on every loop: flash wears). */
void saveBufferIfChanged() {
    if (!SAVE_TO_FLASH || buffer.rounds() == savedRounds) return;
    File file = LittleFS.open(BUFFER_FILE, "w");
    if (!file) return;
    // Only the used part matters; begin() reads the header and ignores the rest.
    file.write(buffer.data(), HydroNodeBuffer::HEADER_SIZE + buffer.usedBytes());
    file.close();
    savedRounds = buffer.rounds();
}

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    if (SAVE_TO_FLASH && !LittleFS.begin()) {
        LittleFS.format();
        LittleFS.begin();
    }
    loadBuffer();
    savedRounds = buffer.begin();
    Serial.printf("%u rounds waiting from before\n", (unsigned)savedRounds);
    hydro.attachBuffer(buffer);

    WiFi.setAutoReconnect(true);
    hydro.connectWiFi(WIFI_SSID, WIFI_PASSWORD, 20000);
}

void loop() {
    HydroNodeValue values[] = {
        {"TEMPERATURE", readTemperature()},
        {"CO2", readCO2()},
    };
    int status = hydro.sendValues(values, 2);

    if (status == HydroNodeClient::ERR_STORED) {
        Serial.printf("Offline: stored, %u rounds waiting (%u %% full)\n", (unsigned)buffer.rounds(),
                      buffer.fillPercent());
    } else {
        Serial.printf("HTTP %d, %u rounds waiting\n", status, (unsigned)buffer.rounds());
    }
    saveBufferIfChanged();
    delay(INTERVAL_MS);
}
