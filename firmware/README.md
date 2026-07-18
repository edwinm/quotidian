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
| Grayscale + anti-aliased text | [src/ui.cpp](src/ui.cpp) | Renders into a 4-bit PSRAM framebuffer |
| Improv Serial provisioning | [src/improv.cpp](src/improv.cpp) | Full protocol, shares the log port |
| Captive portal + QR | [src/portal.cpp](src/portal.cpp) | SoftAP, DNS wildcard, browser timezone |
| Credential storage | [src/settings.cpp](src/settings.cpp) | NVS via `Preferences` |
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

**Known weakness:** readings under 2.5 V are treated as "no battery" and the
footer shows `USB` with a dashed icon — but on hardware, a USB-powered board
with no cell attached reads at the 4.20 V clamp, not low. So the threshold does
not actually distinguish the two, and the footer will report 100% either way.
Telling them apart properly needs the charger's status line rather than a
voltage threshold.

### Verifying layout without looking at the screen

There is only one font, its line box is 51 px tall, and text width is easy to
underestimate — roughly **19 px per character**, not the ~14 you might guess.
Overflows are invisible from the build machine.

So text goes through `drawChecked()`, which measures before drawing and logs
any string that would leave its box:

```
[layout] OVERFLOW right: x=110 w=657 end=767 limit=685 "Scan the code and follow the page."
```

Two checks run. `drawChecked()` tests each string against its column bounds;
`uiDrawText()` additionally records every string drawn in a frame and reports
when a new one intersects an earlier one:

```
[layout] OVERLAP "Network: QuoteDisplay-8" (40,391-854,442) with "in Chrome or Edge."
```

The second check exists because the first is not sufficient: two strings can
each sit inside the screen and still land on top of each other. That shipped
once — bounds checking reported clean while the setup screen visibly overlapped.

Watch the serial monitor after a redraw; a clean boot prints no `[layout]`
lines. Anything containing user data — SSIDs especially — should additionally
go through `uiEllipsize()`, since no SSID length can be relied on.

This caught both bugs in the first setup-screen layout: the left column ran
past the footer rule and through the footer text, and the network name and key
overran the right screen edge.

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
