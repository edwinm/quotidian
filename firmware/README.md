# Firmware — LilyGo T5 4.7" e-paper (ESP32-S3)

A minimal example sketch for the 960×540 e-paper board: it renders a quote in
anti-aliased grayscale type, reads its content from the microSD card, syncs the
date over Wi-Fi, advertises the battery level over BLE, and shows a status
footer.

This targets the **non-touch** board revision — no touch controller is
initialised or required.

## Build

From the **repository root** (that is where `platformio.ini` lives, so the
project is recognised the moment the repo is opened):

```bash
pio run -t upload -t monitor
```

The e-paper driver library is pulled from GitHub automatically; nothing needs
to be vendored. `platformio.ini` points `src_dir` and `boards_dir` back into
this directory — if you move either, update those two paths.

Before flashing, set your network in [src/config.h](src/config.h):

```c
#define WIFI_SSID     "your-network"
#define WIFI_PASSWORD "your-password"
```

Leaving `WIFI_SSID` empty is fine — the sketch then stays offline and falls back
to the build date instead of NTP.

## What each part demonstrates

| Feature | Where | Notes |
| --- | --- | --- |
| Grayscale + anti-aliased text | [src/ui.cpp](src/ui.cpp) | Renders into a 4-bit PSRAM framebuffer |
| microSD | [src/storage.cpp](src/storage.cpp) | SPI, reads `/quote.txt` |
| Battery indicator | [src/battery.cpp](src/battery.cpp) | ADC + eFuse Vref calibration |
| Wi-Fi + NTP | [src/wireless.cpp](src/wireless.cpp) | Station mode, timezone-aware |
| BLE | [src/wireless.cpp](src/wireless.cpp) | Standard Battery Service (0x180F) |

### Grayscale and anti-aliasing

The panel has 16 grey levels. The sketch allocates a `960 × 540 ÷ 2` byte
framebuffer in PSRAM (two 4-bit pixels per byte) and draws everything into it,
flushing once with `epd_draw_grayscale_image()`.

Text anti-aliasing comes for free from that choice. The bundled FiraSans glyph
bitmaps store per-pixel *coverage* rather than on/off bits, and the renderer
blends foreground into background through a 16-entry LUT — but only when a
framebuffer is supplied. Passing `NULL` for direct drawing collapses the type
to 1-bit, so `uiDrawText()` always renders into the buffer.

Two colour scales are in play, which is easy to trip over: shapes take 8-bit
values (`0x00`–`0xFF`), text takes 4-bit values (`0`–`15`). Both are named in
`ink::` in [src/ui.h](src/ui.h).

The bundled font covers ASCII and Latin-1 only (plus box-drawing and emoji
blocks). Accented characters like `é` work; typographic quotes like `“` do not
and render as `?`.

### Battery — the ADC2 caveat

On the ESP32-S3 the battery sense pin is on **ADC2**, and ADC2 is unavailable
while the Wi-Fi radio is running: reads come back as 0. So the sketch samples
the battery in `setup()` *before* `wifiBegin()`, and in `loop()` it stops Wi-Fi,
samples, then reconnects.

The sense divider is also only powered when the panel rail is on, hence the
`epd_poweron()` around the measurement.

With no battery attached the divider floats low; readings under 2.5 V are
treated as "no battery" and the footer shows `USB` with a dashed icon.

### SD card

Copy [sdcard/quote.txt](sdcard/quote.txt) to the root of a FAT32-formatted
card. The format is three lines — quote, author, source — with the third
optional:

```
The unexamined life is not worth living
Socrates
Plato, Apology, 38a
```

No card, no file, or an empty file all fall back to a built-in quote, so the
board still shows something sensible on a bare desk.

## Refresh behaviour

A full-screen e-paper refresh takes roughly 1.5 s and flashes the panel, so the
sketch redraws only every 10 minutes (`REFRESH_INTERVAL_MS`). BLE clients get
battery notifications on that same cadence.
