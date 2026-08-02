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
#define BLE_DEVICE_NAME "Quotidian"

// ---------------------------------------------------------------------------
// Content
// ---------------------------------------------------------------------------

// Per-day quote files in internal flash, written by `npm run export:device`
// and flashed with `pio run -t uploadfs`. The device opens /quotes/MM-DD.tsv
// for today's date.
#define QUOTE_DIR "/quotes"

// Manual override, consulted only when the day file is unavailable. Line 1 is
// the quote, line 2 the author, line 3 (optional) the dates.
#define QUOTE_FILE "/quote.txt"

// The battery indicator stays hidden until the charge drops to this level. On
// something meant to be looked at rather than monitored, a permanent gauge is
// noise; a warning is information.
#define LOW_BATTERY_PERCENT 10

// Draws a line of clock/alarm state at the foot of the quote screen: local
// time, timezone, the RTC chip's own UTC time, the armed alarm and its flags.
//
// Off by default - it is debug text on a display meant to be looked at. Turn it
// on when the clock or the nightly wake needs investigating. It lives on the
// panel rather than in the serial log because the serial port cannot be opened
// on this board without resetting the chip.
#define SHOW_CLOCK_DIAGNOSTICS 0

// Draws two lines of power accounting at the foot of the quote screen: how many
// times the board has woken and what woke it, then the battery now against the
// battery when counting started, with the fall expressed in mV per day.
//
// This is the only way to measure a device that is asleep 99.99% of the time
// and cannot be probed over USB without resetting it. Read it off the panel on
// consecutive mornings: mV/day gives the average draw, and a non-zero `timer`
// count means the RTC alarm is failing and the backstop is carrying it.
//
// On while the week-long battery drain is being chased. Turn it off afterwards.
#define SHOW_POWER_DIAGNOSTICS 1

// ---------------------------------------------------------------------------
// Sleep schedule
// ---------------------------------------------------------------------------

// Development switch.
//
//   1 - normal: wake nightly, draw, deep sleep. Weeks of battery life, but USB
//       disappears with the chip, so reflashing needs a button press first.
//   0 - stay awake after the update. USB stays enumerated, so you can reflash
//       whenever you like. Costs roughly 100 mA - fine on USB, hopeless on a
//       battery.
//
// Set back to 1 before running on battery. Every build prints a reminder while
// this is 0.
#define DEEP_SLEEP_ENABLED 1

#if !DEEP_SLEEP_ENABLED
#pragma message("*** DEEP_SLEEP_ENABLED = 0 - development mode, device will NOT sleep ***")
#endif

// Shortens the wake cycle to this many seconds so the sleep and wake path can
// be exercised without waiting for midnight. 0 uses the real nightly schedule.
//
// Keep it short in both directions: the backstop timer is what recovers the
// board if the alarm fails, and a long one locks you out of USB until it
// expires. That happened during development with a 62-minute backstop.
#define TEST_WAKE_SECONDS 0

#if TEST_WAKE_SECONDS
#pragma message("*** TEST_WAKE_SECONDS set - short wake cycle, not the nightly schedule ***")
#endif

// Local time of the nightly update. A margin past midnight costs nothing and
// absorbs both clock drift and the hour that a DST change shifts the alarm by;
// waking before midnight would otherwise mean rendering yesterday again.
//
// The margin is belt-and-braces, though: correctness comes from reading the
// clock after waking, never from trusting the moment we woke up.
#define WAKE_HOUR   0
#define WAKE_MINUTE 10

// How often to correct the hardware clock over Wi-Fi. The PCF8563 drifts a few
// seconds a day, so a week is already far tighter than a date display needs -
// and a sync costs roughly 0.3 mAh against a daily budget of about 9 mAh.
// Timezone and DST are handled locally and need no network.
#define NTP_SYNC_INTERVAL_DAYS 7

// When the clock is unknown there is no calendar day to wake up for, so the
// device retries on this interval instead of guessing at a nightly schedule.
#define SLEEP_RETRY_MINUTES 15

// Setup mode has to stay awake to serve the portal, which is ruinous on
// battery. If nobody completes provisioning within this window, sleep and try
// again at the next alarm. The button wakes it straight back into setup.
#define SETUP_TIMEOUT_MS (15UL * 60UL * 1000UL)

