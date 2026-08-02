#pragma once

#include <Arduino.h>

// Persistent configuration, stored in NVS (survives reflashing unless the
// flash is fully erased). Credentials never live in source control.

void settingsBegin();

bool settingsHasCredentials();

String settingsSsid();
String settingsPassword();

// POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3". Falls back to
// DEFAULT_TIMEZONE from config.h when provisioning never supplied one.
String settingsTimezone();

// IANA name, e.g. "Europe/Amsterdam". Display only; empty when unknown.
String settingsTimezoneName();

void settingsSaveCredentials(const String &ssid, const String &password);

// Both may be empty, in which case the stored timezone is left alone.
void settingsSaveTimezone(const String &posix, const String &ianaName);

// State that has to survive between updates.
//
// This lives in NVS rather than RTC memory. RTC_DATA_ATTR would survive an
// ordinary deep-sleep wake, but not the power cuts, resets and reflashes this
// board sees in normal use, and losing the counter silently is expensive: the
// give-away was `NTP sync due (9999 days since last)` on every single boot -
// 9999 being the initial value of the counter.
//
// One write a day against NVS wear levelling is nothing.
int  settingsDaysSinceSync();
void settingsSetDaysSinceSync(int days);

// "YYYY-MM-DD" of the last day actually rendered, so a wake that arrives before
// the date rolls over does not redraw yesterday.
String settingsLastRendered();
void   settingsSetLastRendered(const String &day);

// Wake accounting. Diagnostic only: it exists to answer two questions that
// cannot be answered from a display showing yesterday's quote - how often the
// board really woke, and how fast the battery actually fell.
//
// `byTimer` is the important one. It counts wakes the RTC alarm failed to
// deliver, which the backstop had to cover; on a healthy device it stays 0.
//
// The baseline (firstVolts/firstEpoch) is what turns the counters into a rate.
// It re-arms by itself when the battery is found charged, since a drain
// measured across a top-up means nothing.
struct WakeStats {
    uint32_t total;
    uint32_t byAlarm;
    uint32_t byTimer;
    uint32_t byButton;
    uint32_t byOther;     // power-on, reset, brownout
    float    firstVolts;  // battery when this baseline started, 0 if unset
    int64_t  firstEpoch;  // UTC seconds at that moment, 0 if the clock was unknown
};

WakeStats settingsWakeStats();
void      settingsSaveWakeStats(const WakeStats &stats);

// Wipes credentials and timezone, returning the device to provisioning.
void settingsClear();
