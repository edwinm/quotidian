/**
 * Quote of the Day - LilyGo T5 4.7" e-paper (ESP32-S3), non-touch version.
 *
 * Portrait orientation: the 960x540 panel is driven through a 540x960 canvas
 * rotated a quarter turn counter-clockwise (see ui.h).
 *
 * Wi-Fi is provisioned at runtime by either route:
 *   - Improv over USB serial, from a Chromium browser;
 *   - a captive portal, joined by scanning the QR code on the screen.
 * Holding the front button for 3 s erases the credentials and starts over.
 */

#ifndef BOARD_HAS_PSRAM
#error "Enable PSRAM (OPI) - the 960x540 framebuffer does not fit in internal RAM"
#endif

#include <Arduino.h>
#include <Button2.h>

#include "battery.h"
#include "config.h"
#include "improv.h"
#include "power.h"
#include "portal.h"
#include "rtc.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"
#include "utilities.h"
#include "wireless.h"

// --- Layout -----------------------------------------------------------------

static constexpr int kMargin = 36;
static constexpr int kGutter = 28;  // indent past the accent bar
static constexpr int kTextX  = kMargin + kGutter;
static constexpr int kContentRight = UI_WIDTH - kMargin;
static constexpr int kColumnWidth  = kContentRight - kTextX;

// Shown when there is no SD card, or no readable quote file on it.
static const Quote kFallbackQuote = {
    "I think, therefore I am",
    "René Descartes",
    "31 March 1596 - ",   // prefix
    "11 February",        // bold: the run that matches today
    " 1650",              // suffix
    "Wikiquote · CC BY-SA 4.0",
};

enum Mode {
    MODE_SETUP,    // no usable credentials; portal + Improv both listening
    MODE_RUNNING,  // connected (or resigned to offline), showing the quote
};

static Mode sMode = MODE_SETUP;
static Quote sQuote;
static BatteryStatus sBattery;
static uint32_t sNextRefresh = 0;
static Button2 sButton;

// Set from the portal callback; acted on in loop() so the HTTP response has
// already been flushed before the radio is retuned.
static volatile bool sPendingCredentials = false;
static String sPendingSsid;
static String sPendingPassword;

// Populated when a connection attempt fails, so the setup screen can say why.
static String sSetupError;

// Whether the PCF8563 alarm is what switched the board on, as opposed to a
// hand-pressed reset. Read from the chip's alarm flag at boot, because the
// board power-cycles rather than waking and the reset reason cannot tell them
// apart. Reported again before powering down: the boot line is usually lost
// while USB CDC enumerates.
static bool sWokeByAlarm = false;

// --- Quote screen -----------------------------------------------------------

static constexpr int kQuoteTop    = 140;
static constexpr int kQuoteBottom = 800;
static constexpr int kQuoteLead   = 46;  // leading for Font::Large (42 px box)

