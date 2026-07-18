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
// This lives in NVS rather than RTC memory. RTC_DATA_ATTR would be the obvious
// choice for deep sleep, but this board does not deep sleep: the RTC alarm
// power-cycles it, so the reset reason is POWERON and RTC memory is gone. The
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

// Wipes credentials and timezone, returning the device to provisioning.
void settingsClear();
