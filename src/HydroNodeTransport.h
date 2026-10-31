#pragma once
// How a HydroNodeClient reaches the internet.
//
// The signature (HMAC-SHA256) does not care how the bytes travel: WiFi, Ethernet, LTE-M, NB-IoT,
// a Cat-1 modem or anything else that offers an Arduino `Client`. A transport answers four
// questions for the library:
//
//   1. Is the link up?                         isUp()
//   2. Which encrypted Client carries HTTPS?   client()          (must verify the certificate)
//   3. Where does the time come from?          udp() for SNTP, or networkTimeMs() (modem clock)
//   4. What do I report to HydroNode?          network() ("wifi", "eth", "lte-m", ...), signalDbm()
//
// Ready-made transports:
//
//   HydroNodeWiFi            ESP32 / ESP8266 WiFi (what `HydroNode` uses)       HydroNodeWiFi.h
//   HydroNodeEsp32Ethernet   ESP32 with built-in Ethernet (WT32-ETH01, POE)     HydroNodeEsp32Ethernet.h
//   HydroNodeCellular<M>     any TinyGSM modem (SIM7080G, SIM7000, A7670, ...)  HydroNodeCellular.h
//   HydroNodeClientTransport any Client you already have (W5500 + TLS, WiFiNINA,
//                            UNO R4 WiFi, MKR NB 1500, Pico W, ...)             this file
//   HydroNodeFailover        tries a first transport, then a second one         this file
//
// Only the headers you include are compiled: a WiFi sketch never pulls in modem code.
#include <Arduino.h>
#include <Client.h>
#include <Udp.h>
#include <functional>

/** Values for network() and the net= field HydroNode shows in the fleet view. */
namespace HydroNodeNet {
static constexpr const char* WIFI = "wifi";
static constexpr const char* ETHERNET = "eth";
static constexpr const char* LTE_M = "lte-m";
static constexpr const char* NB_IOT = "nb-iot";
static constexpr const char* LTE = "lte";  // Cat-1, Cat-4 and faster
static constexpr const char* GSM = "2g";
static constexpr const char* CELLULAR = "cellular";  // a modem that does not say which
}  // namespace HydroNodeNet

class HydroNodeTransport {
public:
    virtual ~HydroNodeTransport() {}

    /** Short network name, one of HydroNodeNet (sent as net= in X-Device-Status). */
    virtual const char* network() const = 0;

    /** True while the link can carry a request (WiFi joined, cable plugged, modem attached). */
    virtual bool isUp() = 0;

    /**
     * The Client the HTTPS requests run on. connect(host, 443) must give an encrypted connection
     * that checked the server certificate (WiFiClientSecure, ESP_SSLClient, a modem's own TLS ...).
     * The library calls this before every request, so a transport may hand out different clients
     * over time (see HydroNodeFailover).
     */
    virtual Client& client() = 0;

    /**
     * Called right before a connection with the valid Unix time, e.g. to load the root
     * certificates once or to give a BearSSL client the time it checks certificates against.
     */
    virtual void beforeConnect(uint32_t unixTime) { (void)unixTime; }

    /** A UDP socket for the built-in SNTP time sync, nullptr when the link has none (most modems). */
    virtual UDP* udp() { return nullptr; }

    /** Unix time in milliseconds from the network itself (modem clock, NITZ), 0 when unknown. */
    virtual uint64_t networkTimeMs() { return 0; }

    /** Signal strength in dBm; false when not known. */
    virtual bool signalDbm(int& dbm) {
        (void)dbm;
        return false;
    }
};

/**
 * Wraps any Arduino Client that already speaks TLS. Everything else is optional and set with
 * the on...() callbacks:
 *
 *   EthernetClient tcp;
 *   ESP_SSLClient tls;                         // TLS on the microcontroller
 *   EthernetUDP udp;
 *   HydroNodeClientTransport net(tls, HydroNodeNet::ETHERNET);
 *
 *   void setup() {
 *       tls.setClient(&tcp);
 *       tls.setCACert(HYDRONODE_CA_BUNDLE);
 *       net.setUdp(udp);                                           // time by SNTP
 *       net.onIsUp([] { return Ethernet.linkStatus() == LinkON; });
 *       net.onBeforeConnect([](uint32_t now) { tls.setX509Time(now); });
 *   }
 */
