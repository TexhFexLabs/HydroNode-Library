#pragma once

// Plain C++ without Arduino headers: the same file builds on the board and in the native tests
// (extras/test). The universal HydroNode firmware uses this class too.
#include <stdint.h>

/** Four battery thresholds in pack millivolts (volts per cell x cells). */
struct HydroNodeThresholds {
    uint16_t saveMv;
    uint16_t recoveryMv;
    uint16_t standbyMv;
    uint16_t resumeMv;
};

/**
 * HydroNodeBatteryGuard: the battery state machine of the HydroNode station, for your own sketch.
 * Feed it one reading per round (or every 60 s while radio is off) and act on the state:
 *
 *   NORMAL    send as usual
 *   SAVE      below Save: send half as often (factor 2), skip costly sensors
 *   RECOVERY  below Recovery or 3 invalid readings: WiFi off, outputs off, only read the battery
 *   STANDBY   in Recovery, 2 valid readings below Standby: deepest sleep, wake once an hour
 *
 *   NORMAL -> SAVE        below save, back at save + 150 mV
 *   -> RECOVERY           below recovery, or 3 invalid readings in a row
 *   RECOVERY -> STANDBY   2 valid readings below standby in a row (an invalid one never counts)
 *   RECOVERY -> running   2 valid readings at resume or more, at least 60 s apart
 *   STANDBY -> RECOVERY   a valid reading at resume or more (then the 2 readings above)
 *
 * The very first reading decides like a station boot: at resume or more the device runs, below
 * it waits in Recovery. That keeps a nearly empty battery from boot looping when its voltage
 * recovers without load.
 *
 * The guard does not sleep, switch WiFi or touch pins; the sketch does. Its memory() is a plain
 * struct to keep in RTC memory across deep sleep (RTC_DATA_ATTR on the ESP32).
 */
class HydroNodeBatteryGuard {
public:
    enum State : uint8_t { NORMAL = 0, SAVE = 1, RECOVERY = 2, STANDBY = 3 };
    enum Chemistry : uint8_t { LIPO = 0, LI_ION = 1, LIFEPO4 = 2 };

    // Rules shared with the backend, web, apps, firmware and station (Power-Sync).
    static constexpr uint16_t SAVE_EXIT_MV = 150;           // SAVE ends at save + 150
    static constexpr uint16_t GAP_STANDBY_MV = 50;          // standby + 50 <= recovery
    static constexpr uint16_t GAP_RECOVERY_MV = 50;         // recovery + 50 <= save
    static constexpr uint16_t GAP_RESUME_MV = 100;          // recovery + 100 <= resume
    static constexpr uint16_t RESUME_ABOVE_SAVE_MV = 400;   // resume <= save + 400
    static constexpr uint32_t RESUME_STABLE_S = 60;
    static constexpr uint8_t RESUME_READINGS = 2;
    static constexpr uint8_t STANDBY_READINGS = 2;
    static constexpr uint8_t INVALID_LIMIT = 3;
    static constexpr uint32_t RECOVERY_CHECK_S = 60;        // battery check while radio is off
    static constexpr uint32_t STANDBY_WAKE_S = 3600;        // hourly wake in standby

    // Bits of validate(): the field a broken rule belongs to.
    static constexpr uint8_t FIELD_SAVE = 1;
    static constexpr uint8_t FIELD_RECOVERY = 2;
    static constexpr uint8_t FIELD_STANDBY = 4;
    static constexpr uint8_t FIELD_RESUME = 8;

    /** Everything the guard remembers. Keep it in RTC memory to carry it through deep sleep. */
    struct Memory {
        uint32_t magic;
        uint8_t state;
        uint8_t invalid;      // invalid readings in a row
        uint8_t low;          // valid readings below standby in a row
        uint8_t stable;       // valid readings at resume or more in a row
        uint32_t stableSince;
        uint8_t started;      // 1 after the first reading
    };

    /** Starts with the LiPo values of one cell until setThresholds(). */
    HydroNodeBatteryGuard();
    explicit HydroNodeBatteryGuard(const HydroNodeThresholds& thresholds);

    /** New thresholds take effect at the next reading; the state stays until then. */
    void setThresholds(const HydroNodeThresholds& thresholds);
    const HydroNodeThresholds& thresholds() const { return thresholds_; }

    /**
     * One battery reading: pack millivolts, whether the reading worked, and a clock in seconds
     * that keeps counting through sleep (only differences matter). Returns the new state.
     */
    State update(uint16_t millivolts, bool valid, uint32_t nowSeconds);

    /** NORMAL before the first reading. */
    State state() const { return static_cast<State>(memory_.state); }

    /** The last update() changed the state, e.g. to send one last round before Recovery. */
    bool changed() const { return changed_; }

    /** Radio and outputs may run: NORMAL or SAVE. */
    bool radioAllowed() const { return state() == NORMAL || state() == SAVE; }

    /** Interval factor: 1 in NORMAL, 2 in SAVE, 0 while radio is off. */
    uint8_t intervalFactor() const;

    /** How long to sleep: the interval in NORMAL, twice it in SAVE, 60 s in RECOVERY, 1 h in STANDBY. */
    uint32_t sleepSeconds(uint32_t intervalSeconds) const;

    /** normal, save, recovery or standby: the pwr= value HydroNode reads. */
    const char* stateName() const { return stateName(state()); }
    static const char* stateName(State state);

    Memory memory() const { return memory_; }

    /** Restores memory() from RTC memory. Returns false (and starts fresh) for garbage. */
    bool restore(const Memory& memory);

    /** Forgets every reading; the next one decides like a boot. */
    void reset();

    /** Preset of a chemistry, per cell times cells: LiPo 3.50/3.30/3.20/3.60 V. */
    static HydroNodeThresholds defaults(Chemistry chemistry, uint8_t cells = 1);

    /**
     * Checks the rules: gaps between the thresholds and the per cell range of the chemistry
     * (LiPo and Li-ion 2.80 to 4.10 V, LiFePO4 2.50 to 3.40 V). Returns 0 when everything fits,
     * otherwise FIELD_* bits of the values to fix.
     */
    static uint8_t validate(const HydroNodeThresholds& thresholds, uint8_t cells, Chemistry chemistry);

    /** Same with your own per cell range in mV, e.g. for another chemistry. 0/0 checks the gaps only. */
    static uint8_t validate(const HydroNodeThresholds& thresholds, uint8_t cells, uint16_t minPerCellMv,
                            uint16_t maxPerCellMv);

private:
    static constexpr uint32_t MAGIC = 0x484E4247;  // "HNBG"

    HydroNodeThresholds thresholds_;
    Memory memory_;
    bool changed_ = false;

    void step(uint16_t millivolts, bool valid, uint32_t nowSeconds);
    void setState(State state) { memory_.state = static_cast<uint8_t>(state); }
};
