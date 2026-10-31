#pragma once
// WiFi transport for the ESP32 and ESP8266: TLS with the bundled root certificates
// (HydroNodeCerts.h) and SNTP over WiFi. This is what the classic `HydroNode` class uses.
//
//   HydroNodeWiFi wifi;
//   HydroNodeClient hydro(wifi, SENSOR_ID, SECRET_KEY);
//
//   void setup() {
//       wifi.connect("ssid", "password");    // or WiFi.begin() / WiFiManager yourself
//   }
//
// Other boards with WiFi (Pico W, UNO R4 WiFi, MKR WiFi 1010) use HydroNodeClientTransport with
// their own TLS client, see examples/Board_MKR_WiFi1010.
#if defined(ESP32) || defined(ESP8266)

#include "HydroNodeTransport.h"
#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif
#include <WiFiUdp.h>

class HydroNodeWiFi : public HydroNodeTransport {
public:
    /**
     * Connects in station mode and waits until connected or `timeoutMs` passed. Optional: a link
     * you bring up yourself (WiFiManager, your own reconnect logic) works the same.
     */
    bool connect(const char* ssid, const char* password, uint32_t timeoutMs = 30000, Stream* debug = nullptr);

    const char* network() const override { return HydroNodeNet::WIFI; }
    bool isUp() override { return WiFi.status() == WL_CONNECTED; }
    Client& client() override { return tls_; }
    void beforeConnect(uint32_t unixTime) override;
    UDP* udp() override { return &udp_; }
    bool signalDbm(int& dbm) override;

#if defined(ESP8266)
    /** The TLS client, e.g. to change buffer sizes. Certificates are set by the library. */
    BearSSL::WiFiClientSecure& secureClient() { return tls_; }
#else
    WiFiClientSecure& secureClient() { return tls_; }
#endif

protected:
#if defined(ESP8266)
    BearSSL::WiFiClientSecure tls_;
#else
    WiFiClientSecure tls_;
#endif
    WiFiUDP udp_;
    bool ready_ = false;
};

#endif