class HydroNodeClientTransport : public HydroNodeTransport {
public:
    explicit HydroNodeClientTransport(Client& secureClient, const char* network = HydroNodeNet::CELLULAR)
        : client_(&secureClient), network_(network) {}

    /** Use this UDP socket for SNTP. */
    void setUdp(UDP& udp) { udp_ = &udp; }

    /** Without a callback the link counts as up. */
    void onIsUp(std::function<bool()> fn) { isUp_ = fn; }

    /** Unix time in ms from your own source (modem, GPS, RTC chip); return 0 when unknown. */
    void onNetworkTime(std::function<uint64_t()> fn) { networkTime_ = fn; }

    /** Signal strength in dBm; return false when unknown. */
    void onSignal(std::function<bool(int&)> fn) { signal_ = fn; }

    /** Runs before every connection with the Unix time (load certificates, set X509 time). */
    void onBeforeConnect(std::function<void(uint32_t)> fn) { beforeConnect_ = fn; }

    /** Swap the Client, e.g. after re-creating it. */
    void setClient(Client& secureClient) { client_ = &secureClient; }

    void setNetwork(const char* network) { network_ = network; }

    const char* network() const override { return network_; }
    bool isUp() override { return isUp_ ? isUp_() : true; }
    Client& client() override { return *client_; }
    void beforeConnect(uint32_t unixTime) override {
        if (beforeConnect_) beforeConnect_(unixTime);
    }
    UDP* udp() override { return udp_; }
    uint64_t networkTimeMs() override { return networkTime_ ? networkTime_() : 0; }
    bool signalDbm(int& dbm) override { return signal_ ? signal_(dbm) : false; }

private:
    Client* client_;
    const char* network_;
    UDP* udp_ = nullptr;
    std::function<bool()> isUp_;
    std::function<uint64_t()> networkTime_;
    std::function<bool(int&)> signal_;
    std::function<void(uint32_t)> beforeConnect_;
};

/**
 * Two transports, the first one preferred: WiFi at home, LTE when WiFi is gone. Each request
 * goes over the first transport that is up; when none is, the primary answers (and the request
 * fails with ERR_NETWORK_DOWN). Bring the links up yourself (WiFi.begin(), modem.gprsConnect()).
 *
 *   HydroNodeWiFi wifi;
 *   HydroNodeCellular<TinyGsm> lte(modem, tls, HydroNodeNet::LTE_M);
 *   HydroNodeFailover net(wifi, lte);
 *   HydroNodeClient hydro(net, SENSOR_ID, SECRET_KEY);
 */
class HydroNodeFailover : public HydroNodeTransport {
public:
    HydroNodeFailover(HydroNodeTransport& primary, HydroNodeTransport& secondary)
        : primary_(primary), secondary_(secondary) {}

    /** The transport the next request uses. */
    HydroNodeTransport& active() { return primary_.isUp() || !secondary_.isUp() ? primary_ : secondary_; }

    const char* network() const override {
        HydroNodeFailover* self = const_cast<HydroNodeFailover*>(this);
        return self->active().network();
    }
    bool isUp() override { return primary_.isUp() || secondary_.isUp(); }
    Client& client() override { return active().client(); }
    void beforeConnect(uint32_t unixTime) override { active().beforeConnect(unixTime); }
    UDP* udp() override { return active().udp(); }
    uint64_t networkTimeMs() override { return active().networkTimeMs(); }
    bool signalDbm(int& dbm) override { return active().signalDbm(dbm); }

private:
    HydroNodeTransport& primary_;
    HydroNodeTransport& secondary_;
};
