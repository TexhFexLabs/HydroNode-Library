/*
 * HydroNode — Board_AnyClient_Template
 *
 * A fill-in-the-blanks template for ANY board and ANY network that offers an Arduino `Client`.
 * Copy it, answer the four questions in block 2, done. Nothing in HydroNode depends on WiFi, a
 * modem or a chip: the library only needs these four answers.
 *
 *   Q1  Which Client speaks TLS to hydronode.tech:443 and checks the certificate?
 *   Q2  Is the network up right now?
 *   Q3  Where does the time come from? (the signature carries it, ±2 minutes are allowed)
 *   Q4  (optional) How strong is the signal?
 *
 * Typical answers:
 *
 *   Network                 Q1 TLS client                        Q3 time
 *   ---------------------   ----------------------------------   ------------------------------
 *   ESP32/ESP8266 WiFi      use HydroNodeWiFi instead            built in
 *   W5500 Ethernet          ESP_SSLClient over EthernetClient     net.setUdp(ethernetUdp)
 *   NINA / UNO R4 WiFi      WiFiSSLClient                         WiFi.getTime()
 *   Pico W                  WiFiClientSecure + setCACert()        net.setUdp(wifiUdp)
 *   Any TinyGSM modem       use HydroNodeCellular instead         built in (modem clock)
 *   MKR NB 1500             NBSSLClient                           nbAccess.getTime()
 *   GPS on board            (any of the above)                    hydro.setTimeProvider(gps)
 *   RTC chip (DS3231)       (any of the above)                    hydro.setTimeProvider(rtc)
 *
 * Boards: any 32-bit Arduino board with enough RAM for TLS (ESP32, ESP8266, RP2040, SAMD21/51,
 * nRF52, STM32, Renesas RA4M1). AVR boards (UNO R3, Mega) are too small.
 *
 * This template compiles as it is (with a placeholder client that never connects), so you can
 * check your toolchain before wiring anything.
 */
#include <HydroNodeClient.h>

// --- 1. Your credentials -------------------------------------------------
const char* SENSOR_ID  = "your-sensor-id-here";
const char* SECRET_KEY = "your-secret-key-here";

// --- 2. The four answers -------------------------------------------------

/**
 * Q1: replace this placeholder with your TLS client, e.g.
 *   WiFiSSLClient tls;                       (WiFiNINA, WiFiS3)
 *   ESP_SSLClient tls; + tls.setClient(&tcp) (any plain Client)
 */
class PlaceholderClient : public Client {
public:
    int connect(IPAddress, uint16_t) override { return 0; }
    int connect(const char*, uint16_t) override { return 0; }
    size_t write(uint8_t) override { return 0; }
    size_t write(const uint8_t*, size_t) override { return 0; }
    int available() override { return 0; }
    int read() override { return -1; }
    int read(uint8_t*, size_t) override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    void stop() override {}
    uint8_t connected() override { return 0; }
    operator bool() override { return false; }
};
PlaceholderClient tls;

// The network name HydroNode shows: WIFI, ETHERNET, LTE_M, NB_IOT, LTE, GSM or CELLULAR.
HydroNodeClientTransport net(tls, HydroNodeNet::WIFI);
HydroNodeClient hydro(net, SENSOR_ID, SECRET_KEY);

/** Q2: true while the network can carry a request. */
bool networkUp() {
    return false;  // e.g. WiFi.status() == WL_CONNECTED, Ethernet.linkStatus() == LinkON
}

/** Q3: Unix time in milliseconds, 0 while unknown. */
uint64_t networkTimeMs() {
    return 0;  // e.g. uint64_t(WiFi.getTime()) * 1000, or rtc.now().unixtime() * 1000ULL
}

/** Q4 (optional): signal strength in dBm; return false when your network has none. */
bool signalDbm(int& dbm) {
    (void)dbm;
    return false;  // e.g. dbm = WiFi.RSSI(); return true;
}

// --- 3. Your sensor ------------------------------------------------------
float readValue() { return 42.0; }  // TODO

const uint32_t INTERVAL_MS = 60000;

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);

    net.onIsUp(networkUp);
    net.onNetworkTime(networkTimeMs);
    net.onSignal(signalDbm);
    // Has your network a UDP socket? Then SNTP gives millisecond time: net.setUdp(udp);
    // Does your TLS client need the time to check certificates (BearSSL)? Then:
    //   net.onBeforeConnect([](uint32_t now) { tls.setX509Time(now); });

    // Bring your network up here (WiFi.begin(), Ethernet.begin(), modem.gprsConnect() ...).
}

void loop() {
    HydroNodeValue values[] = {{"TEMPERATURE", readValue()}};
    int status = hydro.sendValues(values, 1);

    // 202 = stored by HydroNode. -1 network down (Q2), -2 no time (Q3), -3 TLS failed (Q1).
    Serial.print("HTTP ");
    Serial.println(status);
    delay(INTERVAL_MS);
}
