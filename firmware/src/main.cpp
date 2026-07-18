/**
 * Quote of the Day - LilyGo T5 4.7" e-paper (ESP32-S3), non-touch version.
 *
 * Renders a quote full-screen in anti-aliased grayscale type, with a status
 * footer showing Wi-Fi, SD card, Bluetooth and battery state.
 */

#ifndef BOARD_HAS_PSRAM
#error "Enable PSRAM (OPI) - the 960x540 framebuffer does not fit in internal RAM"
#endif

#include <Arduino.h>

#include "battery.h"
#include "config.h"
#include "storage.h"
#include "ui.h"
#include "wireless.h"

// --- Layout -----------------------------------------------------------------

static constexpr int kMargin      = 40;
static constexpr int kGutter      = 70;   // room for the decorative quote mark
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

static Quote sQuote;
static BatteryStatus sBattery;
static uint32_t sNextRefresh = 0;

// --- Rendering --------------------------------------------------------------

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

    String status = "Wi-Fi: " + wifiDescription();
    status += "   SD: " + storageDescription();
    status += "   BLE: " + String(bleActive() ? "on" : "off");
    uiDrawText(kMargin, 505, status.c_str(), ink::kTextMid);

    // Battery block, right aligned: icon then percentage.
    const int iconW = 59;  // body + terminal nub
    String label = sBattery.present ? String(sBattery.percent) + "%  " +
                                          String(sBattery.volts, 2) + "V"
                                    : String("USB");

    int labelW = uiTextWidth(label.c_str());
    int iconX = EPD_WIDTH - kMargin - labelW - 12 - iconW;

    uiDrawBattery(iconX, 484, sBattery.present ? sBattery.percent : -1);
    uiDrawTextRight(EPD_WIDTH - kMargin, 505, label.c_str(), ink::kTextDark);
}

static void render() {
    uiClearBuffer();
    drawHeader();
    drawQuote();
    drawFooter();
    uiFlush();
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

    batteryBegin();

    // Sample the battery while the radios are still off - the sense pin is on
    // ADC2, which Wi-Fi takes over once it starts.
    sBattery = batteryRead();
    Serial.printf("[battery] %.2f V (%d%%)%s\n", sBattery.volts, sBattery.percent,
                  sBattery.present ? "" : " - no battery, running off USB");

    storageBegin();
    if (!storageReadQuote(sQuote)) {
        sQuote = kFallbackQuote;
        Serial.println("[content] using built-in fallback quote");
    }

    wifiBegin();
    bleBegin();
    bleSetBatteryLevel(sBattery.percent);

    render();
    sNextRefresh = millis() + REFRESH_INTERVAL_MS;
}

void loop() {
    if ((int32_t)(millis() - sNextRefresh) < 0) {
        delay(1000);
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

    render();
    sNextRefresh = millis() + REFRESH_INTERVAL_MS;
}
