/*
 * HydroNode — Ethernet_W5500
 *
 * Ethernet with a WIZnet W5500 (or W5100/W5200) module on SPI. Works on every board the
 * Ethernet library supports and that has the RAM for TLS: ESP32, ESP8266, RP2040 (Pico),
 * SAMD21/51 (Feather M0/M4), nRF52, STM32.
 *
 * The W5500 only speaks plain TCP, so TLS runs on the microcontroller: ESP_SSLClient wraps the
 * EthernetClient and checks the HydroNode certificate against the bundled root certificates.
 * This is the general pattern for ANY Arduino Client without TLS:
 *
 *   plain Client (EthernetClient)  ->  ESP_SSLClient (TLS)  ->  HydroNodeClientTransport  ->  HydroNodeClient
 *
 * Wiring (W5500 module, 3.3 V):  MOSI, MISO, SCK to the board's SPI pins, CS to W5500_CS below,
 *                                VCC 3.3 V, GND. Reset may stay unconnected.
 *
 * Libraries (Arduino Library Manager):
 *   HydroNode-Library, Ethernet (by Arduino), ESP_SSLClient (by Mobizt, MIT)
 *
 * Change: credentials, W5500_CS, MAC (any locally administered address, unique in your network).
 *
 * Time: SNTP over EthernetUDP (the transport's setUdp()).
 */
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <ESP_SSLClient.h>
#include <HydroNodeClient.h>
#include <HydroNodeCerts.h>

// --- 1. Your credentials -------------------------------------------------
const char* SENSOR_ID  = "your-sensor-id-here";
const char* SECRET_KEY = "your-secret-key-here";

// --- 2. Your hardware ----------------------------------------------------
const int W5500_CS = 5;                                  // chip select pin
byte MAC[] = {0x02, 0x48, 0x4E, 0x00, 0x00, 0x01};       // 02:... = locally administered

// --- 3. Network objects --------------------------------------------------
EthernetClient tcp;     // plain TCP on the W5500
ESP_SSLClient tls;      // TLS on top of it, on the microcontroller
EthernetUDP udp;        // time sync (SNTP)

HydroNodeClientTransport net(tls, HydroNodeNet::ETHERNET);
HydroNodeClient hydro(net, SENSOR_ID, SECRET_KEY);

const uint32_t INTERVAL_MS = 60000;

float readSoilMoisture() { return 41.0; }  // TODO: real measurement

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    Ethernet.init(W5500_CS);
    if (Ethernet.begin(MAC) == 0) Serial.println("DHCP failed, check the cable");

    // TLS: trust the HydroNode root certificates, run over the plain TCP client.
    tls.setClient(&tcp);
    tls.setCACert(HYDRONODE_CA_BUNDLE);
    tls.setBufferSizes(4096, 1024);   // receive / transmit; HydroNode answers stay below 4 KB
    tls.setSessionTimeout(120);       // keep the TLS session between requests

    // Tell the transport the rest:
    net.setUdp(udp);                                                   // time by SNTP
    net.onIsUp([] { return Ethernet.linkStatus() != LinkOFF; });       // cable plugged?
    net.onBeforeConnect([](uint32_t now) { tls.setX509Time(now); });  // BearSSL checks dates
}

void loop() {
    Ethernet.maintain();  // renews the DHCP lease

    HydroNodeValue values[] = {{"SOIL_MOISTURE", readSoilMoisture()}};
    int status = hydro.sendValues(values, 1);
    Serial.print("HTTP ");
    Serial.println(status);
    delay(INTERVAL_MS);
}
