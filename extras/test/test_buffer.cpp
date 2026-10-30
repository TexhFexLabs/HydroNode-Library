// Native tests of HydroNodeBuffer: storing, reading, full memory, persistence and dating rounds
// measured before the clock was set. Run with `python3 extras/test/run_tests.py`.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "HydroNodeBuffer.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

static const HydroNodeValue TH[] = {{"TEMPERATURE", 21.5f}, {"HUMIDITY", 48.0f}};

static std::string typeOf(const HydroNodeBuffer& b, size_t round, size_t value) {
    HydroNodeBuffer::Round r;
    HydroNodeValue v;
    if (!b.round(round, r) || !HydroNodeBuffer::value(r, value, v)) return "";
    return v.type;
}

static void storesAndReadsRounds() {
    uint8_t mem[512] = {};
    HydroNodeBuffer b(mem, sizeof(mem));
    CHECK(b.begin() == 0);
    CHECK(b.empty());
    CHECK(b.capacityBytes() == 512 - HydroNodeBuffer::HEADER_SIZE);

    CHECK(b.add(1000, TH, 2));
    CHECK(b.add(1300, TH, 1));
    CHECK(b.rounds() == 2);
    CHECK(b.usedBytes() == HydroNodeBuffer::roundBytes(TH, 2) + HydroNodeBuffer::roundBytes(TH, 1));
    CHECK(HydroNodeBuffer::roundBytes(TH, 2) == 10 + 16 + 13);
    CHECK(b.oldestTime() == 1000);

    HydroNodeBuffer::Round r;
    CHECK(b.round(0, r));
    CHECK(r.time == 1000 && r.count == 2 && r.synced);
    HydroNodeValue v;
    CHECK(HydroNodeBuffer::value(r, 1, v));
    CHECK(strcmp(v.type, "HUMIDITY") == 0 && v.value == 48.0f);
    CHECK(!HydroNodeBuffer::value(r, 2, v));
    CHECK(!b.round(2, r));
}

static void skipsWhatTheServerWouldRefuse() {
    uint8_t mem[256] = {};
    HydroNodeBuffer b(mem, sizeof(mem));
    b.begin();
    HydroNodeValue mixed[] = {{"temperature", 1}, {"HUMIDITY", 0.0f / 0.0f}, {"CO2", 600}, {"CO2", 601}};
    CHECK(b.add(1, mixed, 4));
    HydroNodeBuffer::Round r;
    b.round(0, r);
    CHECK(r.count == 1);
    CHECK(typeOf(b, 0, 0) == "CO2");
    HydroNodeValue none[] = {{"bad", 1}};
    CHECK(!b.add(2, none, 1));
    CHECK(b.rounds() == 1);
}

static void fullMemoryDropsTheOldestOrTheNewest() {
    uint8_t mem[HydroNodeBuffer::HEADER_SIZE + 39 * 3] = {};  // exactly three {TEMPERATURE, HUMIDITY} rounds
    HydroNodeBuffer b(mem, sizeof(mem));
    b.begin();
    for (uint32_t t = 1; t <= 3; t++) CHECK(b.add(t, TH, 2));
    CHECK(b.fillPercent() == 100);
    CHECK(b.add(4, TH, 2));  // DROP_OLDEST
    CHECK(b.rounds() == 3 && b.oldestTime() == 2 && b.dropped() == 1);

    b.setWhenFull(HydroNodeBuffer::DROP_NEWEST);
    CHECK(!b.add(5, TH, 2));
    CHECK(b.rounds() == 3 && b.oldestTime() == 2 && b.dropped() == 2);

    // A round bigger than the whole memory never goes in.
    std::vector<HydroNodeValue> many(64, HydroNodeValue{"X", 1});
    std::vector<std::string> names;
    for (int i = 0; i < 64; i++) names.push_back("TYPE_" + std::to_string(i));
    for (int i = 0; i < 64; i++) many[i].type = names[i].c_str();
    CHECK(!b.add(6, many.data(), many.size()));
}

static void removesRounds() {
    uint8_t mem[512] = {};
    HydroNodeBuffer b(mem, sizeof(mem));
    b.begin();
    for (uint32_t t = 1; t <= 5; t++) b.add(t, TH, 2);
    b.remove(2);
    CHECK(b.rounds() == 4);
    HydroNodeBuffer::Round r;
    b.round(2, r);
    CHECK(r.time == 4);
    b.removeFirst(2);
    CHECK(b.rounds() == 2 && b.oldestTime() == 4);
    b.removeFirst(10);
    CHECK(b.empty() && b.usedBytes() == 0);
}

static void survivesARestartOnlyWhenIntact() {
    uint8_t mem[256] = {};
    {
        HydroNodeBuffer b(mem, sizeof(mem));
        b.begin();
        b.add(100, TH, 2);
        b.add(200, TH, 2);
    }
    HydroNodeBuffer again(mem, sizeof(mem));  // e.g. RTC memory after deep sleep
    CHECK(again.begin() == 2);
    CHECK(typeOf(again, 1, 1) == "HUMIDITY");

    mem[HydroNodeBuffer::HEADER_SIZE + 12] ^= 0x40;  // a flipped bit in a type name
    HydroNodeBuffer broken(mem, sizeof(mem));
    CHECK(broken.begin() == 0);
    CHECK(broken.empty());

    uint8_t garbage[128];
    memset(garbage, 0xA5, sizeof(garbage));  // never written: power-on RAM
    HydroNodeBuffer fresh(garbage, sizeof(garbage));
    CHECK(fresh.begin() == 0);
    CHECK(fresh.add(1, TH, 1));
}

static void roundsMeasuredBeforeTheTimeSyncGetDated() {
    uint8_t mem[512] = {};
    HydroNodeBuffer b(mem, sizeof(mem));
    b.begin();
    b.add(12, TH, 2, false, 7);   // 12 s after boot, run 7
    b.add(42, TH, 2, false, 7);
    b.add(30, TH, 1, false, 3);   // an earlier run: its clock is gone
    b.add(1700000000, TH, 1, true, 7);
    CHECK(b.shiftUnsynced(7, 1700000000 - 50) == 2);
    HydroNodeBuffer::Round r;
    b.round(0, r);
    CHECK(r.synced && r.time == 1700000000 - 38);
    b.round(1, r);
    CHECK(r.synced && r.time == 1700000000 - 8);
    CHECK(b.dropStaleUnsynced(7) == 1);
    CHECK(b.rounds() == 3);
    HydroNodeBuffer again(mem, sizeof(mem));
    CHECK(again.begin() == 3);  // shifting kept the checksum right
}

static void tooSmallMemory() {
    uint8_t mem[8] = {};
    HydroNodeBuffer b(mem, sizeof(mem));
    CHECK(b.begin() == 0);
    CHECK(!b.add(1, TH, 1));
    HydroNodeBuffer none(nullptr, 100);
    CHECK(none.begin() == 0);
    CHECK(!none.add(1, TH, 1));
}

int main() {
    storesAndReadsRounds();
    skipsWhatTheServerWouldRefuse();
    fullMemoryDropsTheOldestOrTheNewest();
    removesRounds();
    survivesARestartOnlyWhenIntact();
    roundsMeasuredBeforeTheTimeSyncGetDated();
    tooSmallMemory();
    printf("buffer: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
