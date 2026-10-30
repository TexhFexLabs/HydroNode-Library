#include "HydroNodeBuffer.h"

#include <math.h>
#include <string.h>

// Header: magic, used bytes, rounds, dropped, checksum (little endian).
static constexpr uint32_t MAGIC = 0x31424E48;  // "HNB1"
static constexpr size_t AT_MAGIC = 0;
static constexpr size_t AT_USED = 4;
static constexpr size_t AT_ROUNDS = 8;
static constexpr size_t AT_DROPPED = 10;
static constexpr size_t AT_CHECKSUM = 12;

// Round: length, time, flags, count, run tag, then per value "TYPE\0" + 4 bytes float.
static constexpr size_t R_LENGTH = 0;
static constexpr size_t R_TIME = 2;
static constexpr size_t R_FLAGS = 6;
static constexpr size_t R_COUNT = 7;
static constexpr size_t R_TAG = 8;
static constexpr uint8_t FLAG_UNSYNCED = 0x01;

HydroNodeBuffer::HydroNodeBuffer(void* memory, size_t size, WhenFull whenFull)
    : memory_(static_cast<uint8_t*>(memory)), size_(memory ? size : 0), whenFull_(whenFull) {}

uint32_t HydroNodeBuffer::read32(size_t o) const {
    return uint32_t(memory_[o]) | (uint32_t(memory_[o + 1]) << 8) | (uint32_t(memory_[o + 2]) << 16) |
           (uint32_t(memory_[o + 3]) << 24);
}

void HydroNodeBuffer::write32(size_t o, uint32_t v) {
    memory_[o] = uint8_t(v);
    memory_[o + 1] = uint8_t(v >> 8);
    memory_[o + 2] = uint8_t(v >> 16);
    memory_[o + 3] = uint8_t(v >> 24);
}

uint16_t HydroNodeBuffer::read16(size_t o) const { return uint16_t(memory_[o] | (memory_[o + 1] << 8)); }

void HydroNodeBuffer::write16(size_t o, uint16_t v) {
    memory_[o] = uint8_t(v);
    memory_[o + 1] = uint8_t(v >> 8);
}

size_t HydroNodeBuffer::usedBytes() const { return size_ < HEADER_SIZE ? 0 : read32(AT_USED); }
size_t HydroNodeBuffer::rounds() const { return size_ < HEADER_SIZE ? 0 : read16(AT_ROUNDS); }
uint16_t HydroNodeBuffer::dropped() const { return size_ < HEADER_SIZE ? 0 : read16(AT_DROPPED); }
void HydroNodeBuffer::setUsed(size_t used) { write32(AT_USED, uint32_t(used)); }
void HydroNodeBuffer::setRounds(size_t rounds) { write16(AT_ROUNDS, uint16_t(rounds)); }
void HydroNodeBuffer::setDropped(uint16_t dropped) { write16(AT_DROPPED, dropped); }

uint8_t HydroNodeBuffer::fillPercent() const {
    size_t capacity = capacityBytes();
    return capacity == 0 ? 100 : uint8_t((usedBytes() * 100 + capacity - 1) / capacity);
}

/** FNV-1a over the header fields after the magic and the used bytes. */
uint32_t HydroNodeBuffer::checksum() const {
    uint32_t hash = 2166136261u;
    for (size_t i = AT_USED; i < AT_CHECKSUM; i++) hash = (hash ^ memory_[i]) * 16777619u;
    size_t end = HEADER_SIZE + read32(AT_USED);
    for (size_t i = HEADER_SIZE; i < end; i++) hash = (hash ^ memory_[i]) * 16777619u;
    return hash;
}

void HydroNodeBuffer::seal() { write32(AT_CHECKSUM, checksum()); }

void HydroNodeBuffer::clear() {
    if (size_ < HEADER_SIZE) return;
    write32(AT_MAGIC, MAGIC);
    setUsed(0);
    setRounds(0);
    setDropped(0);
    seal();
}

