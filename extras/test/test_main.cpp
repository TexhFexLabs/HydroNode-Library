// Native tests for the parts of the library without Arduino: HydroNodeBatteryGuard and the
// X-Device-Config format. Run them with `python3 extras/test/run_tests.py`.
//
//   test_main            built-in cases, exit code 1 on a failure
//   test_main vectors    reads "<CHEMISTRY|null> <cells> <save> <rec> <sby> <res>" per line and
//                        prints the broken fields (sorted, comma separated) per line
#include <stdio.h>
#include <string.h>

#include <string>

#include "HydroNodeBatteryGuard.h"
#include "HydroNodeDeviceConfig.h"

using G = HydroNodeBatteryGuard;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_STR(actual, expected)                                                          \
    do {                                                                                     \
        checks++;                                                                            \
        if (strcmp((actual), (expected)) != 0) {                                             \
            failures++;                                                                      \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (actual), (expected)); \
        }                                                                                    \
    } while (0)

static G lipo() {
    return G(G::defaults(G::LIPO, 1));  // save 3500, recovery 3300, standby 3200, resume 3600
}

/** A guard already running in NORMAL. */
static G running() {
    G g = lipo();
    g.update(3800, true, 0);
    return g;
}

static void firstReadingDecidesLikeABoot() {
    G a = lipo();
    CHECK(a.state() == G::NORMAL);
    CHECK(a.update(3600, true, 0) == G::NORMAL);
    CHECK(!a.changed());

    G b = lipo();
    CHECK(b.update(3550, true, 0) == G::RECOVERY);  // above recovery, below resume
    CHECK(b.changed());

    G c = lipo();
    CHECK(c.update(3620, true, 0) == G::NORMAL);  // below save + 150, but SAVE starts below save

    G d = lipo();
    CHECK(d.update(0, false, 0) == G::RECOVERY);
}

static void saveHasHysteresis() {
    G g = running();
    CHECK(g.update(3499, true, 60) == G::SAVE);
    CHECK(g.changed());
    CHECK(g.update(3649, true, 120) == G::SAVE);
    CHECK(!g.changed());
    CHECK(g.update(3650, true, 180) == G::NORMAL);
    CHECK(g.update(3500, true, 240) == G::NORMAL);  // save itself is not below save
}

static void recoveryAndStandby() {
    G g = running();
    CHECK(g.update(3299, true, 60) == G::RECOVERY);
    CHECK(g.update(3199, true, 120) == G::RECOVERY);  // first reading below standby
    CHECK(g.update(3199, true, 180) == G::STANDBY);
    CHECK(g.changed());

    // A reading between standby and recovery starts the count again.
    G h = running();
    h.update(3199, true, 60);
    h.update(3250, true, 120);
    CHECK(h.update(3199, true, 180) == G::RECOVERY);
    CHECK(h.update(3199, true, 240) == G::STANDBY);
}

static void invalidReadingsNeverLeadToStandby() {
    G g = running();
    CHECK(g.update(3199, true, 60) == G::RECOVERY);
    CHECK(g.update(0, false, 120) == G::RECOVERY);
    CHECK(g.update(3199, true, 180) == G::RECOVERY);  // the invalid one reset the count
    CHECK(g.update(3199, true, 240) == G::STANDBY);

    G h = running();
    for (int i = 0; i < 10; i++) h.update(0, false, 60u * i);
    CHECK(h.state() == G::RECOVERY);
}

static void threeInvalidReadingsTurnTheRadioOff() {
    G g = running();
    CHECK(g.update(0, false, 60) == G::NORMAL);
    CHECK(g.update(0, false, 120) == G::NORMAL);
    CHECK(g.update(0, false, 180) == G::RECOVERY);

    G h = running();
    h.update(0, false, 60);
    h.update(0, false, 120);
    CHECK(h.update(3800, true, 180) == G::NORMAL);  // a valid one resets the count
    CHECK(h.update(0, false, 240) == G::NORMAL);
}

static void resumeNeedsTwoReadingsSixtySecondsApart() {
    G g = running();
    g.update(3200, true, 0);
    CHECK(g.state() == G::RECOVERY);
    CHECK(g.update(3620, true, 100) == G::RECOVERY);
    CHECK(g.update(3620, true, 130) == G::RECOVERY);  // two readings, only 30 s
    CHECK(g.update(3620, true, 160) == G::SAVE);      // 60 s; 3620 < save + 150
    CHECK(g.changed());

    G h = running();
    h.update(3200, true, 0);
    h.update(3800, true, 60);
    CHECK(h.update(3800, true, 120) == G::NORMAL);

    // A dip below resume starts over.
    G k = running();
    k.update(3200, true, 0);
    k.update(3650, true, 60);
    k.update(3550, true, 120);
    CHECK(k.update(3650, true, 180) == G::RECOVERY);
    CHECK(k.update(3650, true, 240) == G::NORMAL);

    // The clock may wrap.
    G w = running();
    w.update(3200, true, 0xFFFFFFF0u);
    w.update(3800, true, 0xFFFFFFF0u);
    CHECK(w.update(3800, true, 0x30u) == G::NORMAL);
}

