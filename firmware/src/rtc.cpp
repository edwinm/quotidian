#include "rtc.h"

#include <Wire.h>
#include <sys/time.h>

#include "config.h"
#include "epd_driver.h"
#include "settings.h"
#include "utilities.h"

// PCF8563 over plain Wire.
//
// The I2C pull-ups on this board hang off the switched PWR_EN rail, not the
// always-on one. With the rail down, SDA and SCL are held up only by the
// ESP32's weak internal pull-ups - just enough for a read to limp through,
// never enough for a write to be acknowledged. The symptom is brutal to chase:
// the clock reads back plausible values while every write is silently lost, so
// the wake alarm ends up armed against a time the chip never accepted.
//
// It took a while to see because the failure depends on *when* you talk to the
// chip, not how. The same raw write succeeds early in boot and NAKs once
// batteryRead() has cycled epd_poweron()/epd_poweroff_all(). Proven directly:
//
//   [rtc] write NAKed - retrying with the panel rail powered
//   [rtc] retry with rail on: ACKED
//
// So every register access here brings the rail up first. SensorLib is not
// used: its transport was an early suspect and this code had to be able to
// rule the library out.

static constexpr uint8_t kAddr = 0x51;

// Register map.
static constexpr uint8_t kRegStatus2   = 0x01;  // bit1 AIE, bit3 AF
static constexpr uint8_t kRegSeconds   = 0x02;  // bit7 VL: time is unreliable
static constexpr uint8_t kRegAlarmMin  = 0x09;  // 0x09..0x0C, bit7 disables each
static constexpr uint8_t kAlarmDisable = 0x80;

static bool sAvailable = false;

// --- Bus power --------------------------------------------------------------

// Reference counted, so a public call that reads and then writes does not drop
// the rail halfway through its own transaction.
static int sPowerDepth = 0;

namespace {
struct BusPower {
    BusPower() {
        if (sPowerDepth++ == 0) {
            epd_poweron();
            delay(5);  // let the rail and the pull-ups come up
        }
    }
    ~BusPower() {
        if (--sPowerDepth == 0) epd_poweroff_all();
    }
};
}  // namespace

// --- Transport --------------------------------------------------------------
// Callers must already hold a BusPower.