/** True when the stored rounds add up to the header: lengths, counts and terminators fit. */
bool HydroNodeBuffer::walk() const {
    size_t used = read32(AT_USED);
    if (used > capacityBytes()) return false;
    size_t offset = HEADER_SIZE, end = HEADER_SIZE + used, found = 0;
    while (offset < end) {
        if (end - offset < ROUND_HEADER_SIZE) return false;
        size_t length = read16(offset + R_LENGTH);
        if (length < ROUND_HEADER_SIZE || length > end - offset) return false;
        uint8_t count = memory_[offset + R_COUNT];
        if (count == 0 || count > MAX_VALUES_PER_ROUND) return false;
        size_t p = offset + ROUND_HEADER_SIZE, stop = offset + length;
        for (uint8_t i = 0; i < count; i++) {
            const void* zero = memchr(memory_ + p, 0, stop - p);
            if (!zero) return false;
            p = size_t(static_cast<const uint8_t*>(zero) - memory_) + 1 + sizeof(float);
            if (p > stop) return false;
        }
        if (p != stop) return false;
        offset = stop;
        found++;
    }
    return found == rounds();
}

size_t HydroNodeBuffer::begin() {
    if (size_ < HEADER_SIZE + ROUND_HEADER_SIZE) {
        size_ = 0;  // too small to hold anything
        return 0;
    }
    if (read32(AT_MAGIC) != MAGIC || read32(AT_CHECKSUM) != checksum() || !walk()) {
        clear();
        return 0;
    }
    return rounds();
}

size_t HydroNodeBuffer::roundBytes(const HydroNodeValue* values, size_t count) {
    if (count == 0 || count > MAX_VALUES_PER_ROUND) return 0;
    size_t bytes = ROUND_HEADER_SIZE;
    for (size_t i = 0; i < count; i++) bytes += strlen(values[i].type) + 1 + sizeof(float);
    return bytes;
}

size_t HydroNodeBuffer::offsetOf(size_t index) const {
    size_t offset = HEADER_SIZE;
    for (size_t i = 0; i < index; i++) offset += read16(offset + R_LENGTH);
    return offset;
}

bool HydroNodeBuffer::add(uint32_t time, const HydroNodeValue* values, size_t count, bool synced, uint16_t runTag) {
    if (size_ < HEADER_SIZE || !values) return false;

    // The same checks sendValues() makes: invalid entries are left out, the rest is stored.
    HydroNodeValue kept[MAX_VALUES_PER_ROUND];
    size_t n = 0;
    for (size_t i = 0; i < count && n < MAX_VALUES_PER_ROUND; i++) {
        if (!hydronode::isValidType(values[i].type) || !isfinite(values[i].value)) continue;
        bool duplicate = false;
        for (size_t k = 0; k < n; k++) duplicate = duplicate || strcmp(kept[k].type, values[i].type) == 0;
        if (!duplicate) kept[n++] = values[i];
    }
    size_t bytes = roundBytes(kept, n);
    if (bytes == 0 || bytes > capacityBytes() || bytes > 0xFFFF) return false;

    size_t used = usedBytes();
    if (used + bytes > capacityBytes()) {
        if (whenFull_ == DROP_NEWEST) {
            uint16_t d = dropped();
            if (d < 0xFFFF) setDropped(d + 1);
            seal();
            return false;
        }
        // Drop the oldest rounds until the new one fits.
        size_t drop = 0, freed = 0;
        size_t offset = HEADER_SIZE;
        while (used - freed + bytes > capacityBytes()) {
            size_t length = read16(offset + R_LENGTH);
            freed += length;
            offset += length;
            drop++;
        }
        memmove(memory_ + HEADER_SIZE, memory_ + HEADER_SIZE + freed, used - freed);
        used -= freed;
        setRounds(rounds() - drop);
        uint32_t d = uint32_t(dropped()) + drop;
        setDropped(d > 0xFFFF ? 0xFFFF : uint16_t(d));
    }

    size_t at = HEADER_SIZE + used;
    write16(at + R_LENGTH, uint16_t(bytes));
    write32(at + R_TIME, time);
    memory_[at + R_FLAGS] = synced ? 0 : FLAG_UNSYNCED;
    memory_[at + R_COUNT] = uint8_t(n);
    write16(at + R_TAG, runTag);
    size_t p = at + ROUND_HEADER_SIZE;
    for (size_t i = 0; i < n; i++) {
        size_t length = strlen(kept[i].type) + 1;
        memcpy(memory_ + p, kept[i].type, length);
        p += length;
        memcpy(memory_ + p, &kept[i].value, sizeof(float));
        p += sizeof(float);
    }
    setUsed(used + bytes);
    setRounds(rounds() + 1);
    seal();
    return true;
}

