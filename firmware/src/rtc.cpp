#include "rtc.h"

#include <SensorPCF8563.hpp>
#include <Wire.h>
#include <sys/time.h>

#include "config.h"
#include "settings.h"
#include "utilities.h"

static SensorPCF8563 sRtc;
static bool sAvailable = false;

bool rtcBegin() {
    Wire.begin(BOARD_SDA, BOARD_SCL);

    Wire.beginTransmission(PCF8563_SLAVE_ADDRESS);
    if (Wire.endTransmission() != 0) {
        Serial.println("[rtc] PCF8563 not responding");
        sAvailable = false;
        return false;
    }

    sAvailable = sRtc.init(Wire, BOARD_SDA, BOARD_SCL, PCF8563_SLAVE_ADDRESS);
    Serial.printf("[rtc] PCF8563 %s\n", sAvailable ? "ready" : "init failed");
    return sAvailable;
}

bool rtcAvailable() {
    return sAvailable;
}

bool rtcTimeValid() {
    if (!sAvailable) return false;

    RTC_DateTime now = sRtc.getDateTime();
    // A chip that lost power reads back a default far outside any plausible
    // range, so a year sanity check is enough to catch it.
    return now.year >= 2024 && now.year < 2100;
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

// Applies the timezone without starting SNTP. configTzTime() would do both,
// but here the clock comes from hardware, not the network.
static void applyTimezone() {
    setenv("TZ", settingsTimezone().c_str(), 1);
    tzset();
}

bool rtcApplyToSystemClock() {
    applyTimezone();

    if (!rtcTimeValid()) {
        Serial.println("[rtc] no valid time held");
        return false;
    }

    RTC_DateTime now = sRtc.getDateTime();

    struct tm utc = {};
    utc.tm_year = now.year - 1900;
    utc.tm_mon  = now.month - 1;
    utc.tm_mday = now.day;
    utc.tm_hour = now.hour;
    utc.tm_min  = now.minute;
    utc.tm_sec  = now.second;

    time_t epoch = utcToEpoch(utc);
    struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
    settimeofday(&tv, nullptr);

    Serial.printf("[rtc] system clock set from chip: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  now.year, now.month, now.day, now.hour, now.minute, now.second);
    return true;
}

void rtcStoreSystemClock() {
    if (!sAvailable) return;

    time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);

    sRtc.setDateTime(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                     utc.tm_hour, utc.tm_min, utc.tm_sec);

    Serial.printf("[rtc] chip set from NTP: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
}

void rtcSetDailyAlarmUtc(int utcHour, int utcMinute) {
    if (!sAvailable) return;

    // Day and weekday left unmatched, so the alarm repeats every day.
    sRtc.setAlarm(utcHour, utcMinute, PCF8563_NO_ALARM, PCF8563_NO_ALARM);
    sRtc.enableAlarm();

    Serial.printf("[rtc] daily alarm armed for %02d:%02d UTC\n", utcHour, utcMinute);
}

void rtcLogAlarmState() {
    if (!sAvailable) return;

    // 0x01 = control/status 2 (AIE bit 1, AF bit 3), 0x09..0x0C = alarm regs.
    Wire.beginTransmission(PCF8563_SLAVE_ADDRESS);
    Wire.write(0x01);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)PCF8563_SLAVE_ADDRESS, (uint8_t)1);
    uint8_t stat2 = Wire.available() ? Wire.read() : 0xFF;

    Wire.beginTransmission(PCF8563_SLAVE_ADDRESS);
    Wire.write(0x09);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)PCF8563_SLAVE_ADDRESS, (uint8_t)4);
    uint8_t a[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    for (int i = 0; i < 4 && Wire.available(); i++) a[i] = Wire.read();

    Serial.printf("[rtc] stat2=0x%02X (AIE=%d AF=%d) alarm min=0x%02X hr=0x%02X "
                  "day=0x%02X wd=0x%02X, INT pin=%d\n",
                  stat2, (stat2 >> 1) & 1, (stat2 >> 3) & 1,
                  a[0], a[1], a[2], a[3], digitalRead(9));
}

void rtcClearAlarm() {
    if (!sAvailable) return;
    sRtc.resetAlarm();
}
