#pragma once
// HydroNode: the classic entry point, unchanged since 1.0.
//
//   #include <HydroNode.h>
//   HydroNode hydro("sensor-id", "secret-key");
//
// On the ESP32 and ESP8266 this is HydroNodeClient with the WiFi transport built in, so every
// sketch written before 1.9.0 compiles and behaves as before. For Ethernet, LTE-M, NB-IoT or other
// boards use HydroNodeClient with a transport (see HydroNodeClient.h and HydroNodeTransport.h).
#include "HydroNodeClient.h"
#include "HydroNodeCerts.h"

#if defined(ESP32) || defined(ESP8266)

#include "HydroNodeWiFi.h"

namespace hydronode {
namespace detail {
/** Holds the WiFi transport so it exists before the HydroNodeClient base that uses it. */
struct WiFiTransportHolder {
    HydroNodeWiFi wifiTransport;
};
}  // namespace detail
}  // namespace hydronode

/**
 * HydroNode — Arduino client for the HydroNode IoT backend over WiFi (ESP32, ESP8266).
 *
 * Sends HMAC-SHA256-signed sensor values over HTTPS and dispatches
 * backend commands (JSON response keys) to registered callbacks.
 *
 * Typical usage:
 *
 *   HydroNode hydro("sensor-id", "secret-key");
 *
 *   void setup() {
 *       hydro.connectWiFi("ssid", "password");   // or manage WiFi yourself
 *       hydro.begin();
 *   }
 *
 *   void loop() {
 *       hydro.sendValue("TEMPERATURE", 21.5);
 *       delay(10000);   // backend accepts one value per type every 10 s
 *   }
 *
 * Several values of one reading go out in one request:
 *
 *   HydroNodeValue v[] = {{"TEMPERATURE", t}, {"HUMIDITY", h}};
 *   hydro.sendValues(v, 2);
 */
class HydroNode : private hydronode::detail::WiFiTransportHolder, public HydroNodeClient {
public:
    HydroNode(const char* sensorId, const char* secretKey, const char* host = "hydronode.tech",
              const char* path = "/api/webhook/sensor-value")
        : WiFiTransportHolder(), HydroNodeClient(wifiTransport, sensorId, secretKey, host, path) {}

    /**
     * Convenience WiFi setup: connects in station mode and blocks until
     * connected or the timeout expires. Entirely optional — if you manage
     * WiFi yourself (WiFiManager, ESP-IDF, custom reconnect logic), simply
     * don't call it.
     *
     * @return true if connected, false on timeout.
     */
    bool connectWiFi(const char* ssid, const char* password, uint32_t timeoutMs = 30000) {
        return wifiTransport.connect(ssid, password, timeoutMs, debugStream_);
    }

    /** Enable debug logging, e.g. hydro.setDebug(Serial). */
    void setDebug(Stream& stream) {
        debugStream_ = &stream;
        HydroNodeClient::setDebug(stream);
    }

    /** The WiFi transport, e.g. wifi().secureClient() to tune the TLS client. */
    HydroNodeWiFi& wifi() { return wifiTransport; }

private:
    Stream* debugStream_ = nullptr;
};

#endif