uint32_t HydroNodeBuffer::oldestTime() const {
    Round r;
    return round(0, r) ? r.time : 0;
}

bool HydroNodeBuffer::round(size_t index, Round& out) const {
    if (index >= rounds()) return false;
    size_t offset = offsetOf(index);
    out.time = read32(offset + R_TIME);
    out.synced = (memory_[offset + R_FLAGS] & FLAG_UNSYNCED) == 0;
    out.runTag = read16(offset + R_TAG);
    out.count = memory_[offset + R_COUNT];
    out.length = read16(offset + R_LENGTH);
    out.data = memory_ + offset + ROUND_HEADER_SIZE;
    return true;
}

bool HydroNodeBuffer::value(const Round& round, size_t i, HydroNodeValue& out) {
    if (i >= round.count || !round.data) return false;
    const uint8_t* p = round.data;
    for (size_t k = 0;; k++) {
        size_t length = strlen(reinterpret_cast<const char*>(p));
        if (k == i) {
            out.type = reinterpret_cast<const char*>(p);
            memcpy(&out.value, p + length + 1, sizeof(float));
            return true;
        }
        p += length + 1 + sizeof(float);
    }
}

void HydroNodeBuffer::remove(size_t index) {
    if (index >= rounds()) return;
    size_t offset = offsetOf(index);
    size_t length = read16(offset + R_LENGTH);
    size_t end = HEADER_SIZE + usedBytes();
    memmove(memory_ + offset, memory_ + offset + length, end - offset - length);
    setUsed(usedBytes() - length);
    setRounds(rounds() - 1);
    seal();
}

void HydroNodeBuffer::removeFirst(size_t n) {
    if (n == 0) return;
    if (n >= rounds()) {
        uint16_t d = dropped();
        clear();
        setDropped(d);
        seal();
        return;
    }
    size_t offset = offsetOf(n);
    size_t end = HEADER_SIZE + usedBytes();
    memmove(memory_ + HEADER_SIZE, memory_ + offset, end - offset);
    setUsed(end - offset);
    setRounds(rounds() - n);
    seal();
}

size_t HydroNodeBuffer::shiftUnsynced(uint16_t runTag, int64_t deltaSeconds) {
    size_t changed = 0, offset = HEADER_SIZE;
    for (size_t i = 0; i < rounds(); i++) {
        if ((memory_[offset + R_FLAGS] & FLAG_UNSYNCED) && read16(offset + R_TAG) == runTag) {
            int64_t time = int64_t(read32(offset + R_TIME)) + deltaSeconds;
            write32(offset + R_TIME, time > 0 && time <= 0xFFFFFFFFLL ? uint32_t(time) : 0);
            memory_[offset + R_FLAGS] &= uint8_t(~FLAG_UNSYNCED);
            changed++;
        }
        offset += read16(offset + R_LENGTH);
    }
    if (changed) seal();
    return changed;
}

size_t HydroNodeBuffer::dropStaleUnsynced(uint16_t currentRunTag) {
    size_t removed = 0;
    for (size_t i = rounds(); i-- > 0;) {
        Round r;
        if (round(i, r) && !r.synced && r.runTag != currentRunTag) {
            remove(i);
            removed++;
        }
    }
    return removed;
}