static void standbyWakesOnlyAtResume() {
    G g = running();
    g.update(3100, true, 0);
    g.update(3100, true, 60);
    CHECK(g.state() == G::STANDBY);
    CHECK(g.update(3500, true, 3660) == G::STANDBY);
    CHECK(g.update(0, false, 7260) == G::STANDBY);
    CHECK(g.update(3600, true, 10860) == G::RECOVERY);
    CHECK(g.changed());
    CHECK(g.update(3600, true, 10920) == G::SAVE);
}

static void timing() {
    G g = running();
    CHECK(g.intervalFactor() == 1);
    CHECK(g.sleepSeconds(300) == 300);
    CHECK(g.radioAllowed());
    CHECK_STR(g.stateName(), "normal");
    g.update(3400, true, 60);
    CHECK(g.intervalFactor() == 2);
    CHECK(g.sleepSeconds(300) == 600);
    CHECK_STR(g.stateName(), "save");
    g.update(3000, true, 120);
    CHECK(g.intervalFactor() == 0);
    CHECK(!g.radioAllowed());
    CHECK(g.sleepSeconds(300) == 60);
    CHECK_STR(g.stateName(), "recovery");
    g.update(3000, true, 180);
    CHECK(g.sleepSeconds(300) == 3600);
    CHECK_STR(g.stateName(), "standby");
}

static void memorySurvivesSleep() {
    G g = running();
    g.update(3200, true, 0);
    g.update(3700, true, 60);
    G::Memory m = g.memory();

    G h = lipo();
    CHECK(h.restore(m));
    CHECK(h.state() == G::RECOVERY);
    CHECK(h.update(3700, true, 120) == G::NORMAL);

    G::Memory garbage = m;
    garbage.magic = 0;
    G k = running();
    CHECK(!k.restore(garbage));
    CHECK(k.update(3550, true, 0) == G::RECOVERY);  // starts like a boot
    garbage = m;
    garbage.state = 9;
    CHECK(!k.restore(garbage));
}

static void newThresholdsApplyAtTheNextReading() {
    G g = running();
    g.setThresholds({3700, 3300, 3200, 3600});
    CHECK(g.state() == G::NORMAL);
    CHECK(g.update(3690, true, 60) == G::SAVE);
}

static void presets() {
    HydroNodeThresholds lipo2 = G::defaults(G::LIPO, 2);
    CHECK(lipo2.saveMv == 7000 && lipo2.recoveryMv == 6600 && lipo2.standbyMv == 6400 && lipo2.resumeMv == 7200);
    HydroNodeThresholds liion = G::defaults(G::LI_ION);
    CHECK(liion.saveMv == 3450 && liion.recoveryMv == 3200 && liion.standbyMv == 3000 && liion.resumeMv == 3500);
    HydroNodeThresholds lfp = G::defaults(G::LIFEPO4);
    CHECK(lfp.saveMv == 3100 && lfp.recoveryMv == 3000 && lfp.standbyMv == 2800 && lfp.resumeMv == 3200);
    CHECK(G::validate(G::defaults(G::LIPO, 3), 3, G::LIPO) == 0);
    CHECK(G::validate(G::defaults(G::LIFEPO4, 4), 4, G::LIFEPO4) == 0);
    CHECK(G::validate({3500, 3300, 3200, 3600}, 1, 0, 0) == 0);
    CHECK(G::validate({3500, 3300, 3290, 3600}, 1, 0, 0) == G::FIELD_STANDBY);
}

static std::string format(const HydroNodeDeviceConfig& c, bool caps, int32_t rej = -1, const char* err = nullptr,
                          size_t size = 300) {
    char buffer[300];
    size_t n = hydronode::formatDeviceConfig(buffer, size, c, caps, rej, err);
    CHECK(n == strlen(buffer));
    return buffer;
}

static HydroNodeDeviceConfig masterExample() {
    HydroNodeDeviceConfig c;
    c.revision = 7;
    c.intervalSeconds = 300;
    c.saveMv = 3500;
    c.recoveryMv = 3300;
    c.standbyMv = 3200;
    c.resumeMv = 3600;
    c.source = "bat";
    c.gauge = "max17048";
    c.cells = 1;
    c.capacityMah = 2000;
    c.powerState = "normal";
    return c;
}