static bool readRegs(uint8_t reg, uint8_t *out, uint8_t len) {
    Wire.beginTransmission(kAddr);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;

    if (Wire.requestFrom(kAddr, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) out[i] = Wire.read();
    return true;
}

static bool writeRegs(uint8_t reg, const uint8_t *data, uint8_t len) {
    Wire.beginTransmission(kAddr);
    Wire.write(reg);
    for (uint8_t i = 0; i < len; i++) Wire.write(data[i]);
    return Wire.endTransmission() == 0;
}

static uint8_t toBcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
static uint8_t fromBcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

// Reads the seven time registers. `vl` reports the chip's own voltage-low flag:
// when set, it has been counting up from whatever it powered on with, and the
// values can look entirely plausible - this board read a confident 2025-02-12.
static bool readClock(struct tm *utc, bool *vl) {
    uint8_t r[7];
    if (!readRegs(kRegSeconds, r, 7)) return false;

    *vl = (r[0] & 0x80) != 0;

    utc->tm_sec  = fromBcd(r[0] & 0x7F);
    utc->tm_min  = fromBcd(r[1] & 0x7F);
    utc->tm_hour = fromBcd(r[2] & 0x3F);
    utc->tm_mday = fromBcd(r[3] & 0x3F);
    utc->tm_mon  = fromBcd(r[5] & 0x1F) - 1;
    // Century bit in the month register: 0 = 20xx, 1 = 19xx.
    utc->tm_year = fromBcd(r[6]) + ((r[5] & 0x80) ? 1900 : 2000) - 1900;
    return true;
}

// --- Lifecycle --------------------------------------------------------------

bool rtcBegin() {
    Wire.begin(BOARD_SDA, BOARD_SCL);

    BusPower power;
    Wire.beginTransmission(kAddr);
    sAvailable = Wire.endTransmission() == 0;

    Serial.printf("[rtc] PCF8563 %s\n", sAvailable ? "ready" : "not responding");
    return sAvailable;
}

bool rtcAvailable() {
    return sAvailable;
}

// --- Time -------------------------------------------------------------------

bool rtcTimeValid() {
    if (!sAvailable) return false;

    BusPower power;
    struct tm t = {};
    bool vl = false;
    if (!readClock(&t, &vl)) return false;

    if (vl) {
        Serial.println("[rtc] chip reports its time is unreliable (VL set)");
        return false;
    }

    int year = t.tm_year + 1900;
    return year >= 2024 && year < 2100;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). Needed
// because this newlib has no timegm(), and mktime() would apply the timezone -
// wrong here, since the chip holds UTC.
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static time_t utcToEpoch(const struct tm &utc) {
    int64_t days = daysFromCivil(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    return (time_t)(days * 86400LL + utc.tm_hour * 3600LL + utc.tm_min * 60LL + utc.tm_sec);
}

bool rtcApplyToSystemClock() {
    // Apply the timezone whatever happens, so local time works even when the
    // clock itself has to come from the network later.
    setenv("TZ", settingsTimezone().c_str(), 1);
    tzset();

    if (!sAvailable) return false;

    BusPower power;
    struct tm utc = {};
    bool vl = false;
    if (!readClock(&utc, &vl) || vl) return false;

    int year = utc.tm_year + 1900;
    if (year < 2024 || year >= 2100) return false;

    struct timeval tv = {.tv_sec = utcToEpoch(utc), .tv_usec = 0};
    settimeofday(&tv, nullptr);

    Serial.printf("[rtc] system clock set from chip: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  year, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return true;
}

void rtcStoreSystemClock() {
    if (!sAvailable) return;

    BusPower power;

    time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);

    const uint8_t regs[7] = {
        toBcd(utc.tm_sec),  // writing this clears VL: the time is trusted again
        toBcd(utc.tm_min),
        toBcd(utc.tm_hour),
        toBcd(utc.tm_mday),
        (uint8_t)utc.tm_wday,
        toBcd(utc.tm_mon + 1),  // century bit 0 = 20xx
        toBcd((utc.tm_year + 1900) % 100),
    };

    bool wrote = writeRegs(kRegSeconds, regs, 7);

    // Read straight back, still inside the same powered block. A write that
    // silently does nothing is otherwise invisible until the alarm fails hours
    // later, which is exactly how this went unnoticed to begin with.
    struct tm back = {};
    bool vl = false;
    bool read = readClock(&back, &vl);
    bool verified = read && !vl && back.tm_year == utc.tm_year &&
                    back.tm_mon == utc.tm_mon && back.tm_mday == utc.tm_mday &&
                    back.tm_hour == utc.tm_hour && back.tm_min == utc.tm_min;

    Serial.printf("[rtc] set chip to %04d-%02d-%02d %02d:%02d:%02d UTC -> %s\n",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec,
                  !wrote ? "WRITE NAK" : verified ? "verified" : "DID NOT STICK");
}

// --- Alarm ------------------------------------------------------------------

void rtcSetDailyAlarmUtc(int utcHour, int utcMinute) {
    if (!sAvailable) return;

    BusPower power;

    // Day and weekday alarms disabled, so this matches every day.
    const uint8_t alarm[4] = {
        toBcd((uint8_t)utcMinute),
        toBcd((uint8_t)utcHour),
        kAlarmDisable,
        kAlarmDisable,
    };
    bool wrote = writeRegs(kRegAlarmMin, alarm, 4);

    // Enable the alarm interrupt (AIE) and clear any pending flag (AF). AIE is
    // what lets a match pull INT low, which is what wakes the board.
    uint8_t status2 = 0;
    readRegs(kRegStatus2, &status2, 1);
    status2 |= 0x02;   // AIE
    status2 &= ~0x08;  // AF
    wrote = writeRegs(kRegStatus2, &status2, 1) && wrote;

    // Verify rather than assume: an alarm that was never accepted looks exactly
    // like one that simply has not fired yet.
    uint8_t back[4] = {0};
    uint8_t status2Back = 0;
    readRegs(kRegAlarmMin, back, 4);
    readRegs(kRegStatus2, &status2Back, 1);

    bool verified = wrote && back[0] == alarm[0] && back[1] == alarm[1] &&
                    (status2Back & 0x02);

    Serial.printf("[rtc] daily alarm %02d:%02d UTC -> %s (AIE=%d, INT pin=%d)\n",
                  utcHour, utcMinute, verified ? "verified" : "NOT SET",
                  (status2Back >> 1) & 1, digitalRead(9));
}

void rtcSetAlarmInMinutes(int minutes) {
    if (!sAvailable) return;

    BusPower power;

    struct tm now = {};
    bool vl = false;
    if (!readClock(&now, &vl)) return;

    int total = now.tm_hour * 60 + now.tm_min + minutes;
    rtcSetDailyAlarmUtc((total / 60) % 24, total % 60);
}

bool rtcAlarmFired() {
    if (!sAvailable) return false;

    BusPower power;

    uint8_t status2 = 0;
    if (!readRegs(kRegStatus2, &status2, 1)) return false;
    return (status2 & 0x08) != 0;  // AF
}

void rtcLogAlarmState() {
    if (!sAvailable) return;

    BusPower power;

    uint8_t status2 = 0, alarm[4] = {0};
    readRegs(kRegStatus2, &status2, 1);
    readRegs(kRegAlarmMin, alarm, 4);

    struct tm now = {};
    bool vl = false;
    readClock(&now, &vl);

    Serial.printf("[rtc] chip %04d-%02d-%02d %02d:%02d:%02d (VL=%d) | AIE=%d AF=%d | "
                  "alarm %02d:%02d | INT pin=%d\n",
                  now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                  now.tm_hour, now.tm_min, now.tm_sec, vl,
                  (status2 >> 1) & 1, (status2 >> 3) & 1,
                  fromBcd(alarm[1] & 0x3F), fromBcd(alarm[0] & 0x7F),
                  digitalRead(9));
}

void rtcClearAlarm() {
    if (!sAvailable) return;

    BusPower power;

    uint8_t status2 = 0;
    if (!readRegs(kRegStatus2, &status2, 1)) return;
    status2 &= ~0x08;  // AF - releases INT back high
    writeRegs(kRegStatus2, &status2, 1);
}