static void drawQuote() {
    std::vector<String> lines = uiWrapText(Font::Large, sQuote.text, kColumnWidth);

    // Measure the whole block first so it can be optically centred.
    const int bodyHeight   = lines.size() * kQuoteLead;
    const int authorGap    = 42;
    const int authorHeight = uiLineHeight(Font::BodyBold);
    const int sourceGap    = 14;

    const bool hasDates = sQuote.datesBold.length() || sQuote.datesPrefix.length() ||
                          sQuote.datesSuffix.length();
    const int datesHeight = hasDates ? sourceGap + uiLineHeight(Font::Small) : 0;

    const int total = bodyHeight + authorGap + authorHeight + datesHeight;

    int top = kQuoteTop + ((kQuoteBottom - kQuoteTop) - total) / 2;
    if (top < kQuoteTop) top = kQuoteTop;

    // The accent bar spans the quotation only, not the attribution.
    uiDrawAccentBar(kMargin, top, 4, bodyHeight);

    int y = top + uiAscender(Font::Large);
    for (const String &line : lines) {
        uiDrawText(Font::Large, kTextX, y, line.c_str(), ink::kTextBlack);
        y += kQuoteLead;
    }

    y = top + bodyHeight + authorGap + uiAscender(Font::BodyBold);
    String author = sQuote.author.length() ? sQuote.author : String("Unknown");
    uiDrawText(Font::BodyBold, kTextX, y,
               uiEllipsize(Font::BodyBold, author, kColumnWidth).c_str(),
               ink::kTextBlack);

    // Dates, drawn as three runs on one baseline. The middle run is the day and
    // month that match today, set bold and black so the link to the date in the
    // header is visible at a glance; the years stay regular and grey.
    if (hasDates) {
        y += (authorHeight - uiAscender(Font::BodyBold)) + sourceGap +
             uiAscender(Font::Small);

        String suffix = sQuote.datesSuffix;
        int used = uiTextWidth(Font::Small, sQuote.datesPrefix.c_str()) +
                   uiTextWidth(Font::SmallBold, sQuote.datesBold.c_str());
        if (used + uiTextWidth(Font::Small, suffix.c_str()) > kColumnWidth) {
            suffix = uiEllipsize(Font::Small, suffix, kColumnWidth - used);
        }

        int x = kTextX;
        x = uiDrawText(Font::Small, x, y, sQuote.datesPrefix.c_str(), ink::kTextMid);
        x = uiDrawText(Font::SmallBold, x, y, sQuote.datesBold.c_str(), ink::kTextBlack);
        uiDrawText(Font::Small, x, y, suffix.c_str(), ink::kTextMid);

        y += uiLineHeight(Font::Small);
    }
}

// The foot of the page carries the credit the licence requires, and nothing
// else - no Wi-Fi, SD or Bluetooth status. This is a thing to read, not a
// dashboard, and it is going in a picture frame.
//
// The battery appears only once it is nearly flat, so it reads as a warning
// rather than as decoration.
static void drawFooter() {
    const int baseline = 910;

    if (sQuote.attribution.length()) {
        uiDrawText(Font::Small, kMargin, baseline,
                   uiEllipsize(Font::Small, sQuote.attribution, kColumnWidth).c_str(),
                   ink::kTextLight);
    }

    if (!sBattery.present || sBattery.percent > LOW_BATTERY_PERCENT) return;

    const int iconW = 44;
    const int iconX = kContentRight - iconW;
    uiDrawBattery(iconX, baseline - 15, sBattery.percent);

    String label = String(sBattery.percent) + "%";
    uiDrawTextRight(Font::SmallBold, iconX - 10, baseline, label.c_str(), ink::kTextBlack);
}

static void renderQuoteScreen() {
    uiClearBuffer();

    uiDrawText(Font::SmallBold, kMargin, 64, "QUOTE OF THE DAY", ink::kTextMid);
    uiDrawTextRight(Font::Small, kContentRight, 64,
                    uiEllipsize(Font::Small, todayLong(), 260).c_str(), ink::kTextMid);
    uiDrawRule(kMargin, 86, kContentRight - kMargin, ink::kLight);

    drawQuote();
    drawFooter();
    uiFlush();
}

// --- Setup screen -----------------------------------------------------------

