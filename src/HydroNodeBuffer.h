#pragma once
// HydroNodeBuffer keeps measurement rounds until they reach HydroNode.
//
// Two reasons to use it:
//
//   Never lose a value      The network is gone for a while (WiFi router restarts, no LTE
//                           coverage in the basement). Rounds that could not be sent wait here
//                           and go out with the next successful request.
//   Send less often         A cellular board measures every 5 minutes but sends once an hour:
//                           one connection for 12 rounds costs a fraction of 12 connections.
//
// The buffer lives in memory YOU give it, so you decide where it survives:
//
//   static uint8_t mem[2048];                  plain RAM: survives nothing, simplest
//   RTC_DATA_ATTR static uint8_t mem[4096];    ESP32 RTC memory: survives deep sleep
//   + LittleFS / Preferences / EEPROM          survives power loss: save data()/size() yourself
//                                              after a change, load it before begin()
//
//   HydroNodeBuffer buffer(mem, sizeof(mem));
//   buffer.begin();                           // keeps rounds from before the sleep if they check out
//   hydro.attachBuffer(buffer);
//
// Stored rounds go to POST /api/webhook/sensor-rounds (HydroNode server 2026-10 and newer): each
// round keeps the time it was measured, up to 7 days back, and is stored once even when a
// request is sent twice.
//
// Layout: a 16-byte header (magic, used bytes, round count, dropped rounds, checksum), then the
// rounds one after another, oldest first. One round takes 10 bytes plus, per value, the type
// name + 1 and 4 bytes for the value: {TEMPERATURE, HUMIDITY} = 10 + 16 + 13 = 39 bytes.
// Plain C++: it runs in the native tests (extras/test) as well.
#include <stddef.h>
#include <stdint.h>

#include "HydroNodeProtocol.h"

class HydroNodeBuffer {
public:
    /** What add() does when the memory is full. */
    enum WhenFull : uint8_t {
        DROP_OLDEST,  // make room by dropping the oldest rounds (default: the newest data wins)
        DROP_NEWEST   // keep what is stored, refuse the new round
    };

    static constexpr size_t HEADER_SIZE = 16;
    static constexpr size_t ROUND_HEADER_SIZE = 10;
    static constexpr size_t MAX_VALUES_PER_ROUND = 64;

    /** One stored round, as read by round(). `data` points into the buffer. */
    struct Round {
        uint32_t time;    // Unix seconds when measured (or the unsynced board clock, see synced)
        bool synced;      // false: measured before the clock was set, see shiftUnsynced()
        uint16_t runTag;  // which run of the board measured it (only matters while unsynced)
        uint8_t count;    // values in the round
        const uint8_t* data;
        size_t length;
    };

    /** `memory` must stay valid as long as the buffer is used; at least 64 bytes. */
    HydroNodeBuffer(void* memory, size_t size, WhenFull whenFull = DROP_OLDEST);

    /**
     * Checks what the memory holds. Rounds stored before (RTC memory after deep sleep, bytes you
     * loaded from flash) are kept when the content checks out; anything else starts empty.
     * Returns the number of rounds kept.
     */
    size_t begin();

    /** Drops every round. */
    void clear();

    /**
     * Stores one round measured at `time` (Unix seconds). Values with an invalid type, a value
     * that is not finite, or a type already in the round are skipped. With `synced` false, `time`
     * is the board clock before it was set; HydroNodeClient fixes it after the next time sync.
     * Returns false when nothing was stored (no valid value, round too big, or full with
     * DROP_NEWEST).
     */
    bool add(uint32_t time, const HydroNodeValue* values, size_t count, bool synced = true, uint16_t runTag = 0);

    /** Rounds stored. */
    size_t rounds() const;
    bool empty() const { return rounds() == 0; }

    /** Bytes in use (without the header) and the bytes rounds can use at most. */
    size_t usedBytes() const;
    size_t capacityBytes() const { return size_ > HEADER_SIZE ? size_ - HEADER_SIZE : 0; }

    /** Percent of the memory in use, 0-100. */
    uint8_t fillPercent() const;

    /** Rounds dropped for lack of space since the last clear() (stops at 65535). */
    uint16_t dropped() const;

    /** Unix time of the oldest round, 0 when empty. */
    uint32_t oldestTime() const;

    /** Reads round `index` (0 = oldest). False when there is no such round. */
    bool round(size_t index, Round& out) const;

    /** Value `i` of a round. The type points into the buffer and stays valid until it changes. */
    static bool value(const Round& round, size_t i, HydroNodeValue& out);

    /** Removes round `index` (0 = oldest). */
    void remove(size_t index);

    /** Removes the `n` oldest rounds. */
    void removeFirst(size_t n);

    /**
     * The clock was set: rounds of run `runTag` measured before move by `deltaSeconds` and count as
     * synced from now on. Returns how many rounds changed.
     */
    size_t shiftUnsynced(uint16_t runTag, int64_t deltaSeconds);

    /** Drops unsynced rounds of runs other than `currentRunTag`: their clock is gone. */
    size_t dropStaleUnsynced(uint16_t currentRunTag);

    /** Bytes a round of these values needs, 0 when it can never fit (too many values). */
    static size_t roundBytes(const HydroNodeValue* values, size_t count);

    /** The whole memory, to save it to flash: write size() bytes from data(). */
    const uint8_t* data() const { return memory_; }
    uint8_t* data() { return memory_; }
    size_t size() const { return size_; }

    /** Changes the policy for a full buffer. */
    void setWhenFull(WhenFull whenFull) { whenFull_ = whenFull; }

private:
    uint8_t* memory_;
    size_t size_;
    WhenFull whenFull_;

    uint32_t read32(size_t offset) const;
    void write32(size_t offset, uint32_t value);
    uint16_t read16(size_t offset) const;
    void write16(size_t offset, uint16_t value);
    void setUsed(size_t used);
    void setRounds(size_t rounds);
    void setDropped(uint16_t dropped);
    uint32_t checksum() const;
    void seal();
    size_t offsetOf(size_t index) const;
    bool walk() const;
};
