# Firmware — LilyGo T5 4.7" e-paper (ESP32-S3)

A minimal example sketch for the 960×540 e-paper board, run in **portrait**:
it renders a quote in anti-aliased grayscale type, reads its content from the
microSD card, syncs the date over Wi-Fi, advertises the battery level over BLE,
and shows a status footer.

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

There is nothing to configure before flashing. Wi-Fi credentials are entered on
the device at first boot (see below) and stored in NVS, so they never touch
source control.

## Wi-Fi setup

With no credentials stored, the display boots into setup mode and puts the
instructions on screen. Two routes are offered, because no single one covers
every device:

**With a phone** — scan the QR code on the screen. It is a standard Wi-Fi join
payload, so iOS and Android cameras recognise it natively and connect to the
device's own access point; the captive portal then opens by itself. Pick your
network, type its password, done.

**With a computer** — connect USB and open
[improv-wifi.com/demo](https://www.improv-wifi.com/demo/) in Chrome or Edge.
This uses [Improv Serial](https://www.improv-wifi.com/serial/) over Web Serial:
no app, no network switching.

The phone route exists because Safari supports neither Web Serial nor Web
Bluetooth, and Apple requires every iOS browser to use WebKit — so on an
iPhone, a captive portal is the only app-free option.

### Changing networks

**Hold the front button for 3 seconds.** Credentials are erased and the device
restarts into setup mode. Same procedure if you move house or change router.

If stored credentials stop working, the device falls back to setup mode on its
own and the screen says which network it could not reach.

### Security note

The setup access point is WPA2 protected with a random password, regenerated on
every boot. You never type it — it is embedded in the QR code. This matters:
on an open access point, your home Wi-Fi password would cross the air
unencrypted while you submit the form.

### Timezone

You are never asked for one. The captive portal derives an exact POSIX TZ string
from the browser, including this year's DST transition rules, by probing
`Date.getTimezoneOffset()` across the year — that avoids shipping a 470-zone
lookup table.

Improv has no channel for timezone data, so devices provisioned over USB keep
`DEFAULT_TIMEZONE` from [src/config.h](src/config.h) until the portal is used.

## What each part demonstrates

| Feature | Where | Notes |
| --- | --- | --- |
| Grayscale + anti-aliased text | [src/ui.cpp](src/ui.cpp) | Own glyph blitter, rotated into portrait |
| Font generation | [tools/fontconvert.py](tools/fontconvert.py) | TrueType → 4-bit coverage bitmaps |
| Improv Serial provisioning | [src/improv.cpp](src/improv.cpp) | Full protocol, shares the log port |
| Captive portal + QR | [src/portal.cpp](src/portal.cpp) | SoftAP, DNS wildcard, browser timezone |
| Credential storage | [src/settings.cpp](src/settings.cpp) | NVS via `Preferences` |
| microSD | [src/storage.cpp](src/storage.cpp) | SPI, reads today's `/quotes/MM-DD.tsv` |
| Device export | [../pipeline/4-export-device.js](../pipeline/4-export-device.js) | 24 MB JSON → 366 small files |
| Battery indicator | [src/battery.cpp](src/battery.cpp) | ADC + eFuse Vref calibration |
| Wi-Fi + NTP | [src/wireless.cpp](src/wireless.cpp) | Station mode, timezone-aware |
| BLE | [src/wireless.cpp](src/wireless.cpp) | Standard Battery Service (0x180F) |

### Portrait orientation

Drawing happens in a **540×960 portrait canvas** that is rotated a quarter turn
counter-clockwise on its way to the landscape framebuffer. `ui.cpp` maps every
logical point with `px = ly, py = 539 - lx`.

If the picture is upside down for the way the device stands, flip
`UI_ROTATE_CCW` in [src/ui.cpp](src/ui.cpp) — it is a single `constexpr bool`.

Rotation is why `ui.cpp` draws glyphs itself instead of calling the display
library's `write_mode()`: that renderer only writes horizontally, into a buffer
whose stride is hard-coded to `EPD_WIDTH`. The blitter here walks each glyph's
4-bit coverage bitmap and places pixels through the rotating mapper, which keeps
the anti-aliasing intact. Rectangles take a shortcut — a rotated rectangle is
still a rectangle, so those map once and let the driver fill.

### Fonts

The display library ships one font. [tools/fontconvert.py](tools/fontconvert.py)
generates the rest from any TrueType file:

```bash
python3 tools/fontconvert.py FontBody 26 ~/Library/Fonts/Roboto-Regular.ttf \
    -o src/fonts/font_body.h
```

Six are checked in (Roboto regular and bold at 18, 26 and 36 px), covering ASCII
and Latin-1. Bitmaps are **uncompressed** on purpose — the rotating blitter reads
them directly and would otherwise need zlib. That costs about 154 kB of flash.

### Grayscale and anti-aliasing

The panel has 16 grey levels. The sketch allocates a `960 × 540 ÷ 2` byte
framebuffer in PSRAM (two 4-bit pixels per byte) and draws everything into it,
flushing once with `epd_draw_grayscale_image()`.

Text anti-aliasing comes for free from that choice. The generated glyph bitmaps
store per-pixel *coverage* rather than on/off bits, and `drawGlyph()` blends
that coverage from paper toward the foreground through a 16-entry ramp. Pixels
with zero coverage are skipped, so whatever is underneath shows through.

Two colour scales are in play, which is easy to trip over: shapes take 8-bit
values (`0x00`–`0xFF`), text takes 4-bit values (`0`–`15`). Both are named in
`ink::` in [src/ui.h](src/ui.h).

The generated fonts cover ASCII and Latin-1. Accented characters like `é` work;
typographic quotes like `“` do not, and fall back to `?`.

### Battery — the ADC2 caveat

On the ESP32-S3 the battery sense pin is on **ADC2**, and ADC2 is unavailable
while the Wi-Fi radio is running: reads come back as 0. So the sketch samples
the battery in `setup()` *before* `wifiBegin()`, and in `loop()` it stops Wi-Fi,
samples, then reconnects.

The sense divider is also only powered when the panel rail is on, hence the
`epd_poweron()` around the measurement.

**Known weakness:** readings under 2.5 V are treated as "no battery" and the
footer shows `USB` with a dashed icon — but on hardware, a USB-powered board
with no cell attached reads at the 4.20 V clamp, not low. So the threshold does
not actually distinguish the two, and the footer will report 100% either way.
Telling them apart properly needs the charger's status line rather than a
voltage threshold.

### Verifying layout without looking at the screen

Text is wider than you expect — at 36 px the type runs about 19 px per
character — and overflows are invisible from the build machine.

`uiDrawText()` therefore checks every string it draws, with no opt-in from
callers. It reports strings that leave the canvas:

```
[layout] OFF-CANVAS x: 110..767 (canvas 0..540) "Scan the code and follow"
```

and, separately, strings that land on top of earlier ones in the same frame:

```
[layout] OVERLAP "Network: QuoteDisplay-8" (40,391-854,442) with "in Chrome or Edge."
```

The second check exists because the first is not sufficient: two strings can
each sit inside the screen and still land on top of each other. That shipped
once — bounds checking reported clean while the setup screen visibly overlapped.

Watch the serial monitor after a redraw; a clean boot prints no `[layout]`
lines. Anything containing user data — SSIDs especially — should additionally
go through `uiEllipsize()`, since no SSID length can be relied on.

The overlap check exists because bounds checking alone reported a clean screen
while the setup layout was visibly broken: two strings each sat inside the
canvas and still collided. Both checks were confirmed to fire by reintroducing
the real defect and reflashing.

### Captive portal — why it does not connect inline

Submitting the form saves the credentials and answers immediately; the
connection attempt happens afterwards, from `loop()`.

That is deliberate. Bringing the station interface up retunes the radio to the
home network's channel, and in `WIFI_AP_STA` mode the access point follows it —
which kicks the phone off the setup network mid-request. Connecting inline
would mean the user often never sees the response. Instead the portal says
"saved, this network will disappear", and the **e-paper** reports the outcome.
Having a display is what makes that trade acceptable.

Improv does not have this problem: the USB link is unaffected by retuning
Wi-Fi, so it connects inline and reports status through the protocol.

### SD card and why the dataset lives there

The full dataset (`data/quotes-by-day.json`) is **24 MB**. It fits nowhere on
this device:

| Store | Capacity | Verdict |
| --- | --- | --- |
| Internal flash | 16 MB total, 6.5 MB app partition | too small |
| PSRAM | 8 MB | too small |
| SD card | GBs | fits easily |

So `npm run export:device` splits it into one file per calendar day, keeping
only the fields that get rendered. That comes to **7.8 MB across 366 files**,
the largest day being 40 kB. The device opens exactly one — so no more than
40 kB is ever in RAM, and nothing has to be streamed or indexed.

Worth knowing: at 7.8 MB the *exported* set would also fit in internal flash
with a custom partition table (single app slot, no OTA, ~13 MB FFat). The SD
card was kept because the dataset can then be refreshed by swapping a card
instead of reflashing.

Copy the export to the card root as `/quotes/`:

```bash
npm run export:device
cp -r data/device/quotes /Volumes/YOUR_CARD/
```

The format is tab-separated, one quote per line, in the field order
`text, author, datesPrefix, datesBold, datesSuffix, attribution`. It is not
JSON on purpose — the device needs no parser, just a line count and a split on
tabs.

### Picking today's quote

Every quote is keyed to the day of its author's birth or death, so the file for
today's date holds only people connected to today. Days contain 22 to 212
quotes; one is chosen by `year % count`, which is stable within a day and moves
on from one year to the next.

The **day and month that match today are drawn bold and black**, while the years
stay regular and grey — so the tie between the quote and the date in the header
is visible without reading it:

> 1122 &ndash; **18 July** 1192

The pre-split is done by the exporter, not the firmware, so the device never
parses a date string. Years can carry a `BC` suffix, which that split handles.

Without a synced clock the calendar day is genuinely unknown. Rather than guess,
the device shows its built-in quote — note that its bold date will *not* be
today, since no day was matched.

### Attribution

The corpus is CC BY-SA 4.0 and `attributionRequired` is true for **all 41 363**
quotes, so the credit line always renders:

> Wikiquote &middot; CC BY-SA 4.0

Author and licence are shown alongside the quote itself. The flag is still
honoured per quote rather than hard-coded, so a future source that does not
require attribution simply renders without it.

## Refresh behaviour

A full-screen e-paper refresh takes roughly 1.5 s and flashes the panel, so the
sketch redraws only every 10 minutes (`REFRESH_INTERVAL_MS`). BLE clients get
battery notifications on that same cadence.
