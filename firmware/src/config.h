#pragma once

// ---------------------------------------------------------------------------
// Wi-Fi. Leave the SSID empty to skip networking entirely; the sketch then
// falls back to the build date instead of NTP time.
// ---------------------------------------------------------------------------
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// How long to wait for an association before giving up (ms).
#define WIFI_TIMEOUT_MS 15000

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
#define NTP_SERVER "pool.ntp.org"

// POSIX TZ string. This one is Europe/Amsterdam incl. DST rules.
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"

// ---------------------------------------------------------------------------
// Bluetooth Low Energy. The device advertises under this name and exposes the
// standard Battery Service (0x180F), so any BLE scanner app can read the
// charge level.
// ---------------------------------------------------------------------------
#define BLE_DEVICE_NAME "Quote of the Day"

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

// Plain-text file on the SD card. Line 1 is the quote, line 2 the author,
// line 3 (optional) the source. Missing card or file -> built-in fallback.
#define QUOTE_FILE "/quote.txt"

// Full-screen redraws are slow and wear the panel, so keep this generous.
#define REFRESH_INTERVAL_MS (10UL * 60UL * 1000UL)