static void deviceConfigHeader() {
    CHECK_STR(format(masterExample(), true).c_str(),
              "v=1 rev=7 int=300 save=3500 rec=3300 sby=3200 res=3600 src=bat gauge=max17048 cells=1 mah=2000 "
              "pwr=normal caps=settings");
    CHECK_STR(format(masterExample(), false, 8, "invalid").c_str(),
              "v=1 rev=7 int=300 save=3500 rec=3300 sby=3200 res=3600 src=bat gauge=max17048 cells=1 mah=2000 "
              "pwr=normal rej=8 err=invalid");

    HydroNodeDeviceConfig empty;
    CHECK_STR(format(empty, false).c_str(), "v=1 rev=0");
    CHECK_STR(format(empty, true, 0).c_str(), "v=1 rev=0 caps=settings rej=0");

    // An error of the board itself; a refusal's reason wins.
    HydroNodeDeviceConfig noMeasurement;
    noMeasurement.source = "bat";
    noMeasurement.error = "no_measurement";
    CHECK_STR(format(noMeasurement, false).c_str(), "v=1 rev=0 src=bat err=no_measurement");
    CHECK_STR(format(noMeasurement, false, 3, "invalid").c_str(), "v=1 rev=0 src=bat rej=3 err=invalid");

    HydroNodeDeviceConfig usb;
    usb.intervalSeconds = 60;
    usb.source = "USB";
    CHECK_STR(format(usb, false).c_str(), "v=1 rev=0 int=60 src=usb");

    // Text values cannot break the header or add keys.
    HydroNodeDeviceConfig evil;
    evil.source = "b a;t\r\nX-Evil: 1";
    evil.gauge = " ";
    evil.powerState = "save=1";
    CHECK_STR(format(evil, false).c_str(), "v=1 rev=0 src=batxevil1 pwr=save1");

    // A key that does not fit is left out whole; nothing goes past the buffer.
    std::string small = format(masterExample(), true, -1, nullptr, 24);
    CHECK_STR(small.c_str(), "v=1 rev=7 int=300");
    CHECK(format(masterExample(), true, 65535, "storage").size() <= hydronode::DEVICE_CONFIG_MAX);
}

static void settingsChecks() {
    HydroNodeSettings s{8, 300, 3500, 3300, 3200, 3600, 0};
    CHECK(hydronode::settingsValid(s));
    s.intervalSeconds = 9;
    CHECK(!hydronode::settingsValid(s));
    s.intervalSeconds = 10;
    CHECK(hydronode::settingsValid(s));
    s.intervalSeconds = 604800;
    CHECK(hydronode::settingsValid(s));
    s.intervalSeconds = 604801;
    CHECK(!hydronode::settingsValid(s));
    s.intervalSeconds = 300;
    s.standbyMv = 3251;
    CHECK(!hydronode::settingsValid(s));
    HydroNodeSettings intervalOnly{2, 120, 0, 0, 0, 0, 0};
    CHECK(hydronode::settingsValid(intervalOnly));
}

static G::Chemistry chemistry(const char* name) {
    if (strcmp(name, "LI_ION") == 0) return G::LI_ION;
    if (strcmp(name, "LIFEPO4") == 0) return G::LIFEPO4;
    return G::LIPO;  // LIPO and no chemistry
}

static int vectors() {
    char chem[32];
    unsigned cells, save, rec, sby, res;
    while (scanf("%31s %u %u %u %u %u", chem, &cells, &save, &rec, &sby, &res) == 6) {
        HydroNodeThresholds t{static_cast<uint16_t>(save), static_cast<uint16_t>(rec), static_cast<uint16_t>(sby),
                              static_cast<uint16_t>(res)};
        uint8_t broken = G::validate(t, static_cast<uint8_t>(cells), chemistry(chem));
        std::string out;
        const char* names[] = {"recovery", "resume", "save", "standby"};
        const uint8_t bits[] = {G::FIELD_RECOVERY, G::FIELD_RESUME, G::FIELD_SAVE, G::FIELD_STANDBY};
        for (int i = 0; i < 4; i++) {
            if (!(broken & bits[i])) continue;
            if (!out.empty()) out += ",";
            out += names[i];
        }
        printf("%s\n", out.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "vectors") == 0) return vectors();
    firstReadingDecidesLikeABoot();
    saveHasHysteresis();
    recoveryAndStandby();
    invalidReadingsNeverLeadToStandby();
    threeInvalidReadingsTurnTheRadioOff();
    resumeNeedsTwoReadingsSixtySecondsApart();
    standbyWakesOnlyAtResume();
    timing();
    memorySurvivesSleep();
    newThresholdsApplyAtTheNextReading();
    presets();
    deviceConfigHeader();
    settingsChecks();
    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
