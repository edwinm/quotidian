# Quotidian

*quotidian* — daily; of every day. The word already contains "quot".

A data pipeline that builds a per-day dataset of quotes by writers, poets and
philosophers, keyed to each author's **birth or death date**. A screen device can
show one quote each day, with the matching date rendered bold — so the quote
doubles as a date indicator.

Example (11 February):

> I think, therefore I am
> — René Descartes
>
> 31 March 1596 – **11 February 1650**

## Repository layout

Two separate pieces of software live here — Node.js on the desktop, C++ on the
device:

| Path | Language | What it is |
| --- | --- | --- |
| `pipeline/` | Node.js | Fetches and builds the quote dataset (see below) |
| `data/` | JSON | Output of the pipeline |
| `firmware/` | C++ | ESP32-S3 e-paper display — see [firmware/README.md](firmware/README.md) |
| `platformio.ini` | — | Firmware build config; at the root so PlatformIO finds it on open |

The firmware consumes this dataset: `npm run export:device` writes one file per
calendar day, and the display shows a quote whose author was born or died on
today's date. See [firmware/README.md](firmware/README.md).

## Getting started

You need the [LilyGo T5 4.7" e-paper board](https://lilygo.cc/products/t5-4-7-inch-e-paper-v2-3)
(ESP32-S3, **non-touch** version) and a USB-C data cable. No SD card — the
quotes live in the board's own flash.

The board is sold in touch and non-touch versions; this firmware targets the
non-touch one and never initialises a touch controller. It has to be the
ESP32-S3 variant — the older ESP32 board uses different pins for the panel and
the real-time clock.

There are two ways to get it onto the board: flash a ready-made release, which
needs nothing installed, or build it yourself.

### Flash a release

Download two files from the
[latest release](https://github.com/edwinm/quotidian/releases/latest):

| File | What it is | Flash at |
| --- | --- | --- |
| `quotidian-1.0-firmware.bin` | the program: bootloader, partition table and application in one | `0x0` |
| `quotidian-1.0-quotes.bin` | the quotes: 20 for every day of the year, as a LittleFS image | `0xC90000` |

**1. Put the board in download mode.** Connect USB, then hold **BOOT** (IO0),
tap **RST**, and release BOOT. The board has no power between its nightly
updates, so without this it will not show up on the computer at all.

**2. Flash both files.** In Chrome or Edge, open Espressif's browser flasher at
[espressif.github.io/esptool-js](https://espressif.github.io/esptool-js/),
click **Connect** and pick the board, then add both files with their addresses
from the table and click **Program**.

Or from a terminal, with [esptool](https://docs.espressif.com/projects/esptool/)
(`pip install esptool`):

```bash
python3 -m esptool --chip esp32s3 write_flash 0x0 quotidian-1.0-firmware.bin 0xC90000 quotidian-1.0-quotes.bin
```

**3. Cold boot it.** After flashing, the board stays in download mode and does
nothing — while the screen keeps showing its old image, so it looks as if it is
working. Disconnect USB, wait a few seconds, and reconnect it without touching
any button. With a battery attached, disconnect the battery as well: otherwise
there is no power cut and it stays in download mode.

The two files are independent. Flashing only the quotes file replaces the
quotes and leaves everything else alone. Flashing the firmware file erases the
saved Wi-Fi network, because it overwrites the settings area on its way to the
application, so the board comes up in setup mode again — see
[Connect it to Wi-Fi](#connect-it-to-wi-fi).

### Or build it yourself

You also need [Node.js](https://nodejs.org) and
[PlatformIO](https://platformio.org).

**1. Generate the quote files.**

```bash
npm install
npm run export:device                  # 366 day files, ~1.2 MB
```

`data/quotes-by-day.json` is already in the repo, so there is no need to re-run
the Wikidata/Wikiquote fetch unless you want fresher data.

**2. Build and flash.** Put the board in download mode as above, then write the
firmware and the quotes:

```bash
pio run -t upload                      # the application
pio run -t uploadfs                    # the quotes, into the flash filesystem
```

The two are independent: `uploadfs` replaces the dataset without touching the
application, so refreshing the quotes later does not mean reflashing the
firmware. Finish with a cold boot, as above.

On Apple Silicon without Rosetta, `uploadfs` fails with *Bad CPU type in
executable*: PlatformIO ships `mklittlefs` for Intel only. See
[firmware/README.md](firmware/README.md#where-the-dataset-lives-and-why-not-on-a-card)
for building a native one that the board can mount.

### Connect it to Wi-Fi

The display boots into setup mode and shows you how:
scan the QR code with a phone, or open
[improv-wifi.com](https://www.improv-wifi.com/) in Chrome while it is
plugged in. Nothing needs to be edited in the source, and the credentials are
stored on the device rather than in the repo.

The clock syncs over NTP, and the display then shows a quote by someone born or
died on today's date.

It then switches off until 00:10 the next night, when the on-board real-time
clock powers it up again to draw the new day. To move it to another network
later, hold the user button down and tap reset.

## Data sources

Two sources are combined:

- **[Wikidata](https://query.wikidata.org)** (SPARQL) — the people and their birth/
  death dates. We select occupations philosopher, poet, writer, playwright,
  novelist, essayist, with both dates known, and an English Wikiquote page.
- **[Wikiquote](https://en.wikiquote.org)** (English) — the quotes themselves.

Both are licensed **CC BY-SA 4.0**; every stored quote records this, so
attribution is always available (`license`, `licenseUrl`, `attributionRequired`).

## Only quotes that are certain

The extraction is deliberately strict — uncertain quotes are dropped, not shown:

- Only quotes in **trusted sections**. Sections named *Misattributed*, *Disputed*,
  *Attributed*, *Unsourced*, *"Quotes about…"*, and meta sections are skipped. A
  blocked top-level section stays blocked even if it has innocuous subheadings
  (this is what prevented a Newton quote leaking onto Descartes' page).
- Every quote must have a **source citation** in the wikitext. No citation → dropped.
- Sources that read *"Attributed to…"* are rejected.
- Editorial insertions (`[like this]`) are rejected.
- **English-only**: a stopword heuristic drops non-English quotes.
- Citations are cleaned to readable text (`{{cite book|…}}` templates parsed);
  anything still messy is dropped. Thin citations (`p. 12`) are enriched with the
  work title from the section heading.

## Pipeline

```bash
npm run fetch:people   # Wikidata  -> data/people.json
npm run fetch:quotes   # Wikiquote -> data/quotes-raw.json
npm run build          # bucket by day -> data/quotes-by-day.json + coverage report
npm run export:device  # split per day -> data/device/quotes/*.tsv (for the SD card)
npm run all            # all four in order
```

All steps retry on transient API errors / rate-limiting. If `fetch:quotes` prints
a `WARNING: N batch(es) failed`, the data is incomplete — just re-run it.

### Preview a day

```bash
node pipeline/pick.js              # today
node pipeline/pick.js 2026-02-11   # a specific date
```

## Output: `data/quotes-by-day.json`

An object keyed by `"MM-DD"` (all 366 days incl. 29 Feb). Each entry:

```json
{
  "text": "…the quote…",
  "author": "René Descartes",
  "birthDisplay": "31 March 1596",
  "deathDisplay": "11 February 1650",
  "matched": "death",                 // which date is "today" → render bold
  "source": "…citation…",
  "sourceUrl": "https://en.wikiquote.org/wiki/René_Descartes",
  "license": "CC BY-SA 4.0",
  "licenseUrl": "https://creativecommons.org/licenses/by-sa/4.0/",
  "attributionRequired": true
}
```

A person appears on **both** their birth day and death day (if both are known),
giving two chances to place them on the calendar.

### Rotation

If a day has several quotes they rotate per year (`index = year % count`), so the
device shows a different quote each year but a stable one within a day.

## Coverage

Every one of the 366 calendar days has quotes (currently 22–212 per day, ~41k
quote-instances total). `data/coverage-report.json` lists any empty or thin days —
if a future run produces gaps, that's where they'll show.

## Licence

The code is MIT — see [LICENSE](LICENSE).

Two bundled things are not: the font headers are derivative of Cabin and stay
under the SIL Open Font License, and the quotations are CC BY-SA 4.0 from
Wikiquote. [NOTICE](NOTICE) sets out both, and as an SPDX expression the
repository is `MIT AND OFL-1.1 AND CC-BY-SA-4.0`.
