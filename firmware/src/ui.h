#pragma once

#include <Arduino.h>
#include <vector>

#include "epd_driver.h"

// The panel is a 16-level grayscale device. Two different scales are in play:
//
//   * Shapes (lines, rects, circles) take an 8-bit value, 0x00 black .. 0xFF white.
//   * Text takes a 4-bit value, 0 black .. 15 white, because the glyph renderer
//     blends foreground into background through a 16-entry LUT. That blend is
//     exactly what makes the type anti-aliased: the font bitmaps store per-pixel
//     coverage, not on/off bits.
//
// Both scales are listed here so the two never get mixed up by accident.
namespace ink {
// 8-bit, for shapes.
constexpr uint8_t kBlack = 0x00;
constexpr uint8_t kDark  = 0x33;
constexpr uint8_t kMid   = 0x77;
constexpr uint8_t kLight = 0xBB;
constexpr uint8_t kPaper = 0xFF;

// 4-bit, for text.
constexpr uint8_t kTextBlack = 0;
constexpr uint8_t kTextDark  = 2;
constexpr uint8_t kTextMid   = 6;
constexpr uint8_t kTextLight = 10;
constexpr uint8_t kTextPaper = 15;
}  // namespace ink

// Allocates the framebuffer in PSRAM and brings the panel up. False if the
// allocation failed, in which case nothing else in here is safe to call.
bool uiBegin();

// Resets the framebuffer to white. Does not touch the panel.
void uiClearBuffer();

// Pushes the framebuffer to the panel: power on, flash clear, draw, power off.
void uiFlush();

// Width in pixels that `text` would occupy on one line.
int uiTextWidth(const char *text);

// Draws a single line with its baseline at `y`. Returns the x cursor afterwards.
int uiDrawText(int x, int y, const char *text, uint8_t textColor);

// Draws `text` right-aligned so it ends at `xRight`.
void uiDrawTextRight(int xRight, int y, const char *text, uint8_t textColor);

// Greedy word wrap. Splits on spaces only, so long unbroken tokens may overrun.
std::vector<String> uiWrapText(const String &text, int maxWidth);

// Battery pictogram with a grayscale fill proportional to `percent`.
// `percent` < 0 draws an empty body with a dash, meaning "no battery".
void uiDrawBattery(int x, int y, int percent);

// Horizontal hairline.
void uiDrawRule(int x, int y, int width, uint8_t color);

// Vertical accent bar that fades from dark at the top to near-paper at the
// bottom, using the panel's intermediate grey levels.
void uiDrawAccentBar(int x, int y, int width, int height);

// Renders `text` as a QR code with each module drawn `scale` pixels square,
// including the mandatory 4-module quiet zone. Returns the total side length
// in pixels, or 0 if the text did not fit. Drawn pure black on white: e-paper
// greys would only hurt scanning contrast.
int uiDrawQr(int x, int y, const char *text, int scale);

// Side length uiDrawQr() would produce, without drawing anything.
int uiQrSize(const char *text, int scale);
