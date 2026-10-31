#if defined(ESP32) || defined(ESP8266)

#include "HydroNodeWiFi.h"
#include "HydroNodeCerts.h"

bool HydroNodeWiFi::connect(const char* ssid, const char* password, uint32_t timeoutMs, Stream* debug) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    if (debug) debug->println("[HydroNode] WiFi: connecting to " + String(ssid));
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start >= timeoutMs) {
            if (debug) debug->println("[HydroNode] WiFi: connect timeout");
            return false;
        }
        delay(250);
    }
    if (debug) debug->println("[HydroNode] WiFi: connected, IP " + WiFi.localIP().toString());
    return true;
}

void HydroNodeWiFi::beforeConnect(uint32_t unixTime) {
#if defined(ESP8266)
    // BearSSL: parse the roots once, they stay valid for the lifetime of the sketch. ECDSA roots
    // only: the RSA-4096 ones would take ~6 KB of a heap the handshake needs in full. The session
    // lets the next connection resume instead of running the full handshake again (when the
    // server still knows it).
    static BearSSL::X509List trust(HYDRONODE_CA_BUNDLE_EC);
    static BearSSL::Session session;
    if (!ready_) {
        tls_.setTrustAnchors(&trust);
        tls_.setSession(&session);
        ready_ = true;
    }
    tls_.setX509Time(unixTime);  // BearSSL checks certificate dates against this
#else
    (void)unixTime;
    if (!ready_) {
        tls_.setCACert(HYDRONODE_CA_BUNDLE);
        ready_ = true;
    }
#endif
}

bool HydroNodeWiFi::signalDbm(int& dbm) {
    if (WiFi.status() != WL_CONNECTED) return false;
    dbm = WiFi.RSSI();
    return true;
}

#endif
