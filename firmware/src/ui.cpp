#include "ui.h"

#include <qrcode.h>

#include "fonts/font_body.h"
#include "fonts/font_body_bold.h"
#include "fonts/font_large.h"
#include "fonts/font_small.h"
#include "fonts/font_small_bold.h"
#include "fonts/font_title.h"

// Quarter turn counter-clockwise. Flip to false if the display ends up upside
// down for the way the device is actually stood up.
static constexpr bool UI_ROTATE_CCW = true;

static uint8_t *sFramebuffer = nullptr;

// Version 4 (33x33 modules) holds 78 bytes at ECC_LOW - ample for a Wi-Fi join
// payload, which runs to roughly 45 characters.
static constexpr uint8_t kQrVersion = 4;
static constexpr uint8_t kQrQuietZone = 4;  // modules, mandated by the spec

static const GFXfont *fontFor(Font font) {
    switch (font) {
        case Font::Small:     return &FontSmall;
        case Font::SmallBold: return &FontSmallBold;
        case Font::Body:      return &FontBody;
        case Font::BodyBold:  return &FontBodyBold;
        case Font::Large:     return &FontLarge;
        case Font::Title:     return &FontTitle;
    }
    return &FontBody;
}

// --- Rotation ---------------------------------------------------------------
//
// Portrait (lx, ly) -> landscape framebuffer. Counter-clockwise sends the
// portrait top edge to the left edge of the panel.

static inline void mapPoint(int lx, int ly, int *px, int *py) {
    if (UI_ROTATE_CCW) {
        *px = ly;
        *py = (UI_WIDTH - 1) - lx;
    } else {
        *px = (UI_HEIGHT - 1) - ly;
        *py = lx;
    }
}

static inline void putPixel4(int lx, int ly, uint8_t value) {
    if (lx < 0 || lx >= UI_WIDTH || ly < 0 || ly >= UI_HEIGHT) return;

    int px, py;
    mapPoint(lx, ly, &px, &py);

    uint32_t pos = (uint32_t)py * (EPD_WIDTH / 2) + (px >> 1);
    uint8_t old = sFramebuffer[pos];
    if (px & 1) {
        sFramebuffer[pos] = (old & 0x0F) | (value << 4);
    } else {
        sFramebuffer[pos] = (old & 0xF0) | value;
    }
}

// --- Collision detection ----------------------------------------------------
//
// Bounds checks alone are not enough: two strings can each sit inside the
// screen and still land on top of each other. That is exactly what shipped in
// the first setup screen, so every string drawn in a frame is recorded and
// tested against its predecessors.

struct TextRect {
    int  x0, y0, x1, y1;
    char label[24];
};

static TextRect sRects[40];
static int sRectCount = 0;

static void recordAndCheckOverlap(int x, int y0, int y1, int width, const char *text) {
    if (x < 0 || x + width > UI_WIDTH) {
        Serial.printf("[layout] OFF-CANVAS x: %d..%d (canvas 0..%d) \"%.23s\"\n",
                      x, x + width, UI_WIDTH, text);
    }
    if (y0 < 0 || y1 > UI_HEIGHT) {
        Serial.printf("[layout] OFF-CANVAS y: %d..%d (canvas 0..%d) \"%.23s\"\n",
                      y0, y1, UI_HEIGHT, text);
    }

    for (int i = 0; i < sRectCount; i++) {
        const TextRect &r = sRects[i];
        bool overlaps = x < r.x1 && x + width > r.x0 && y0 < r.y1 && y1 > r.y0;
        if (overlaps) {
            Serial.printf("[layout] OVERLAP \"%.23s\" (%d,%d-%d,%d) with \"%s\"\n",
                          text, x, y0, x + width, y1, r.label);
        }
    }

    if (sRectCount < (int)(sizeof(sRects) / sizeof(sRects[0]))) {
        TextRect &r = sRects[sRectCount++];
        r.x0 = x; r.y0 = y0; r.x1 = x + width; r.y1 = y1;
        snprintf(r.label, sizeof(r.label), "%s", text);
    }
}

// --- Lifecycle --------------------------------------------------------------

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
    sRectCount = 0;
}

void uiFlush() {
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), sFramebuffer);
    epd_poweroff_all();
}

// --- Text -------------------------------------------------------------------

