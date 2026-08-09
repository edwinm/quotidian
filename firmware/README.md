# Quotidian firmware — LilyGo T5 4.7" e-paper (ESP32-S3)

A minimal example sketch for the 960×540 e-paper board, run in **portrait**:
it renders a quote in anti-aliased grayscale type, reads its content from the
internal flash, syncs the date over Wi-Fi, advertises the battery level over BLE,
and shows a status footer.

This targets the **non-touch** revision of the
[LilyGo T5 4.7" e-paper board](https://lilygo.cc/products/t5-4-7-inch-e-paper-v2-3)
— no touch controller is initialised or required.

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
[improv-wifi.com](https://www.improv-wifi.com/) in Chrome or Edge.
This uses [Improv Serial](https://www.improv-wifi.com/serial/) over Web Serial:
no app, no network switching.

The phone route exists because Safari supports neither Web Serial nor Web
Bluetooth, and Apple requires every iOS browser to use WebKit — so on an
iPhone, a captive portal is the only app-free option.

### Changing networks

**Hold the user button (S4) down and tap reset.** Credentials are erased and the
device restarts into setup mode. Same procedure if you move house or change
router.

It has to be that gesture rather than a press while running, because in normal
operation the board is only powered for a few seconds a night — and it is *off*
the rest of the time, not asleep, so pressing a button does nothing at all. The
board has no power to notice it with. Holding it across a reset is the one
moment the firmware is guaranteed to look.

S4 is configured as an `ext1` wake source in `powerDown()`, which reads as
though a press should wake the device. It does not: see the section on the
power model below.

Presses do work whenever the board happens to be running anyway — during setup
mode, or throughout when `DEEP_SLEEP_ENABLED` is 0. A short press redraws the
current day, which is the quick way to refresh the `SHOW_POWER_DIAGNOSTICS`
lines while developing.

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
| Storage | [src/storage.cpp](src/storage.cpp) | LittleFS in internal flash, reads today's `/quotes/MM-DD.tsv` |
| Device export | [../pipeline/4-export-device.js](../pipeline/4-export-device.js) | Best 20 per day → 366 small files, 1.2 MB |
| Battery indicator | [src/battery.cpp](src/battery.cpp) | ADC + eFuse Vref calibration |
| Wi-Fi + NTP | [src/wireless.cpp](src/wireless.cpp) | Station mode, timezone-aware |
| BLE | [src/wireless.cpp](src/wireless.cpp) | Standard Battery Service (0x180F) |
| Nightly power-down | [src/power.cpp](src/power.cpp) | The rail drops; the RTC alarm switches it back on |

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
python3 tools/fontconvert.py FontBody 26 Cabin-Regular.ttf -o src/fonts/font_body.h
```

The generated headers are checked in; the TrueType sources are not, so a
regeneration starts by fetching Cabin from its OFL home and cutting the two
static instances (see the variable-font note below):

```bash
pip install freetype-py fonttools
curl -sSL -o "Cabin[wdth,wght].ttf" \
  'https://github.com/google/fonts/raw/main/ofl/cabin/Cabin%5Bwdth%2Cwght%5D.ttf'
python3 -m fontTools.varLib.instancer "Cabin[wdth,wght].ttf" wght=400 wdth=100 -o Cabin-Regular.ttf
python3 -m fontTools.varLib.instancer "Cabin[wdth,wght].ttf" wght=700 wdth=100 -o Cabin-Bold.ttf
```

All six, at the sizes [src/ui.h](src/ui.h) declares:

```bash
python3 tools/fontconvert.py FontSmall     18 Cabin-Regular.ttf -o src/fonts/font_small.h
python3 tools/fontconvert.py FontSmallBold 18 Cabin-Bold.ttf    -o src/fonts/font_small_bold.h
python3 tools/fontconvert.py FontBody      26 Cabin-Regular.ttf -o src/fonts/font_body.h
python3 tools/fontconvert.py FontBodyBold  26 Cabin-Bold.ttf    -o src/fonts/font_body_bold.h
python3 tools/fontconvert.py FontLarge     36 Cabin-Regular.ttf -o src/fonts/font_large.h
python3 tools/fontconvert.py FontTitle     36 Cabin-Bold.ttf    -o src/fonts/font_title.h
```

Check the reported `advance_y`/`ascender`/`descender` against what they were:
the layout constants in [src/main.cpp](src/main.cpp) are tuned to them, so a
metric that moves silently reflows every screen.

Six are checked in — **Cabin** regular and bold at 18, 26 and 36 px, 227 glyphs
each. Bitmaps are **uncompressed** on purpose: the rotating blitter reads them
directly and would otherwise need zlib. That costs about 176 kB of flash.

#### The glyph set is generated from the data, not guessed

The device is fed UTF-8 exactly as the pipeline produces it. Nothing is
transliterated down to fit the fonts — the fonts are built to fit the text:

```bash
npm run export:device
node firmware/tools/charset.mjs      # prints the INTERVALS list
node firmware/tools/checkcoverage.mjs  # fails if any character has no glyph
```

`charset.mjs` walks the exported day files and emits the exact
`INTERVALS` for [tools/fontconvert.py](tools/fontconvert.py); paste it in and
regenerate. Beyond ASCII and Latin-1 this corpus needs 30 more codepoints —
accented names from Polish, Vietnamese, Turkish and Māori, plus the em dashes,
curly quotes and ellipses Wikiquote writes.

`checkcoverage.mjs` reads the intervals back out of the *generated header*, so
it verifies what was actually built rather than what was intended, and is worth
running after any dataset change. A missing glyph is otherwise invisible until
it shows up as a `?` on the panel weeks later — which is exactly what had been
happening to one quote in twelve, every em dash and curly apostrophe in the set.

Two traps, both of which have already cost time here:

- **Do not widen the intervals speculatively.** Bridging small gaps pulled in
  U+2016, U+2017 and U+201B, which Cabin has no outline for, so they baked in as
  empty `.notdef` boxes. `fontconvert.py` now warns when a requested codepoint
  is absent from the face.
- **Cabin is a Latin face.** It has nothing for CJK, so a Japanese codepoint
  would come out as a box whatever is requested. The pipeline rejects text
  outside the Latin blocks (`isSupportedScript` in
  [../pipeline/common.js](../pipeline/common.js)) — one quote in 7320, Bashō's
  haiku, which carries its own romaji anyway. Real CJK would need a second font
  and a fallback path in `ui.cpp`.

Cabin (Pablo Impallari) sits in the humanist tradition of Edward Johnston and
Eric Gill, which suits the 1930s frame the display lives in while staying
legible at 18 px. Gill Sans itself was used first and reads slightly better
still, but it is proprietary and cannot ship under MIT. Of the period faces,
Futura-derived designs like Jost* have a lower x-height and rounder, less
distinguishable letters — a real cost on the metadata line — and the Art Deco
Didones (Bodoni, Didot) have hairlines thinner than a pixel at that size and
break up entirely on this panel.

**Licensing.** Cabin is OFL, and these headers hold its rasterised outlines, so
they are a derivative of the font: [src/fonts/OFL.txt](src/fonts/OFL.txt)
applies to them, while the code is MIT. OFL permits exactly this kind of
bundling; it only requires that the notice travels with the font data. See
[../NOTICE](../NOTICE).

**Variable fonts need instantiating first.** Cabin ships from Google Fonts only
as a variable font, and neither `set_var_named_instance()` nor
`set_var_design_coords()` moved the weight axis in this freetype build — regular
and bold came out byte-identical and the mistake was invisible until the file
sizes were compared. Cut static instances with fonttools instead:

```bash
python3 -m fontTools.varLib.instancer "Cabin[wdth,wght].ttf" wght=700 wdth=100 \
    -o Cabin-Bold.ttf
```

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

`get_glyph()` falls back to `?` for any codepoint the font lacks, so a gap in
the interval set is silent everywhere except the panel. `checkcoverage.mjs`
exists to make that failure loud; see the font section above.

### Flashing: finish with a cold boot, no buttons

After `pio run -t upload` this board usually stays in ROM download mode instead
of running what was just written. The screen keeps its last image, so it looks
like a working device while nothing is executing — no render, no alarm, nothing
overnight. That cost a night of testing before it was understood.

Always end a flash with: **unplug USB, wait a few seconds, plug back in without
touching any button.** That is a clean power-on with GPIO0 high, which starts
the application. Holding BOOT during the replug does the opposite — it is how
you deliberately enter download mode to flash in the first place.

Do not open the serial port to check. On this board that resets the chip, often
straight back into download mode; the on-panel diagnostics exist because of
this.

Watching whether the USB port disappears tells you the application ran and
ended its cycle, and nothing more. It does **not** tell you the chip went to
sleep: USB CDC vanishes just the same when the board switches off. Reading more
than that into it is how the sleep model came to be documented backwards for a
while — see below.

### The board switches off, it does not sleep

This has now been diagnosed twice, wrongly the second time, so both the answer
and the way the evidence misled are worth keeping.

`powerDown()` calls `esp_deep_sleep_start()`, but that is not what happens.
The rail drops and the PCF8563 alarm brings it back: the board is switched off
in between, not asleep.

Measured on the panel, on the boot the alarm itself caused, with nothing
attached and nobody touching it:

```
rst 1 POWERON  wake 0 NONE 0x0
alarm 0  timer 0  btn 0  nomask 0  other 3
```

`ESP_RST_POWERON` means the supply cycled — not an external reset
(`ESP_RST_EXT`), not a brownout (`ESP_RST_BROWNOUT`). `wake 0 NONE` with an
empty ext1 mask means the chip did not resume from sleep at all. RTC memory
does not survive either, which is why persistent state lives in NVS.

**How the second diagnosis went wrong.** Those same readings were once
dismissed as artifacts of the serial monitor toggling `EN` over USB-JTAG,
and the board was declared to deep sleep after all. The evidence for that was
a port-presence test: USB vanished for 126 s and came back. It proves nothing.
**USB CDC disappears identically whether the chip deep sleeps or the board
powers off**, so the test could never separate the two — and it was written up
as though it had. The `SHOW_POWER_DIAGNOSTICS` line above is what finally
distinguished them, because it reports the reset reason instead of inferring it.

The consequences:

- **The RTC alarm is the only way back.** No GPIO wake — a board with no power
  cannot notice a button. No timer — nothing is running to count. Everything
  rests on the alarm having been armed correctly, which is why
  `rtcSetDailyAlarmUtc()` reads its registers back and only reports `verified`
  once they hold what was written. That read-back is the safety net; there is
  no second one.
- **`esp_sleep_enable_timer_wakeup()` is useless here** and is not called. It
  was added as a backstop while the deep-sleep model was believed, and would
  have sat in the code reading as protection that cannot fire.
- **An alarm start is indistinguishable from a reset** by reset reason alone,
  so `rtcAlarmFired()` reads the chip's own alarm flag instead, before clearing
  it. `powerWakeSource()` can only ever return `Other`; its `Alarm`, `Timer`
  and `Button` cases exist to prove that, and their counters staying at zero is
  the expected result rather than a fault.
- **Between updates the board is unreachable**, including for flashing. Hold
  `IO0` and tap reset to get into the ROM bootloader.

Being properly off should mean a standby current near zero, which makes the
week-long battery death that started this investigation a load on the always-on
side rather than a sleep-current problem. The microSD card was exactly that,
and it has been removed; see the storage section.

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
[layout] OVERLAP "Network: Quotidian-8" (40,391-854,442) with "in Chrome or Edge."
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

### Where the dataset lives, and why not on a card

The corpus behind the dataset (`data/quotes-raw.json`) is 12 MB and 41,000
quote-instances, which fits nowhere on this device. But the device does not
need the corpus — it needs one quote a day.

Step 3 ranks each calendar day and keeps only the best 20 (see
[../pipeline/score.js](../pipeline/score.js)), and `npm run export:device`
writes those as one file per day, holding only the fields that get rendered.
That comes to **1.2 MB across 366 files**, the largest day being 4 kB. The
device opens exactly one, so no more than 4 kB is ever in RAM and nothing has
to be streamed or indexed.

Twenty is not an arbitrary cut. The rotation shows one quote per day per year,
so a day's list *is* a twenty-year cycle — beyond that the extra quotes are
never reached within the life of the device, and below it the same quote comes
round too often.

| Store | Capacity | Verdict |
| --- | --- | --- |
| **Internal flash, stock table** | 3.375 MB filesystem partition | **used** — 1.43 MB in 4 kB LittleFS blocks, 42% full |
| SD card | GBs | works, but cannot be powered down |
| Internal flash, no OTA slot | ~11.9 MB | unnecessary now |
| PSRAM | 8 MB | volatile, not a store |

It started on an SD card, and the shrink is what let it move. At 8.5 MB the
export only fitted internal flash with a custom partition table; at 1.2 MB it
fits the **stock** one, leaving the unused 6.25 MB OTA slot alone.

The card had to go because it sits on the unswitched 3V3 rail and cannot be
turned off in software, which made it the prime suspect for the sleep current
that flattened a 2000 mAh cell in a week. The slot is still on the board, and
`parkSdCardPins()` still releases its lines before sleep in case a card is left
in it.

The one property the card had worth keeping was that the data could be replaced
without reflashing the firmware. `uploadfs` preserves it:

```bash
npm run export:device     # regenerate, and verify every glyph exists
pio run -t buildfs        # pack the image, and check it fits
pio run -t uploadfs       # write it, without touching the application
```

**`data_dir` must be set in the `[platformio]` section**, and is, in
[../platformio.ini](../platformio.ini). `board_build.data_dir` inside the
environment looks plausible, is accepted in silence, and does nothing — the
image is then built from the default `data/`, which here means trying to flash
the 12 MB corpus and failing with `No more free space`.

Without the dataset the device still runs: it logs `/quotes/MM-DD.tsv not found`
and falls back to `/quote.txt`, then to a built-in quote. `LittleFS.begin()` is
called with `formatOnFail = false` on purpose — a missing filesystem and an
empty one look the same to the caller, and formatting would destroy the dataset
to paper over what is usually a bad flash.

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
