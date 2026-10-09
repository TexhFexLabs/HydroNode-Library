#include "HydroNodeBatteryGuard.h"

HydroNodeBatteryGuard::HydroNodeBatteryGuard() : HydroNodeBatteryGuard(defaults(LIPO, 1)) {}

HydroNodeBatteryGuard::HydroNodeBatteryGuard(const HydroNodeThresholds& thresholds) : thresholds_(thresholds) {
    reset();
}

void HydroNodeBatteryGuard::setThresholds(const HydroNodeThresholds& thresholds) {
    thresholds_ = thresholds;
}

void HydroNodeBatteryGuard::reset() {
    memory_ = Memory{MAGIC, NORMAL, 0, 0, 0, 0, 0};
    changed_ = false;
}

bool HydroNodeBatteryGuard::restore(const Memory& memory) {
    if (memory.magic != MAGIC || memory.state > STANDBY || memory.started > 1) {
        reset();
        return false;
    }
    memory_ = memory;
    changed_ = false;
    return true;
}

HydroNodeBatteryGuard::State HydroNodeBatteryGuard::update(uint16_t millivolts, bool valid, uint32_t nowSeconds) {
    State before = state();
    if (!memory_.started) {
        // First reading, like a station boot: run only at resume or more.
        memory_.started = 1;
        setState(valid && millivolts >= thresholds_.resumeMv ? NORMAL : RECOVERY);
    }
    step(millivolts, valid, nowSeconds);
    changed_ = state() != before;
    return state();
}

void HydroNodeBatteryGuard::step(uint16_t mv, bool valid, uint32_t now) {
    const HydroNodeThresholds& t = thresholds_;
    if (state() == STANDBY) {
        // Hourly wake: only a valid reading at resume or more starts the way back.
        if (!valid || mv < t.resumeMv) return;
        setState(RECOVERY);
        memory_.invalid = 0;
        memory_.low = 0;
        memory_.stable = 1;
        memory_.stableSince = now;
        return;
    }
    if (!valid) {
        // A missing reading is no undervoltage: it never leads to STANDBY.
        memory_.low = 0;
        memory_.stable = 0;
        if (memory_.invalid < INVALID_LIMIT) memory_.invalid++;
        if (memory_.invalid >= INVALID_LIMIT) setState(RECOVERY);
        return;
    }
    memory_.invalid = 0;
    if (mv < t.recoveryMv) {
        setState(RECOVERY);
        memory_.stable = 0;
        if (mv < t.standbyMv) {
            if (memory_.low < STANDBY_READINGS) memory_.low++;
        } else {
            memory_.low = 0;
        }
        if (memory_.low >= STANDBY_READINGS) setState(STANDBY);
        return;
    }
    memory_.low = 0;
    uint32_t saveExit = static_cast<uint32_t>(t.saveMv) + SAVE_EXIT_MV;
    if (state() == RECOVERY) {
        if (mv < t.resumeMv) {
            memory_.stable = 0;
            return;
        }
        if (memory_.stable == 0) memory_.stableSince = now;
        if (memory_.stable < RESUME_READINGS) memory_.stable++;
        if (memory_.stable >= RESUME_READINGS && static_cast<uint32_t>(now - memory_.stableSince) >= RESUME_STABLE_S) {
            setState(mv >= saveExit ? NORMAL : SAVE);
            memory_.stable = 0;
        }
    } else if (mv < t.saveMv) {
        setState(SAVE);
    } else if (mv >= saveExit) {
        setState(NORMAL);
    }
}

uint8_t HydroNodeBatteryGuard::intervalFactor() const {
    switch (state()) {
        case NORMAL: return 1;
        case SAVE:   return 2;
        default:     return 0;
    }
}

uint32_t HydroNodeBatteryGuard::sleepSeconds(uint32_t intervalSeconds) const {
    switch (state()) {
        case NORMAL:   return intervalSeconds;
        case SAVE:     return intervalSeconds > UINT32_MAX / 2 ? UINT32_MAX : intervalSeconds * 2;
        case RECOVERY: return RECOVERY_CHECK_S;
        default:       return STANDBY_WAKE_S;
    }
}

const char* HydroNodeBatteryGuard::stateName(State state) {
    switch (state) {
        case NORMAL:   return "normal";
        case SAVE:     return "save";
        case RECOVERY: return "recovery";
        default:       return "standby";
    }
}

HydroNodeThresholds HydroNodeBatteryGuard::defaults(Chemistry chemistry, uint8_t cells) {
    uint16_t n = cells == 0 ? 1 : cells;
    switch (chemistry) {
        case LI_ION:
            return {static_cast<uint16_t>(3450 * n), static_cast<uint16_t>(3200 * n), static_cast<uint16_t>(3000 * n),
                    static_cast<uint16_t>(3500 * n)};
        case LIFEPO4:
            return {static_cast<uint16_t>(3100 * n), static_cast<uint16_t>(3000 * n), static_cast<uint16_t>(2800 * n),
                    static_cast<uint16_t>(3200 * n)};
        default:
            return {static_cast<uint16_t>(3500 * n), static_cast<uint16_t>(3300 * n), static_cast<uint16_t>(3200 * n),
                    static_cast<uint16_t>(3600 * n)};
    }
}

uint8_t HydroNodeBatteryGuard::validate(const HydroNodeThresholds& t, uint8_t cells, Chemistry chemistry) {
    if (chemistry == LIFEPO4) return validate(t, cells, 2500, 3400);
    return validate(t, cells, 2800, 4100);
}

uint8_t HydroNodeBatteryGuard::validate(const HydroNodeThresholds& t, uint8_t cells, uint16_t minPerCellMv,
                                        uint16_t maxPerCellMv) {
    uint32_t n = cells == 0 ? 1 : cells;
    uint32_t save = t.saveMv, recovery = t.recoveryMv, standby = t.standbyMv, resume = t.resumeMv;
    uint8_t broken = 0;
    if (minPerCellMv != 0 || maxPerCellMv != 0) {
        uint32_t min = minPerCellMv * n, max = maxPerCellMv * n;
        if (save < min || save > max) broken |= FIELD_SAVE;
        if (recovery < min || recovery > max) broken |= FIELD_RECOVERY;
        if (standby < min || standby > max) broken |= FIELD_STANDBY;
        if (resume < min || resume > max) broken |= FIELD_RESUME;
    }
    // Gaps are pack mV, whatever the cell count.
    if (recovery + GAP_RECOVERY_MV > save) broken |= FIELD_RECOVERY;
    if (standby + GAP_STANDBY_MV > recovery) broken |= FIELD_STANDBY;
    if (recovery + GAP_RESUME_MV > resume) broken |= FIELD_RESUME;
    if (resume > save + RESUME_ABOVE_SAVE_MV) broken |= FIELD_RESUME;
    return broken;
}
