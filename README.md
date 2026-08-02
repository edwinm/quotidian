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

The firmware consumes this dataset: `npm run export:device` splits it into one
file per calendar day for the SD card, and the display shows a quote whose
author was born or died on today's date. See
[firmware/README.md](firmware/README.md).

## Getting started

You need the [LilyGo T5 4.7" e-paper board](https://lilygo.cc/products/t5-4-7-inch-e-paper-v2-3)
(ESP32-S3, **non-touch** version), a microSD card, [Node.js](https://nodejs.org)
and [PlatformIO](https://platformio.org).

The board is sold in touch and non-touch versions; this firmware targets the
non-touch one and never initialises a touch controller. It has to be the
ESP32-S3 variant — the older ESP32 board uses different pins for the panel, the
SD card and the real-time clock.

**1. Prepare the SD card.** It must be **FAT32** — the ESP32's SD library cannot
read exFAT, which is how cards larger than 32 GB usually ship. On macOS, find
the disk and erase it:

```bash
diskutil list                                      # find your card, e.g. /dev/disk10
diskutil eraseDisk FAT32 QUOTES MBRFormat /dev/disk10
```

Check the size in `diskutil list` before erasing — the command destroys
everything on the disk it is given, and getting the identifier wrong means
erasing something else.

**2. Copy the quotes onto it.**

```bash
npm install
npm run export:device                  # 366 day files, ~7.8 MB
COPYFILE_DISABLE=1 cp -r data/device/quotes /Volumes/QUOTES/
```

`COPYFILE_DISABLE=1` matters on macOS: without it, Finder and `cp` write an
`._name` resource fork beside every file on a FAT volume, doubling the file
count. The device ignores them, but they are pure clutter. If you already
copied without it, `find /Volumes/QUOTES -name "._*" -delete` cleans up.

`data/quotes-by-day.json` is already in the repo, so there is no need to re-run
the Wikidata/Wikiquote fetch unless you want fresher data.

**3. Build and flash.** Insert the card in the board, connect USB:

```bash
pio run -t upload -t monitor
```

**4. Connect it to Wi-Fi.** The display boots into setup mode and shows you how:
scan the QR code with a phone, or open
[improv-wifi.com](https://www.improv-wifi.com/) in Chrome while it is
plugged in. Nothing needs to be edited in the source, and the credentials are
stored on the device rather than in the repo.

The clock syncs over NTP, and the display then shows a quote by someone born or
died on today's date.

It then sleeps until 00:10 the next night, when the on-board real-time clock
wakes it to draw the new day. A short press of the user button wakes it early
and redraws. To move it to another network later, hold that button down and tap
reset.

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