// One column, top to bottom: title, QR, the two routes, then device details.
// Portrait gives enough height that nothing has to share a row.
static void renderSetupScreen() {
    uiClearBuffer();

    uiDrawText(Font::Title, kMargin, 68, "Set up Wi-Fi", ink::kTextBlack);
    uiDrawText(Font::Small, kMargin, 100, "Quote of the Day", ink::kTextMid);
    uiDrawRule(kMargin, 120, kContentRight - kMargin, ink::kLight);

    // --- QR, centred. 6 px per module scans comfortably at arm's length ---
    const int qrScale = 6;
    const int qrSide = uiQrSize(portalQrPayload().c_str(), qrScale);
    uiDrawQr((UI_WIDTH - qrSide) / 2, 154, portalQrPayload().c_str(), qrScale);

    // --- The two routes ---
    struct Option {
        int         titleY;
        const char *title;
        const char *lines[2];
    };
    static const Option kOptions[] = {
        {478, "With a phone",    {"Scan the code to connect.", nullptr}},
        {580, "With a computer", {"Open improv-wifi.com", "in Chrome or Edge."}},
    };

    for (const Option &opt : kOptions) {
        int y = opt.titleY;
        uiDrawText(Font::BodyBold, kTextX, y, opt.title, ink::kTextBlack);

        for (const char *line : opt.lines) {
            if (!line) break;
            y += 36;
            uiDrawText(Font::Body, kTextX, y, line, ink::kTextDark);
        }

        const int barTop = opt.titleY - uiAscender(Font::BodyBold);
        const int barBottom = y + uiDescender(Font::Body);
        uiDrawAccentBar(kMargin, barTop, 4, barBottom - barTop);
    }

    // --- Details and hint ---
    uiDrawRule(kMargin, 790, kContentRight - kMargin, ink::kLight);

    const int valueX = kMargin + 96;
    uiDrawText(Font::SmallBold, kMargin, 822, "Network", ink::kTextMid);
    uiDrawText(Font::Small, valueX, 822,
               uiEllipsize(Font::Small, portalSsid(), kContentRight - valueX).c_str(),
               ink::kTextBlack);

    uiDrawText(Font::SmallBold, kMargin, 850, "Key", ink::kTextMid);
    uiDrawText(Font::Small, valueX, 850, portalPassword().c_str(), ink::kTextBlack);

    if (sSetupError.length()) {
        int y = 902;
        for (const String &line :
             uiWrapText(Font::Small, sSetupError, kContentRight - kMargin)) {
            if (y > 950) break;
            uiDrawText(Font::Small, kMargin, y, line.c_str(), ink::kTextBlack);
            y += uiLineHeight(Font::Small);
        }
    } else {
        uiDrawText(Font::Small, kMargin, 912, "Hold the button 3 s to start over.",
                   ink::kTextMid);
    }

    uiFlush();
}

static void renderMessage(const char *title, const char *detail) {
    uiClearBuffer();

    int y = UI_HEIGHT / 2 - 40;
    const int barTop = y - uiAscender(Font::Title);

    uiDrawText(Font::Title, kTextX, y,
               uiEllipsize(Font::Title, title, kColumnWidth).c_str(), ink::kTextBlack);

    int barBottom = y + uiDescender(Font::Title);
    if (detail) {
        for (const String &line : uiWrapText(Font::Body, detail, kColumnWidth)) {
            y += 44;
            uiDrawText(Font::Body, kTextX, y, line.c_str(), ink::kTextDark);
            barBottom = y + uiDescender(Font::Body);
        }
    }
    uiDrawAccentBar(kMargin, barTop, 4, barBottom - barTop);

    uiFlush();
}

// --- Content ----------------------------------------------------------------

// Today's quote comes from the day file matching the calendar date, so the
// author's birth or death day is always today. Without a valid clock the day
// is genuinely unknown, and picking one would be a guess - so the built-in
// quote is used instead.
static void loadQuote() {
    int year, month, day;
    if (todayParts(&year, &month, &day) &&
        storageReadQuoteForDay(month, day, year, sQuote)) {
        return;
    }

    if (storageReadOverrideQuote(sQuote)) return;

    sQuote = kFallbackQuote;
    Serial.println("[content] using built-in fallback quote");
}

// --- Sleep scheduling -------------------------------------------------------

// Both of these used to live in RTC memory, which was wrong: the RTC alarm
// power-cycles this board rather than waking it from deep sleep, so RTC memory
// does not survive. They are in NVS now - see settings.h.

static String todayKey() {
    int y, m, d;
    if (!todayParts(&y, &m, &d)) return String("");
    char buf[12];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return String(buf);
}

// Seconds from now until the next local WAKE_HOUR:WAKE_MINUTE.
static long secondsUntilNextWake() {
    if (TEST_WAKE_SECONDS) return TEST_WAKE_SECONDS;

    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);

    struct tm target = lt;
    target.tm_hour = WAKE_HOUR;
    target.tm_min  = WAKE_MINUTE;
    target.tm_sec  = 0;

    time_t at = mktime(&target);
    if (at <= now) at += 24 * 60 * 60;
    return (long)(at - now);
}