// Minimal UTF-8 decode. Advances `p` past the sequence.
static uint32_t nextCodepoint(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t cp = *s;

    if (cp < 0x80) {
        *p += 1;
    } else if ((cp & 0xE0) == 0xC0) {
        cp = ((cp & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((cp & 0xF0) == 0xE0) {
        cp = ((cp & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else {
        cp = ((cp & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
    }
    return cp;
}

static GFXglyph *glyphFor(const GFXfont *f, uint32_t cp) {
    GFXglyph *g = nullptr;
    get_glyph(f, cp, &g);
    if (!g) get_glyph(f, '?', &g);
    return g;
}

int uiTextWidth(Font font, const char *text) {
    const GFXfont *f = fontFor(font);
    int width = 0;
    const char *p = text;
    while (*p) {
        GFXglyph *g = glyphFor(f, nextCodepoint(&p));
        if (g) width += g->advance_x;
    }
    return width;
}

int uiLineHeight(Font font) { return fontFor(font)->advance_y; }
int uiAscender(Font font)   { return fontFor(font)->ascender; }
int uiDescender(Font font)  { return -fontFor(font)->descender; }

// Draws one glyph into the portrait canvas, blending its coverage toward the
// foreground so edges stay anti-aliased. Zero-coverage pixels are skipped so
// whatever is underneath shows through.
static void drawGlyph(const GFXfont *f, GFXglyph *g, int x, int baseline, uint8_t fg) {
    const uint8_t *bitmap = f->bitmap + g->data_offset;
    int byteWidth = g->width / 2 + g->width % 2;

    uint8_t ramp[16];
    for (int c = 0; c < 16; c++) {
        ramp[c] = (uint8_t)(ink::kTextPaper + c * ((int)fg - ink::kTextPaper) / 15);
    }

    for (int gy = 0; gy < g->height; gy++) {
        int ly = baseline - g->top + gy;
        for (int gx = 0; gx < g->width; gx++) {
            uint8_t byte = bitmap[gy * byteWidth + gx / 2];
            uint8_t coverage = (gx & 1) ? (byte >> 4) : (byte & 0x0F);
            if (coverage == 0) continue;
            putPixel4(x + g->left + gx, ly, ramp[coverage]);
        }
    }
}

int uiDrawText(Font font, int x, int y, const char *text, uint8_t textColor) {
    const GFXfont *f = fontFor(font);

    recordAndCheckOverlap(x, y - f->ascender, y - f->descender,
                          uiTextWidth(font, text), text);

    int cursor = x;
    const char *p = text;
    while (*p) {
        GFXglyph *g = glyphFor(f, nextCodepoint(&p));
        if (!g) continue;
        drawGlyph(f, g, cursor, y, textColor);
        cursor += g->advance_x;
    }
    return cursor;
}

void uiDrawTextRight(Font font, int xRight, int y, const char *text, uint8_t textColor) {
    uiDrawText(font, xRight - uiTextWidth(font, text), y, text, textColor);
}

void uiDrawTextCenter(Font font, int xCenter, int y, const char *text, uint8_t textColor) {
    uiDrawText(font, xCenter - uiTextWidth(font, text) / 2, y, text, textColor);
}

std::vector<String> uiWrapText(Font font, const String &text, int maxWidth) {
    std::vector<String> lines;
    String line;

    int start = 0;
    while (start <= (int)text.length()) {
        int space = text.indexOf(' ', start);
        if (space < 0) space = text.length();

        String word = text.substring(start, space);
        if (word.length() > 0) {
            String candidate = line.length() ? line + " " + word : word;
            if (uiTextWidth(font, candidate.c_str()) > maxWidth && line.length()) {
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

String uiEllipsize(Font font, const String &text, int maxWidth) {
    if (uiTextWidth(font, text.c_str()) <= maxWidth) return text;

    String out = text;
    while (out.length() > 1 && uiTextWidth(font, (out + "...").c_str()) > maxWidth) {
        out.remove(out.length() - 1);
    }
    return out + "...";
}

// --- Shapes -----------------------------------------------------------------

void uiFillRect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;

    // A rotated rectangle is still a rectangle, so this maps the whole shape
    // once and lets the driver fill it, rather than going pixel by pixel.
    int px, py;
    if (UI_ROTATE_CCW) {
        mapPoint(x + w - 1, y, &px, &py);
    } else {
        mapPoint(x, y + h - 1, &px, &py);
    }
    epd_fill_rect(px, py, h, w, color, sFramebuffer);
}

void uiDrawRule(int x, int y, int width, uint8_t color) {
    uiFillRect(x, y, width, 1, color);
}

void uiDrawAccentBar(int x, int y, int width, int height) {
    // One row at a time so the ramp is smooth rather than banded. The panel
    // quantises to 16 levels on its own.
    for (int i = 0; i < height; i++) {
        float t = (float)i / (float)(height - 1);
        uint8_t shade = (uint8_t)(ink::kDark + t * (ink::kLight - ink::kDark));
        uiFillRect(x, y + i, width, 1, shade);
    }
}

void uiDrawBattery(int x, int y, int percent) {
    const int w = 40;
    const int h = 20;
    const int nubW = 4;
    const int nubH = 9;

    uiFillRect(x, y, w, 2, ink::kDark);
    uiFillRect(x, y + h - 2, w, 2, ink::kDark);
    uiFillRect(x, y, 2, h, ink::kDark);
    uiFillRect(x + w - 2, y, 2, h, ink::kDark);
    uiFillRect(x + w, y + (h - nubH) / 2, nubW, nubH, ink::kDark);

    if (percent < 0) {
        uiFillRect(x + 10, y + h / 2, w - 20, 2, ink::kMid);
        return;
    }

    const int inset = 4;
    const int trackW = w - inset * 2;
    int fillW = (trackW * constrain(percent, 0, 100)) / 100;

    uiFillRect(x + inset, y + inset, trackW, h - inset * 2, ink::kLight);
    if (fillW > 0) {
        uiFillRect(x + inset, y + inset, fillW, h - inset * 2, ink::kDark);
    }
}

int uiQrSize(const char *text, int scale) {
    (void)text;  // module count follows from the version alone
    return (4 * kQrVersion + 17 + 2 * kQrQuietZone) * scale;
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
    uiFillRect(x, y, side, side, ink::kPaper);

    int origin = kQrQuietZone * scale;
    for (uint8_t my = 0; my < qrcode.size; my++) {
        for (uint8_t mx = 0; mx < qrcode.size; mx++) {
            if (!qrcode_getModule(&qrcode, mx, my)) continue;
            uiFillRect(x + origin + mx * scale, y + origin + my * scale,
                       scale, scale, ink::kBlack);
        }
    }

    return side;
}
