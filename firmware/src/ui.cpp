#include "ui.h"

#include <qrcode.h>

#include "firasans.h"

// Version 4 (33x33 modules) holds 78 bytes at ECC_LOW - ample for a Wi-Fi
// join payload, which runs to roughly 45 characters.
static constexpr uint8_t kQrVersion = 4;
static constexpr uint8_t kQrQuietZone = 4;  // modules, mandated by the spec

static uint8_t *sFramebuffer = nullptr;

// The bundled FiraSans is the only font in the library: advance_y 50,
// ascender 39, descender -12.
static const GFXfont *kFont = &FiraSans;

static FontProperties textProps(uint8_t textColor) {
    FontProperties props = {};
    props.fg_color = textColor;
    props.bg_color = ink::kTextPaper;
    props.fallback_glyph = '?';
    props.flags = 0;
    return props;
}

bool uiBegin() {
    epd_init();

    // 4 bits per pixel, two pixels per byte.
    sFramebuffer = (uint8_t *)heap_caps_malloc(EPD_WIDTH / 2 * EPD_HEIGHT, MALLOC_CAP_SPIRAM);
    if (!sFramebuffer) {
        Serial.println("[ui] framebuffer allocation failed - is PSRAM enabled?");
        return false;
    }

    uiClearBuffer();
    return true;
}

void uiClearBuffer() {
    memset(sFramebuffer, ink::kPaper, EPD_WIDTH / 2 * EPD_HEIGHT);
}

void uiFlush() {
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), sFramebuffer);
    epd_poweroff_all();
}

int uiTextWidth(const char *text) {
    int32_t x = 0, y = 0, x1 = 0, y1 = 0, w = 0, h = 0;
    FontProperties props = textProps(ink::kTextBlack);
    get_text_bounds(kFont, text, &x, &y, &x1, &y1, &w, &h, &props);
    return (int)w;
}

int uiDrawText(int x, int y, const char *text, uint8_t textColor) {
    int32_t cursorX = x;
    int32_t cursorY = y;
    FontProperties props = textProps(textColor);
    // Drawing into the framebuffer (rather than passing NULL for direct output)
    // is what preserves the glyphs' grayscale edges.
    write_mode(kFont, text, &cursorX, &cursorY, sFramebuffer, BLACK_ON_WHITE, &props);
    return (int)cursorX;
}

void uiDrawTextRight(int xRight, int y, const char *text, uint8_t textColor) {
    uiDrawText(xRight - uiTextWidth(text), y, text, textColor);
}

std::vector<String> uiWrapText(const String &text, int maxWidth) {
    std::vector<String> lines;
    String line;

    int start = 0;
    while (start <= text.length()) {
        int space = text.indexOf(' ', start);
        if (space < 0) space = text.length();

        String word = text.substring(start, space);
        if (word.length() > 0) {
            String candidate = line.length() ? line + " " + word : word;
            if (uiTextWidth(candidate.c_str()) > maxWidth && line.length()) {
                lines.push_back(line);
                line = word;
            } else {
                line = candidate;
            }
        }
        start = space + 1;
    }

    if (line.length()) lines.push_back(line);
    return lines;
}

String uiEllipsize(const String &text, int maxWidth) {
    if (uiTextWidth(text.c_str()) <= maxWidth) return text;

    String out = text;
    while (out.length() > 1 && uiTextWidth((out + "...").c_str()) > maxWidth) {
        out.remove(out.length() - 1);
    }
    return out + "...";
}

void uiDrawRule(int x, int y, int width, uint8_t color) {
    epd_draw_hline(x, y, width, color, sFramebuffer);
}

void uiDrawAccentBar(int x, int y, int width, int height) {
    // One row at a time so the ramp is smooth rather than banded. The panel
    // quantises to 16 levels on its own.
    for (int i = 0; i < height; i++) {
        float t = (float)i / (float)(height - 1);
        uint8_t shade = (uint8_t)(ink::kDark + t * (ink::kLight - ink::kDark));
        epd_draw_hline(x, y + i, width, shade, sFramebuffer);
    }
}

int uiQrSize(const char *text, int scale) {
    (void)text;  // module count follows from the version alone
    int modules = 4 * kQrVersion + 17;
    return (modules + 2 * kQrQuietZone) * scale;
}

int uiDrawQr(int x, int y, const char *text, int scale) {
    QRCode qrcode;
    uint8_t data[qrcode_getBufferSize(kQrVersion)];

    if (qrcode_initText(&qrcode, data, kQrVersion, ECC_LOW, text) < 0) {
        Serial.println("[ui] QR payload too long");
        return 0;
    }

    int side = (qrcode.size + 2 * kQrQuietZone) * scale;

    // The quiet zone must be paper-white or scanners struggle to lock on.
    epd_fill_rect(x, y, side, side, ink::kPaper, sFramebuffer);

    int origin = kQrQuietZone * scale;
    for (uint8_t my = 0; my < qrcode.size; my++) {
        for (uint8_t mx = 0; mx < qrcode.size; mx++) {
            if (!qrcode_getModule(&qrcode, mx, my)) continue;
            epd_fill_rect(x + origin + mx * scale, y + origin + my * scale,
                          scale, scale, ink::kBlack, sFramebuffer);
        }
    }

    return side;
}

void uiDrawBattery(int x, int y, int percent) {
    const int w = 54;
    const int h = 26;
    const int nubW = 5;
    const int nubH = 12;

    epd_draw_rect(x, y, w, h, ink::kDark, sFramebuffer);
    epd_fill_rect(x + w, y + (h - nubH) / 2, nubW, nubH, ink::kDark, sFramebuffer);

    if (percent < 0) {
        // No battery detected - running off USB.
        epd_draw_hline(x + 14, y + h / 2, w - 28, ink::kMid, sFramebuffer);
        return;
    }

    const int inset = 4;
    const int trackW = w - inset * 2;
    int fillW = (trackW * constrain(percent, 0, 100)) / 100;

    // Light wash over the whole track, solid fill for the charge itself. Both
    // are mid-greys rather than black, so the icon stays visually quieter than
    // the text next to it.
    epd_fill_rect(x + inset, y + inset, trackW, h - inset * 2, ink::kLight, sFramebuffer);
    if (fillW > 0) {
        epd_fill_rect(x + inset, y + inset, fillW, h - inset * 2, ink::kDark, sFramebuffer);
    }
}
