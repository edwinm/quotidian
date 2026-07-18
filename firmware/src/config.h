#pragma once

// ---------------------------------------------------------------------------
// Wi-Fi credentials are NOT configured here. They are provisioned at runtime -
// over USB with Improv, or through the captive portal - and stored in NVS.
// See firmware/README.md.
// ---------------------------------------------------------------------------

// How long to wait for an association before giving up (ms).
#define WIFI_TIMEOUT_MS 15000

// Hold the front button this long to erase credentials and return to setup.
#define RESET_HOLD_MS 3000

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
#define NTP_SERVER "pool.ntp.org"

// Used until provisioning supplies the real one. The captive portal derives an
// exact POSIX TZ from the browser; Improv has no channel for it, so devices set
// up over USB keep this default until the portal is used.
#define DEFAULT_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
#define FIRMWARE_VERSION "1.0.0"

// Advertised over BLE, and reported to Improv clients.
#define BLE_DEVICE_NAME "Quote of the Day"

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

// Plain-text file on the SD card. Line 1 is the quote, line 2 the author,
// line 3 (optional) the source. Missing card or file -> built-in fallback.
#define QUOTE_FILE "/quote.txt"

// Full-screen redraws are slow and wear the panel, so keep this generous.
#define REFRESH_INTERVAL_MS (10UL * 60UL * 1000UL)