// Arms both wake sources, then sleeps.
//
// The PCF8563 alarm is the accurate one and normally fires first. The ESP32's
// own timer is set an hour later purely as a backstop: its RC oscillator is
// minutes-per-day inaccurate, so it is no good for scheduling, but it does
// guarantee the device still wakes if the alarm never arrives.
// True once the cycle has finished and the device is deliberately staying up,
// which only happens with DEEP_SLEEP_ENABLED == 0.
static bool sStayingAwake = false;

// Ends a wake cycle. Sleeps normally; in development mode it returns instead,
// leaving USB enumerated so the board can be reflashed without a button press.
static bool endCycleAwake(long seconds) {
    if (DEEP_SLEEP_ENABLED) return false;

    Serial.printf("[power] DEEP SLEEP DISABLED - staying awake "
                  "(would have slept %ld s). Short-press the button to redraw.\n",
                  seconds);
    sStayingAwake = true;
    return true;
}

static void sleepUntilNextWake() {
    // With no valid clock there is no calendar day to aim at. Retry soon rather
    // than arming a nightly schedule against a time we do not have - the first
    // version armed nothing at all in this case and slept straight through.
    if (!timeSynced()) {
        if (endCycleAwake(SLEEP_RETRY_MINUTES * 60L)) return;

        // The absolute time is not trustworthy, but the chip still counts, so a
        // relative alarm still brings the board back. There is no timer to fall
        // back on.
        Serial.printf("[power] clock unknown, retrying in %d min\n", SLEEP_RETRY_MINUTES);
        rtcSetAlarmInMinutes(SLEEP_RETRY_MINUTES);
        powerDown();
    }

    long seconds = secondsUntilNextWake();
    if (endCycleAwake(seconds)) return;

    time_t at = time(nullptr) + seconds;
    struct tm utc;
    gmtime_r(&at, &utc);
    rtcSetDailyAlarmUtc(utc.tm_hour, utc.tm_min);
    rtcLogAlarmState();

    Serial.printf("[power] started by %s; next update in %ld s (%.1f h)\n",
                  sWokeByAlarm ? "RTC alarm" : "power-on/reset",
                  seconds, seconds / 3600.0);

    powerDown();
}

// Woke before the date rolled over - too early, or the alarm shifted an hour
// across a DST change. Nothing to draw yet; come back at the real target.
// Woke before the date rolled over - too early, or the alarm shifted an hour
// across a DST change. Nothing to draw yet, so re-arm for the real target. This
// is just sleepUntilNextWake(): with no timer left, re-arming the alarm is the
// only mechanism there is, and it is the correct one.
static void sleepUntilDateRolls() {
    Serial.println("[power] started early, date has not rolled - re-arming");
    sleepUntilNextWake();
}

// --- The nightly update -----------------------------------------------------

static void runDailyUpdate() {
    // Correct the hardware clock only when it is due, or when it holds nothing
    // usable. Wi-Fi is the single most expensive thing this device does.
    const int daysSinceSync = settingsDaysSinceSync();
    bool needSync = !timeSynced() || daysSinceSync >= NTP_SYNC_INTERVAL_DAYS;

    if (needSync) {
        Serial.printf("[power] NTP sync due (%d days since last)\n", daysSinceSync);
        if (wifiBegin() && timeSynced()) {
            settingsSetDaysSinceSync(0);
        }
        wifiStop();  // hands ADC2 back and drops the radio
    } else {
        settingsSetDaysSinceSync(daysSinceSync + 1);
        Serial.printf("[power] clock from RTC, %d days since NTP\n", daysSinceSync + 1);
    }

    if (!timeSynced()) {
        // No clock at all: render the fallback rather than a wrong day.
        Serial.println("[power] no valid clock, showing fallback");
    }

    storageBegin();
    loadQuote();
    renderQuoteScreen();

    String key = todayKey();
    if (key.length()) settingsSetLastRendered(key);
}

