#pragma once

#include <Arduino.h>

// Connects to `ssid` and, on success, syncs the clock over NTP using the
// stored timezone. Returns false on timeout or bad credentials.
//
// Call batteryRead() *before* this: the battery ADC channel is shared with the
// Wi-Fi radio (see battery.h).
bool wifiConnect(const String &ssid, const String &password);

// Reconnects using the credentials in NVS.
bool wifiBegin();

bool wifiConnected();

// Drops the association and powers the radio down, which hands ADC2 back so
// the battery can be sampled again.
void wifiStop();

// SSID when connected, otherwise "offline".
String wifiDescription();

// True once the clock has been set from NTP.
bool timeSynced();

// "Saturday 18 July 2026", from the RTC. Falls back to the build date when the
// clock was never synced.
String todayLong();

// Starts BLE advertising with the standard Battery Service (0x180F).
void bleBegin();

bool bleActive();

// Publishes a new level to connected clients. `percent` < 0 is ignored.
void bleSetBatteryLevel(int percent);
