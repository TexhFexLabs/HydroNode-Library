/*
 * HydroNode — Ethernet_ESP32
 *
 * Sends over the Ethernet port built into the ESP32: a cable instead of WiFi, ideal for a
 * greenhouse controller, a pump room or anything mounted next to a switch. With PoE boards one
 * cable carries power and data.
 *
 * Boards (ESP32 with an RMII PHY, Arduino core 3.x):
 *   WT32-ETH01 ............ LAN8720, address 1, MDC 23, MDIO 18, power 16, clock GPIO0 in
 *   Olimex ESP32-POE(-ISO)  LAN8720, address 0, MDC 23, MDIO 18, power 12, clock GPIO17 out
 *   wESP32, LilyGO T-ETH .. see the board's documentation, same idea
 * Pick your board in block 2. A W5500 module on SPI is something else: see Ethernet_W5500.
 *
 * Libraries: HydroNode-Library (nothing else; ETH.h is part of the ESP32 core)
 *
 * How it works
 *   The ESP32 runs one network stack for WiFi and Ethernet, so HydroNodeEsp32Ethernet reuses the
 *   WiFi transport (TLS with the HydroNode root certificates, time by SNTP) and only asks the
 *   Ethernet port whether it is up. HydroNode shows "Ethernet" in the fleet view (net=eth).
 */
#include <ETH.h>
#include <HydroNodeClient.h>
#include <HydroNodeEsp32Ethernet.h>

// --- 1. Your credentials -------------------------------------------------
const char* SENSOR_ID  = "your-sensor-id-here";
const char* SECRET_KEY = "your-secret-key-here";

// --- 2. Your board -------------------------------------------------------
// WT32-ETH01. For the Olimex ESP32-POE use: address 0, power 12, ETH_CLOCK_GPIO17_OUT.
const int ETH_ADDRESS = 1;
const int ETH_MDC = 23;
const int ETH_MDIO = 18;
const int ETH_POWER = 16;
const eth_clock_mode_t ETH_CLOCK = ETH_CLOCK_GPIO0_IN;

// --- 3. Transport and client ---------------------------------------------
HydroNodeEsp32Ethernet eth;
HydroNodeClient hydro(eth, SENSOR_ID, SECRET_KEY);

const uint32_t INTERVAL_MS = 30000;

float readWaterTemperature() { return 19.8; }  // TODO: real measurement
float readWaterLevel()       { return 72.0; }  // TODO

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    // Start the port. DHCP runs in the background; the transport reports "up" once the cable is in
    // and an address arrived.
    ETH.begin(ETH_PHY_LAN8720, ETH_ADDRESS, ETH_MDC, ETH_MDIO, ETH_POWER, ETH_CLOCK);
    uint32_t start = millis();
    while (!eth.isUp() && millis() - start < 15000) delay(100);
    Serial.println(eth.isUp() ? "Ethernet up, IP " + ETH.localIP().toString() : String("No Ethernet yet, will keep trying"));
}

void loop() {
    HydroNodeValue values[] = {
        {"WATER_TEMPERATURE", readWaterTemperature()},
        {"WATER_LEVEL", readWaterLevel()},
    };
    // Without a cable sendValues() returns -1 (ERR_NETWORK_DOWN) right away, no timeout.
    int status = hydro.sendValues(values, 2);
    Serial.printf("HTTP %d\n", status);
    delay(INTERVAL_MS);
}
