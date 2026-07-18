/**
 * Quote of the Day - LilyGo T5 4.7" e-paper (ESP32-S3), non-touch version.
 *
 * Renders a quote full-screen in anti-aliased grayscale type, with a status
 * footer showing Wi-Fi, SD card, Bluetooth and battery state.
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

static constexpr int kMargin      = 40;
static constexpr int kGutter      = 70;   // room for the decorative accent bar
static constexpr int kLineHeight  = 58;   // FiraSans advance_y is 50; a little air
static constexpr int kHeaderRuleY = 84;
static constexpr int kFooterRuleY = 452;
static constexpr int kBodyTop     = 130;
static constexpr int kBodyBottom  = 430;

// Shown when there is no SD card, or no readable quote file on it.
static const Quote kFallbackQuote = {
    "I think, therefore I am",
    "René Descartes",
    "31 March 1596 - 11 February 1650",
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

// FiraSans metrics, needed to reason about where glyphs actually land relative
// to their baseline.
static constexpr int kAscender  = 39;
static constexpr int kDescender = 12;

// Draws text and reports on the serial log when it would leave the intended
// box. The e-paper cannot be inspected from the build machine, so this is how
// layout regressions get caught - see "Verifying layout" in the README.
static void drawChecked(int x, int baseline, int rightLimit, const char *text,
                        uint8_t color) {
    int w = uiTextWidth(text);
    if (x + w > rightLimit) {
        Serial.printf("[layout] OVERFLOW right: x=%d w=%d end=%d limit=%d \"%s\"\n",
                      x, w, x + w, rightLimit, text);
    }
    if (baseline + kDescender > EPD_HEIGHT) {
        Serial.printf("[layout] OVERFLOW bottom: baseline=%d \"%s\"\n", baseline, text);
    }
    uiDrawText(x, baseline, text, color);
}

// --- Quote screen -----------------------------------------------------------

static void drawHeader() {
    uiDrawText(kMargin, 58, "QUOTE OF THE DAY", ink::kTextMid);
    uiDrawTextRight(EPD_WIDTH - kMargin, 58, todayLong().c_str(), ink::kTextMid);
    uiDrawRule(kMargin, kHeaderRuleY, EPD_WIDTH - 2 * kMargin, ink::kMid);
}

static void drawQuote() {
    const int textX = kMargin + kGutter;
    const int maxWidth = EPD_WIDTH - textX - kMargin;

    std::vector<String> lines = uiWrapText(sQuote.text, maxWidth);

    // Attribution sits one blank line below the quote body.
    int blockHeight = lines.size() * kLineHeight + kLineHeight;
    if (sQuote.source.length()) blockHeight += kLineHeight;

    int top = kBodyTop + ((kBodyBottom - kBodyTop) - blockHeight) / 2;
    if (top < kBodyTop) top = kBodyTop;

    // Accent bar in the gutter, spanning just the quote body. The bundled font
    // only covers ASCII + Latin-1, so a typographic quote mark is not an
    // option here - a grey ramp does the decorative work instead.
    uiDrawAccentBar(kMargin, top + 12, 6, lines.size() * kLineHeight);

    int y = top + kLineHeight;  // step to the first baseline

    for (const String &line : lines) {
        uiDrawText(textX, y, line.c_str(), ink::kTextBlack);
        y += kLineHeight;
    }

    y += kLineHeight / 2;
    String byline = "- " + (sQuote.author.length() ? sQuote.author : String("Unknown"));
    uiDrawText(textX, y, byline.c_str(), ink::kTextDark);

    if (sQuote.source.length()) {
        y += kLineHeight;
        uiDrawText(textX, y, sQuote.source.c_str(), ink::kTextMid);
    }
}

static void drawFooter() {
    uiDrawRule(kMargin, kFooterRuleY, EPD_WIDTH - 2 * kMargin, ink::kLight);

    // Battery block first, right aligned: icon then percentage. Its left edge
    // is what bounds the status text, which contains an arbitrary SSID.
    const int iconW = 59;  // body + terminal nub
    String label = sBattery.present ? String(sBattery.percent) + "%  " +
                                          String(sBattery.volts, 2) + "V"
                                    : String("USB");

    int labelW = uiTextWidth(label.c_str());
    int iconX = EPD_WIDTH - kMargin - labelW - 12 - iconW;

    uiDrawBattery(iconX, 484, sBattery.present ? sBattery.percent : -1);
    uiDrawTextRight(EPD_WIDTH - kMargin, 505, label.c_str(), ink::kTextDark);

    String status = "Wi-Fi: " + wifiDescription();
    status += "   SD: " + storageDescription();
    status += "   BLE: " + String(bleActive() ? "on" : "off");

    const int statusLimit = iconX - 24;
    drawChecked(kMargin, 505, statusLimit,
                uiEllipsize(status, statusLimit - kMargin).c_str(), ink::kTextMid);
}

static void renderQuoteScreen() {
    uiClearBuffer();
    drawHeader();
    drawQuote();
    drawFooter();
    uiFlush();
}

// --- Setup screen -----------------------------------------------------------

// Two columns: instructions on the left, the join QR on the right.
//
// The vertical budget is tight - only one font is available and its line box is
// 51 px - so every baseline below is placed explicitly rather than accumulated
// from wrapped text. Wrapping here previously pushed the second option past the
// footer rule and straight through the footer text.
static void renderSetupScreen() {
    uiClearBuffer();

    uiDrawText(kMargin, 58, "SET UP WI-FI", ink::kTextMid);
    uiDrawTextRight(EPD_WIDTH - kMargin, 58, "Quote of the Day", ink::kTextMid);
    uiDrawRule(kMargin, kHeaderRuleY, EPD_WIDTH - 2 * kMargin, ink::kMid);

    // --- Right column: QR, vertically centred in the body region ---
    const int qrScale = 5;
    const int qrSide = uiQrSize(portalQrPayload().c_str(), qrScale);
    const int qrX = EPD_WIDTH - kMargin - qrSide;
    const int qrY = (kHeaderRuleY + kFooterRuleY) / 2 - qrSide / 2;

    uiDrawQr(qrX, qrY, portalQrPayload().c_str(), qrScale);

    // --- Left column ---
    const int textX = kMargin + kGutter;
    const int textRight = qrX - 30;  // keep clear of the QR's quiet zone

    // Baselines are explicit. Only ~29 characters fit in this column at the
    // one available font size, so the copy is kept terse rather than wrapped.
    const int phoneTitleY = 150;
    const int phoneBodyY  = 208;
    const int compTitleY  = 285;
    const int compBodyY1  = 343;
    const int compBodyY2  = 401;
    const int detailsY    = 430;

    uiDrawAccentBar(kMargin, phoneTitleY - kAscender, 6,
                    (phoneBodyY + kDescender) - (phoneTitleY - kAscender));
    drawChecked(textX, phoneTitleY, textRight, "With a phone", ink::kTextBlack);
    drawChecked(textX, phoneBodyY, textRight,
                "Scan the code to connect.", ink::kTextDark);

    uiDrawAccentBar(kMargin, compTitleY - kAscender, 6,
                    (compBodyY2 + kDescender) - (compTitleY - kAscender));
    drawChecked(textX, compTitleY, textRight, "With a computer", ink::kTextBlack);
    drawChecked(textX, compBodyY1, textRight, "Open improv-wifi.com", ink::kTextDark);
    drawChecked(textX, compBodyY2, textRight, "in Chrome or Edge.", ink::kTextDark);

    // Network name and key for anyone who cannot scan. Sits below the QR, so it
    // may run the full width - which is why it is not in the right column,
    // where it previously ran off the screen edge.
    String details = "Network: " + portalSsid() + "   Key: " + portalPassword();
    drawChecked(kMargin, detailsY, EPD_WIDTH - kMargin,
                uiEllipsize(details, EPD_WIDTH - 2 * kMargin).c_str(),
                ink::kTextMid);

    // --- Footer ---
    uiDrawRule(kMargin, kFooterRuleY, EPD_WIDTH - 2 * kMargin, ink::kLight);

    String footer = sSetupError.length()
                        ? sSetupError
                        : String("Hold the button 3 s to start over.");
    drawChecked(kMargin, 505, EPD_WIDTH - kMargin,
                uiEllipsize(footer, EPD_WIDTH - 2 * kMargin).c_str(),
                sSetupError.length() ? ink::kTextBlack : ink::kTextMid);

    uiFlush();
}

static void renderMessage(const char *title, const char *detail) {
    const int x = kMargin + kGutter;
    const int limit = EPD_WIDTH - kMargin;

    uiClearBuffer();
    uiDrawAccentBar(kMargin, 240 - kAscender, 6, kLineHeight + kAscender + kDescender);
    drawChecked(x, 240, limit, uiEllipsize(title, limit - x).c_str(), ink::kTextBlack);
    if (detail) {
        drawChecked(x, 240 + kLineHeight, limit,
                    uiEllipsize(detail, limit - x).c_str(), ink::kTextDark);
    }
    uiFlush();
}

// --- Content ----------------------------------------------------------------

static void loadQuote() {
    if (!storageReadQuote(sQuote)) {
        sQuote = kFallbackQuote;
        Serial.println("[content] using built-in fallback quote");
    }
}

// --- Mode transitions -------------------------------------------------------

static void enterRunningMode() {
    sMode = MODE_RUNNING;
    sSetupError = "";

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
    renderMessage("Connecting...", ssid.c_str());

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

    renderMessage("Connecting...", sPendingSsid.c_str());
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
    renderMessage("Wi-Fi forgotten.", "Restarting for setup...");
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
    loadQuote();

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

    if (storageMounted()) {
        Quote fresh;
        if (storageReadQuote(fresh)) sQuote = fresh;
    }

    renderQuoteScreen();
    sNextRefresh = millis() + REFRESH_INTERVAL_MS;
}
