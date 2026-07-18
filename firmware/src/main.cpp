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
#include "portal.h"
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

// --- Quote screen -----------------------------------------------------------

static constexpr int kQuoteTop    = 120;
static constexpr int kQuoteBottom = 820;
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
    const int attribHeight =
        sQuote.attribution.length() ? 10 + uiLineHeight(Font::Small) : 0;

    const int total = bodyHeight + authorGap + authorHeight + datesHeight + attribHeight;

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

    // Attribution. The corpus is CC BY-SA, which requires crediting the source
    // and naming the licence wherever the quote is shown.
    if (sQuote.attribution.length()) {
        y += 10 + uiAscender(Font::Small);
        uiDrawText(Font::Small, kTextX, y,
                   uiEllipsize(Font::Small, sQuote.attribution, kColumnWidth).c_str(),
                   ink::kTextLight);
    }
}

static void drawStatusFooter() {
    uiDrawRule(kMargin, 858, kContentRight - kMargin, ink::kLight);

    // The battery sits at the right of the footer block and the status lines
    // run down the left, so the two cannot collide however long the SSID is.
    const int iconW = 44;
    const int iconX = kContentRight - iconW;
    uiDrawBattery(iconX, 886, sBattery.present ? sBattery.percent : -1);

    String label = sBattery.present ? String(sBattery.percent) + "%" : String("USB");
    const int labelW = uiTextWidth(Font::SmallBold, label.c_str());
    uiDrawTextRight(Font::SmallBold, iconX - 12, 902, label.c_str(), ink::kTextDark);

    const int statusWidth = (iconX - 12 - labelW - 20) - kMargin;

    uiDrawText(Font::Small, kMargin, 894,
               uiEllipsize(Font::Small, "Wi-Fi: " + wifiDescription(), statusWidth).c_str(),
               ink::kTextMid);
    uiDrawText(Font::Small, kMargin, 920,
               uiEllipsize(Font::Small, "SD: " + storageDescription(), statusWidth).c_str(),
               ink::kTextMid);
    uiDrawText(Font::Small, kMargin, 946,
               (String("BLE: ") + (bleActive() ? "on" : "off")).c_str(), ink::kTextMid);
}

static void renderQuoteScreen() {
    uiClearBuffer();

    uiDrawText(Font::SmallBold, kMargin, 52, "QUOTE OF THE DAY", ink::kTextMid);
    uiDrawTextRight(Font::Small, kContentRight, 52,
                    uiEllipsize(Font::Small, todayLong(), 260).c_str(), ink::kTextMid);
    uiDrawRule(kMargin, 74, kContentRight - kMargin, ink::kLight);

    drawQuote();
    drawStatusFooter();
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
// author's birth or death day is always today. Without a synced clock the day
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

// --- Mode transitions -------------------------------------------------------

static void enterRunningMode() {
    sMode = MODE_RUNNING;
    sSetupError = "";

    // Only now is the clock synced, so only now can today's quote be chosen.
    loadQuote();

    improvSetProvisioned(true);
    if (!bleActive()) bleBegin();
    bleSetBatteryLevel(sBattery.percent);

    renderQuoteScreen();
    sNextRefresh = millis() + REFRESH_INTERVAL_MS;
}

static void enterSetupMode() {
    sMode = MODE_SETUP;

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
    enterRunningMode();
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
    enterRunningMode();
}

// --- Button -----------------------------------------------------------------

static void onLongPress(Button2 &btn) {
    (void)btn;
    Serial.println("[button] long press - clearing Wi-Fi credentials");

    settingsClear();
    renderMessage("Wi-Fi forgotten", "Restarting for setup...");
    delay(1500);
    ESP.restart();
}

// --- Lifecycle --------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] Quote of the Day");

    if (!uiBegin()) {
        // Without a framebuffer there is nothing to show; halt loudly.
        while (true) {
            Serial.println("[boot] halted: no framebuffer");
            delay(5000);
        }
    }

    settingsBegin();
    batteryBegin();

    // Sample the battery while the radios are still off - the sense pin is on
    // ADC2, which Wi-Fi takes over once it starts.
    sBattery = batteryRead();
    Serial.printf("[battery] %.2f V (%d%%)%s\n", sBattery.volts, sBattery.percent,
                  sBattery.present ? "" : " - no battery, running off USB");

    sButton.begin(BUTTON_1);
    sButton.setLongClickTime(RESET_HOLD_MS);
    sButton.setLongClickDetectedHandler(onLongPress);

    storageBegin();
    improvBegin(onImprovCredentials);

    if (settingsHasCredentials() && wifiBegin()) {
        enterRunningMode();
    } else {
        if (settingsHasCredentials()) {
            sSetupError = "Could not reach " + settingsSsid() + ".";
        }
        enterSetupMode();
    }
}

void loop() {
    sButton.loop();
    improvLoop();

    if (sMode == MODE_SETUP) {
        portalLoop();
        if (sPendingCredentials) applyPendingCredentials();
        return;
    }

    if ((int32_t)(millis() - sNextRefresh) < 0) {
        delay(10);
        return;
    }

    // Re-reading the battery means dropping Wi-Fi for the duration, because
    // ADC2 and the radio cannot both be active.
    bool wasConnected = wifiConnected();
    if (wasConnected) wifiStop();

    sBattery = batteryRead();
    bleSetBatteryLevel(sBattery.percent);

    if (wasConnected) wifiBegin();

    if (storageMounted()) loadQuote();

    renderQuoteScreen();
    sNextRefresh = millis() + REFRESH_INTERVAL_MS;
}
