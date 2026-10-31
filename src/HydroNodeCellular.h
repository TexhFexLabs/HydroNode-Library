#pragma once
// Cellular transport for any modem TinyGSM drives: LTE-M and NB-IoT (SIM7080G, SIM7000, BG95,
// SARA-R4), LTE Cat-1/Cat-4 (A7670, SIM7600) or 2G (SIM800).
//
// Include the TinyGSM modem first, this header after it:
//
//   #define TINY_GSM_MODEM_SIM7080       // your modem
//   #include <TinyGsmClient.h>
//   #include <ESP_SSLClient.h>            // TLS on the microcontroller (any modem)
//   #include <HydroNodeClient.h>
//   #include <HydroNodeCellular.h>
//
//   TinyGsm modem(SerialAT);
//   TinyGsmClient tcp(modem);
//   ESP_SSLClient tls;
//   HydroNodeCellular<TinyGsm> lte(modem, tls, HydroNodeNet::LTE_M);
//   HydroNodeClient hydro(lte, SENSOR_ID, SECRET_KEY);
//
// Two ways to get TLS, pick per modem:
//
//   TLS on the microcontroller   TinyGsmClient + ESP_SSLClient (MIT). Works with every modem, the
//                                root certificates stay in your sketch. Needs ~40 KB RAM: ESP32,
//                                RP2040, SAMD51, nRF52.
//   TLS in the modem             TinyGsmClientSecure (SIM7000SSL, SIM7080, A7670, SIM7600, BG96).
//                                Little RAM, but the modem has to trust the root certificate
//                                (upload it once, see examples/Cellular_ModemTLS).
//
// Time: the library asks the modem (NTP through the modem when it can, otherwise the network time
// the operator sends). Signal: CSQ turned into dBm.
//
// TinyGSM is LGPL-3.0. This header only calls it; your sketch decides to use it.
#include "HydroNodeTransport.h"

template <typename Modem>
class HydroNodeCellular : public HydroNodeTransport {
public:
    HydroNodeCellular(Modem& modem, Client& secureClient, const char* network = HydroNodeNet::CELLULAR)
        : modem_(modem), client_(&secureClient), network_(network) {}

    /** NTP server the modem asks (when it supports NTP). nullptr: only the operator's clock. */
    void setTimeServer(const char* server) { timeServer_ = server; }

    /** Runs before every connection with the Unix time, e.g. tls.setX509Time(now). */
    void onBeforeConnect(std::function<void(uint32_t)> fn) { beforeConnect_ = fn; }

    /** The modem, e.g. to power it down after sending. */
    Modem& modem() { return modem_; }

    const char* network() const override { return network_; }

    bool isUp() override { return modem_.isGprsConnected(); }

    Client& client() override { return *client_; }

    void beforeConnect(uint32_t unixTime) override {
        if (beforeConnect_) beforeConnect_(unixTime);
    }

    uint64_t networkTimeMs() override {
#if defined(TINY_GSM_MODEM_HAS_NTP)
        // 1 = synchronized. A failure is fine: the operator's clock may still be right.
        if (timeServer_) modem_.NTPServerSync(timeServer_, 0);
#endif
#if defined(TINY_GSM_MODEM_HAS_TIME)
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        float zone = 0;
        if (!modem_.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &zone)) return 0;
        if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) return 0;  // modem clock not set
        int64_t days = daysFromCivil(year, month, day);
        int64_t local = days * 86400 + hour * 3600 + minute * 60 + second;
        // +CCLK gives local time and the zone in hours: UTC = local - zone.
        int64_t utc = local - (int64_t)(zone * 3600.0f);
        return utc > 0 ? uint64_t(utc) * 1000ULL : 0;
#else
        return 0;
#endif
    }

    bool signalDbm(int& dbm) override {
        int csq = modem_.getSignalQuality();
        if (csq <= 0 || csq >= 99) return false;
        dbm = -113 + 2 * csq;  // 3GPP TS 27.007: 0 = -113 dBm, 31 = -51 dBm
        return true;
    }

    /** Days since 1970-01-01 for a date of the Gregorian calendar (Howard Hinnant's algorithm). */
    static int64_t daysFromCivil(int y, int m, int d) {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const int64_t yoe = y - era * 400;
        const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

private:
    Modem& modem_;
    Client* client_;
    const char* network_;
    const char* timeServer_ = "pool.ntp.org";
    std::function<void(uint32_t)> beforeConnect_;
};