// --- Mode transitions -------------------------------------------------------

static void enterSetupMode() {
    sMode = MODE_SETUP;

    // BLE only runs here. A device that sleeps 24 hours a day cannot advertise
    // meaningfully, and the radio is far too expensive to hold up for it - so
    // the Battery Service is available while provisioning and not otherwise.
    if (!bleActive()) bleBegin();
    bleSetBatteryLevel(sBattery.percent);

    improvSetProvisioned(false);
    portalBegin([](const String &ssid, const String &password,
                   const String &posixTz, const String &ianaTz) {
        settingsSaveTimezone(posixTz, ianaTz);
        sPendingSsid = ssid;
        sPendingPassword = password;
        sPendingCredentials = true;
    });

    renderSetupScreen();
}

// Improv connects inline and reports the outcome through its own protocol.
// Unlike the portal this is safe: the USB link is unaffected by retuning Wi-Fi.
static bool onImprovCredentials(const String &ssid, const String &password) {
    renderMessage("Connecting", ssid.c_str());

    portalStop();
    if (!wifiConnect(ssid, password)) {
        sSetupError = "Could not connect to " + ssid + ". Check the password.";
        enterSetupMode();
        return false;
    }

    settingsSaveCredentials(ssid, password);
    sMode = MODE_RUNNING;
    return true;
}

static void applyPendingCredentials() {
    sPendingCredentials = false;

    renderMessage("Connecting", sPendingSsid.c_str());
    portalStop();

    if (!wifiConnect(sPendingSsid, sPendingPassword)) {
        sSetupError = "Could not connect to " + sPendingSsid + ". Check the password.";
        enterSetupMode();
        return;
    }

    settingsSaveCredentials(sPendingSsid, sPendingPassword);
    sMode = MODE_RUNNING;
}

static void onShortPress(Button2 &btn) {
    (void)btn;
    Serial.println("[button] short press - redrawing");
    runDailyUpdate();
}

static void forgetWifi() {
    settingsClear();
    renderMessage("Wi-Fi forgotten", "Restarting for setup...");
    delay(1500);
    ESP.restart();
}

static void onLongPress(Button2 &btn) {
    (void)btn;
    Serial.println("[button] long press - clearing Wi-Fi credentials");
    forgetWifi();
}

// Held down at startup, the button forgets the network.
//
// The long press above only works while the board happens to be awake, which
// in normal operation is a few seconds a night - so it is not a gesture anyone
// can actually perform. This one is: hold the button, tap reset. Nothing else
// on this board uses GPIO21, and the check runs before Button2 claims it.
static bool buttonHeldAtBoot() {
    pinMode(BUTTON_1, INPUT_PULLUP);
    delay(20);  // let the pull-up settle

    // Sample rather than read once: a single reading catches any glitch on the
    // line as an intentional press.
    for (int i = 0; i < 10; i++) {
        if (digitalRead(BUTTON_1) != LOW) return false;
        delay(20);
    }
    return true;
}

// --- Setup mode -------------------------------------------------------------

// Provisioning needs the device awake and serving, which no battery enjoys.
// It runs as a bounded window rather than a state the device can be left in.
static void runSetupMode() {
    enterSetupMode();

    // The window is measured from the last sign of life, not from when setup
    // started. Provisioning takes as long as it takes - reading a QR, typing a
    // password, retrying a typo - and having the board switch off mid-attempt
    // is the one thing it must not do.
    //
    // In development mode the window never closes; there is nothing useful to
    // sleep for, and recursing to restart the loop would eventually eat stack.
    uint32_t lastActivity = millis();
    while (!DEEP_SLEEP_ENABLED ||
           (int32_t)(millis() - (lastActivity + SETUP_TIMEOUT_MS)) < 0) {
        sButton.loop();
        improvLoop();
        portalLoop();

        // Any packet from a browser, or any page served, means somebody is
        // standing there.
        uint32_t seen = max(improvLastActivityMs(), portalLastActivityMs());
        if (seen > lastActivity) lastActivity = seen;

        if (sPendingCredentials) {
            applyPendingCredentials();
            lastActivity = millis();  // on failure, another full window
        }

        // Either route may have succeeded; both land here.
        if (sMode == MODE_RUNNING) {
            portalStop();
            runDailyUpdate();
            sleepUntilNextWake();
        }
        delay(5);
    }

    Serial.printf("[power] setup idle for %lu min, powering down\n",
                  SETUP_TIMEOUT_MS / 60000UL);
    portalStop();
    renderMessage("Setup paused", "Press the button to try again.");
    sleepUntilNextWake();
}

