/**
 * Quotidian - LilyGo T5 4.7" e-paper (ESP32-S3), non-touch version.
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

// Defined by ESP-IDF but its header sits in a compat path off the include set.
extern "C" void esp_brownout_disable(void);

// --- Layout -----------------------------------------------------------------

static constexpr int kMargin = 36;
static constexpr int kGutter = 28;  // indent past the accent bar
static constexpr int kTextX  = kMargin + kGutter;
static constexpr int kContentRight = UI_WIDTH - kMargin;
static constexpr int kColumnWidth  = kContentRight - kTextX;

// Shown when the filesystem is missing or holds no readable quote for today.
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

// Whether the PCF8563 alarm is what started this cycle, as opposed to a reset
// or the button. Read from the chip's own alarm flag rather than from
// powerWakeSource(), which reports what woke the ESP32: the two disagree in the
// case that matters, where the alarm fired but the wake came from the backstop
// timer or a finger. Reported again before powering down: the boot line is
// usually lost while USB CDC enumerates.
static bool sWokeByAlarm = false;

// --- Wake accounting --------------------------------------------------------

#if SHOW_POWER_DIAGNOSTICS
// Counts this wake and maintains the drain baseline. See SHOW_POWER_DIAGNOSTICS
// in config.h for what the numbers are for.
static void recordWake(WakeSource source) {
    WakeStats stats = settingsWakeStats();

    // A battery found fuller than the baseline has been charged, which ends the
    // run being measured. Counters and baseline restart together, so the wake
    // count and the volts lost always describe the same stretch of time.
    //
    // Only ever on battery: running off USB must not wipe a run in progress,
    // which is exactly what reading the numbers over USB would otherwise do.
    const bool haveBaseline = stats.firstVolts > 0.0f;
    const bool recharged = haveBaseline && sBattery.volts > stats.firstVolts + 0.15f;
    if (sBattery.present && (!haveBaseline || recharged)) {
        stats = {};
        stats.firstVolts = sBattery.volts;
        stats.firstEpoch = timeSynced() ? (int64_t)time(nullptr) : 0;
    }

    stats.total++;
    switch (source) {
        case WakeSource::Alarm:  stats.byAlarm++;  break;
        case WakeSource::Timer:  stats.byTimer++;  break;
        case WakeSource::Button: stats.byButton++; break;
        default:                 stats.byOther++;  break;
    }

    settingsSaveWakeStats(stats);
    Serial.printf("[power] wake %lu by %s (alarm %lu, timer %lu, button %lu, other %lu)\n",
                  (unsigned long)stats.total, powerWakeSourceName(source),
                  (unsigned long)stats.byAlarm, (unsigned long)stats.byTimer,
                  (unsigned long)stats.byButton, (unsigned long)stats.byOther);
}
#endif

// --- Quote screen -----------------------------------------------------------

static constexpr int kQuoteTop    = 140;
static constexpr int kQuoteBottom = 800;
static constexpr int kQuoteLead   = 46;  // leading for Font::Large (42 px box)

// Side margins for the quote screen. Wide margins frame a short quote nicely;
// a long one would turn into a tall, thin column, so it drops to the narrower
// margin to gain width. The whole screen - header, quote, footer - shares the
// chosen margin so everything stays aligned.
static constexpr int kWideMargin   = 3 * kMargin;  // 108: short quotes
static constexpr int kNarrowMargin = 2 * kMargin;  //  72: longer quotes
static constexpr int kWideMarginMaxLines = 5;      // above this, use kNarrowMargin

// Picks the quote-screen margin from how many lines the quote wraps to at the
// wide setting. Cheap enough to wrap twice; drawQuote wraps again to lay out.
static int quoteMargin() {
    int wideWidth = UI_WIDTH - 2 * kWideMargin;
    if ((int)uiWrapText(Font::Large, sQuote.text, wideWidth).size() > kWideMarginMaxLines)
        return kNarrowMargin;
    return kWideMargin;
}

static void drawQuote(int margin) {
    const int bodyX     = margin;
    const int bodyWidth = UI_WIDTH - 2 * margin;
    std::vector<String> lines = uiWrapText(Font::Large, sQuote.text, bodyWidth);

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

    int y = top + uiAscender(Font::Large);
    for (const String &line : lines) {
        uiDrawText(Font::Large, bodyX, y, line.c_str(), ink::kTextBlack);
        y += kQuoteLead;
    }

    y = top + bodyHeight + authorGap + uiAscender(Font::BodyBold);
    String author = sQuote.author.length() ? sQuote.author : String("Unknown");
    uiDrawText(Font::BodyBold, bodyX, y,
               uiEllipsize(Font::BodyBold, author, bodyWidth).c_str(),
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
        if (used + uiTextWidth(Font::Small, suffix.c_str()) > bodyWidth) {
            suffix = uiEllipsize(Font::Small, suffix, bodyWidth - used);
        }

        int x = bodyX;
        x = uiDrawText(Font::Small, x, y, sQuote.datesPrefix.c_str(), ink::kTextMid);
        x = uiDrawText(Font::SmallBold, x, y, sQuote.datesBold.c_str(), ink::kTextBlack);
        uiDrawText(Font::Small, x, y, suffix.c_str(), ink::kTextMid);

        y += uiLineHeight(Font::Small);
    }
}

#if SHOW_POWER_DIAGNOSTICS
// The two lines that answer "where did the battery go". `line` draws one row and
// moves up; see drawFooter.
template <typename LineFn>
static void drawPowerDiagnostics(LineFn line) {
    const WakeStats stats = settingsWakeStats();

    char buf[128];
    snprintf(buf, sizeof(buf), "wakes %lu   alarm %lu   timer %lu   btn %lu   other %lu",
             (unsigned long)stats.total, (unsigned long)stats.byAlarm,
             (unsigned long)stats.byTimer, (unsigned long)stats.byButton,
             (unsigned long)stats.byOther);
    line(String(buf));

    if (!sBattery.present) {
        line(String("running on USB - no drain to measure"));
        return;
    }

    // Elapsed time comes from the clock rather than millis(), so it counts the
    // sleep that makes up almost all of it. Under a few hours the rate is mostly
    // ADC noise, so the raw reading stands alone until the run is long enough to
    // divide by.
    const int64_t now = timeSynced() ? (int64_t)time(nullptr) : 0;
    const double days = (stats.firstEpoch && now > stats.firstEpoch)
                            ? (now - stats.firstEpoch) / 86400.0
                            : 0.0;

    if (stats.firstVolts > 0.0f && days > 0.25) {
        const double lost = stats.firstVolts - sBattery.volts;
        snprintf(buf, sizeof(buf), "%.2f V %d%%   from %.2f V over %.1f d   %.0f mV/d",
                 sBattery.volts, sBattery.percent, stats.firstVolts, days,
                 lost * 1000.0 / days);
    } else {
        snprintf(buf, sizeof(buf), "%.2f V %d%%   baseline %.2f V",
                 sBattery.volts, sBattery.percent, stats.firstVolts);
    }
    line(String(buf));
}
#endif

// The foot of the page carries the credit the licence requires, and nothing
// else - no Wi-Fi, SD or Bluetooth status. This is a thing to read, not a
// dashboard, and it is going in a picture frame.
//
// The battery appears only once it is nearly flat, so it reads as a warning
// rather than as decoration.
static void drawFooter(int margin) {
    const int baseline   = 910;
    const int right      = UI_WIDTH - margin;
    const int bodyWidth  = UI_WIDTH - 2 * margin;

    if (sQuote.attribution.length()) {
        uiDrawText(Font::Small, margin, baseline,
                   uiEllipsize(Font::Small, sQuote.attribution, bodyWidth).c_str(),
                   ink::kTextLight);
    }

    // Diagnostic panels stack upwards from just above the credit, so either can
    // be switched on alone or both together without them landing on each other.
#if SHOW_CLOCK_DIAGNOSTICS || SHOW_POWER_DIAGNOSTICS
    const int diagLead = 26;
    int diagY = baseline - 30;
    auto diagLine = [&](const String &text) {
        uiDrawText(Font::Small, margin, diagY,
                   uiEllipsize(Font::Small, text, bodyWidth).c_str(), ink::kTextLight);
        diagY -= diagLead;
    };
#endif

#if SHOW_CLOCK_DIAGNOSTICS
    // Temporary. Everything needed to tell whether the clock and the alarm
    // agree with the wall clock, read off the panel because serial cannot be
    // opened without resetting the board.
    struct tm lt;
    char local[40] = "no clock";
    if (timeSynced() && getLocalTime(&lt, 0)) {
        snprintf(local, sizeof(local), "%02d-%02d %02d:%02d local",
                 lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    }
    diagLine(rtcDiagnostics());
    diagLine(String(local) + "  TZ " + settingsTimezone() +
             (sWokeByAlarm ? "  by ALARM" : "  by reset"));
#endif

#if SHOW_POWER_DIAGNOSTICS
    drawPowerDiagnostics(diagLine);
#endif

    if (!sBattery.present || sBattery.percent > LOW_BATTERY_PERCENT) return;

    const int iconW = 44;
    const int iconX = right - iconW;
    uiDrawBattery(iconX, baseline - 15, sBattery.percent);

    String label = String(sBattery.percent) + "%";
    uiDrawTextRight(Font::SmallBold, iconX - 10, baseline, label.c_str(), ink::kTextBlack);
}

static void renderQuoteScreen() {
    uiClearBuffer();

    const int margin = quoteMargin();
    const int right  = UI_WIDTH - margin;

    uiDrawText(Font::SmallBold, margin, 64, "QUOTIDIAN", ink::kTextMid);
    uiDrawTextRight(Font::Small, right, 64,
                    uiEllipsize(Font::Small, todayLong(), 260).c_str(), ink::kTextMid);
    uiDrawRule(margin, 86, right - margin, ink::kLight);

    drawQuote(margin);
    drawFooter(margin);
    uiFlush();
}

// --- Setup screen -----------------------------------------------------------

// One column, top to bottom: title, QR, the two routes, then device details.
// Portrait gives enough height that nothing has to share a row.
static void renderSetupScreen() {
    uiClearBuffer();

    uiDrawText(Font::Title, kMargin, 68, "Set up Wi-Fi", ink::kTextBlack);
    uiDrawText(Font::Small, kMargin, 100, "Quotidian", ink::kTextMid);
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

// Both of these used to live in RTC memory, which was wrong: it is lost to the
// power cuts, resets and reflashes this board sees in normal use, and losing it
// is silent. They are in NVS now - see settings.h.

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
// own timer is armed this much later purely as a backstop: its RC oscillator is
// minutes-per-day inaccurate, so it is no good for scheduling, but it does
// guarantee the device still wakes if the alarm never arrives.
//
// The margin has to clear the RC oscillator's own error over a day, or the
// backstop would race the alarm and take over a job it is far worse at.
static constexpr long kBackstopMarginSeconds = TEST_WAKE_SECONDS ? 60 : 3600;

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
        // relative alarm still brings the board back.
        Serial.printf("[power] clock unknown, retrying in %d min\n", SLEEP_RETRY_MINUTES);
        rtcSetAlarmInMinutes(SLEEP_RETRY_MINUTES);
        powerDown(SLEEP_RETRY_MINUTES * 60L + kBackstopMarginSeconds);
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

    powerDown(seconds + kBackstopMarginSeconds);
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
    // Disable the brownout detector first: the e-paper boost converter's inrush
    // dips VDD3V3 below the S3's brownout threshold for a moment, resetting the
    // board (reset reason 9). The dip is transient, not a real undervoltage, and
    // it is worse on battery's higher source impedance than on USB - which is
    // why the nightly wake failed on battery while working on USB.
    esp_brownout_disable();

    Serial.begin(115200);
    // Never block on Serial. With ARDUINO_USB_CDC_ON_BOOT the port is USB CDC;
    // if a host has it open but is not reading (or once the TX buffer fills), a
    // blocking write or flush hangs forever. That is what stalled powerDown
    // during bench testing. 0 = drop output rather than wait.
    Serial.setTxTimeoutMs(0);
    delay(200);

    powerLogWakeReason();

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
    Serial.printf("[boot] Quotidian - started by %s\n",
                  sWokeByAlarm ? "RTC alarm" : "power-on/reset");

    batteryBegin();

    // Sample while the radios are off - the sense pin is on ADC2, which Wi-Fi
    // takes over once it starts.
    sBattery = batteryRead();
    Serial.printf("[battery] %.2f V (%d%%)%s\n", sBattery.volts, sBattery.percent,
                  sBattery.present ? "" : " - no battery, running off USB");

#if SHOW_POWER_DIAGNOSTICS
    // After the battery read, which needs the radios off, and after the clock is
    // up, so the baseline can be stamped with a real time.
    recordWake(powerWakeSource());
#endif

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
