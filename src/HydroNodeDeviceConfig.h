#pragma once

// Plain C++ without Arduino headers, like HydroNodeBatteryGuard: tested natively in extras/test.
#include <stddef.h>
#include <stdint.h>

/**
 * The device settings a board runs, reported to HydroNode as X-Device-Config:
 *
 *   v=1 rev=7 int=300 save=3500 rec=3300 sby=3200 res=3600 src=bat gauge=max17048 cells=1 mah=2000 pwr=normal
 *
 * Thresholds are pack millivolts (volts per cell x cells). Zero or nullptr leaves a key out.
 */
struct HydroNodeDeviceConfig {
    uint32_t intervalSeconds = 0;     // send interval
    uint16_t saveMv = 0;              // SAVE below
    uint16_t recoveryMv = 0;          // radio off below
    uint16_t standbyMv = 0;           // standby below
    uint16_t resumeMv = 0;            // resume at
    const char* source = nullptr;     // "usb", "bat" or "solar"
    const char* gauge = nullptr;      // e.g. "max17048", "adc", "ina226"
    uint8_t cells = 0;
    uint32_t capacityMah = 0;
    const char* powerState = nullptr; // "normal", "save", "recovery" or "standby"
    uint16_t revision = 0;            // the settings revision these values came with, 0 = own defaults
};

/**
 * New settings from HydroNode (answer key "settings"). Values the answer leaves out keep the
 * current ones; capacityMah is 0 when neither the answer nor the current config has one.
 */
struct HydroNodeSettings {
    uint16_t revision;
    uint32_t intervalSeconds;
    uint16_t saveMv;
    uint16_t recoveryMv;
    uint16_t standbyMv;
    uint16_t resumeMv;
    uint32_t capacityMah;
};

namespace hydronode {

/** Longest X-Device-Config value; the backend drops longer ones. */
constexpr size_t DEVICE_CONFIG_MAX = 256;

/** Send interval limits of an HTTP device, in seconds. */
constexpr uint32_t INTERVAL_MIN_S = 10;
constexpr uint32_t INTERVAL_MAX_S = 604800;

/**
 * Writes the X-Device-Config value into `out` (NUL-terminated, at most DEVICE_CONFIG_MAX
 * characters; a key that does not fit is left out whole). `rejectedRevision` >= 0 adds
 * rej=<rev>, `error` adds err=<reason>. Text values keep only a-z, 0-9 and _. Returns the length.
 */
size_t formatDeviceConfig(char* out, size_t size, const HydroNodeDeviceConfig& config, bool acceptsSettings,
                          int32_t rejectedRevision = -1, const char* error = nullptr);

/**
 * Checks settings before they reach the sketch: interval 10 to 604800 s and the gaps between
 * the four thresholds (the per cell range needs the chemistry, HydroNode checked it already).
 * Returns true when they fit.
 */
bool settingsValid(const HydroNodeSettings& settings);

}  // namespace hydronode
