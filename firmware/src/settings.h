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

// Wipes credentials and timezone, returning the device to provisioning.
void settingsClear();