// --- Lifecycle --------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(200);

    powerLogResetReason();

    if (!uiBegin()) {
        while (true) {
            Serial.println("[boot] halted: no framebuffer");
            delay(5000);
        }
    }

    settingsBegin();

    // The hardware clock carries the time across sleep and power loss, so the
    // system clock and timezone come from it before anything else needs a date.
    rtcBegin();
    rtcApplyToSystemClock();

    // Must be read before clearing: the flag is the only evidence of what
    // started this cycle.
    sWokeByAlarm = rtcAlarmFired();
    rtcClearAlarm();
    Serial.printf("[boot] Quote of the Day - started by %s\n",
                  sWokeByAlarm ? "RTC alarm" : "power-on/reset");

    batteryBegin();

    // Sample while the radios are off - the sense pin is on ADC2, which Wi-Fi
    // takes over once it starts.
    sBattery = batteryRead();
    Serial.printf("[battery] %.2f V (%d%%)%s\n", sBattery.volts, sBattery.percent,
                  sBattery.present ? "" : " - no battery, running off USB");

    if (buttonHeldAtBoot()) {
        Serial.println("[button] held at startup - clearing Wi-Fi credentials");
        forgetWifi();  // does not return
    }

    sButton.begin(BUTTON_1);
    sButton.setLongClickTime(RESET_HOLD_MS);
    sButton.setLongClickDetectedHandler(onLongPress);
    sButton.setClickHandler(onShortPress);

    improvBegin(onImprovCredentials);

    if (!settingsHasCredentials()) {
        runSetupMode();
        return;  // sleeps, or falls through to loop() in development mode
    }

    // An alarm that fires before the date has rolled would redraw yesterday, so
    // this compares the calendar day rather than trusting the wake instant.
    //
    // It applies ONLY to alarm wakes. Without that condition it fires on every
    // boot of a day already rendered - so a reset or a button press would put
    // the board straight back to sleep without drawing anything, which is
    // exactly what it did.
    //
    // The short test cycle skips it too: on a 3-minute loop the date never
    // rolls, and the guard would suppress every render.
    String lastRendered = settingsLastRendered();
    if (!TEST_WAKE_SECONDS && sWokeByAlarm && lastRendered.length() &&
        todayKey() == lastRendered) {
        sleepUntilDateRolls();
        return;  // sleeps, or falls through to loop() in development mode
    }

    sMode = MODE_RUNNING;
    runDailyUpdate();

    sleepUntilNextWake();
}

// Only runs with DEEP_SLEEP_ENABLED == 0. In normal operation setup() ends in
// deep sleep and this is never reached.
void loop() {
    if (!sStayingAwake) {
        delay(100);
        return;
    }

    sButton.loop();
    improvLoop();
    if (sMode == MODE_SETUP) portalLoop();

    // Still honour the calendar: redraw when the day actually rolls over, so
    // development mode behaves like the real thing, just without sleeping.
    static uint32_t nextDayCheck = 0;
    if ((int32_t)(millis() - nextDayCheck) >= 0) {
        nextDayCheck = millis() + 60000;
        String key = todayKey();
        if (key.length() && key != settingsLastRendered()) {
            Serial.println("[power] date rolled over, redrawing");
            runDailyUpdate();
        }
    }

    delay(10);
}
