#pragma once
// Ethernet built into the ESP32 (RMII PHY like the LAN8720 on the WT32-ETH01 or Olimex
// ESP32-POE). The ESP32 runs the same network stack for Ethernet and WiFi, so this transport
// reuses the WiFi one (TLS with the bundled roots, SNTP) and only asks the Ethernet port whether
// it is up. Start the port yourself with ETH.begin(...), see examples/Ethernet_ESP32.
//
// A W5500/W5100 module on SPI is no built-in Ethernet: use HydroNodeClientTransport with an
// EthernetClient and ESP_SSLClient (examples/Ethernet_W5500).
#if defined(ESP32)

#include <ETH.h>
#include "HydroNodeWiFi.h"

class HydroNodeEsp32Ethernet : public HydroNodeWiFi {
public:
    const char* network() const override { return HydroNodeNet::ETHERNET; }

    bool isUp() override { return ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0); }

    /** Ethernet has no signal strength. */
    bool signalDbm(int& dbm) override {
        (void)dbm;
        return false;
    }
};

#endif
