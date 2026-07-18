// Split the day-keyed dataset into one small file per calendar day, in a format
// the firmware can read without a JSON parser.
//
// The full dataset is ~24 MB: too large for the ESP32's 16 MB flash, and larger
// than its 8 MB of PSRAM, so it cannot live on the device in either form. Only
// one day is ever needed at a time, so it is split into 366 files on the SD
// card. Each is a few tens of kB and holds only the fields that get rendered.
//
// The format is tab-separated, one quote per line, because the device then
// needs no parser at all: count the lines, seek to the one for this year, split
// on tabs. Values have tabs and newlines stripped so the framing cannot break.
//
//   npm run export:device

import { mkdir, readFile, writeFile, rm } from 'node:fs/promises';
import path from 'node:path';

const OUT_DIR = 'data/device/quotes';

// Order must match Quote parsing in firmware/src/storage.cpp.
const FIELDS = [
  'text',
  'author',
  'datesPrefix',
  'datesBold',
  'datesSuffix',
  'attribution',
];

// Strips the framing characters. Used for every field.
function strip(value) {
  return String(value ?? '').replace(/[\t\r\n]+/g, ' ');
}

// Also trims. Not used on the date runs, whose leading and trailing spaces are
// what separate them when the device draws the three parts back to back.
function clean(value) {
  return strip(value).trim();
}

// The matched date is what ties the quote to today, so it is rendered bold -
// but only the day and month, since the year is not what matches. Splitting
// here keeps the firmware from having to parse date strings.
//
// "18 July 1192" -> bold "18 July", suffix " 1192"
// "23 January 3101 BC" -> bold "23 January", suffix " 3101 BC"
function splitMatchedDate(display) {
  const parts = display.split(' ');
  if (parts.length >= 3 && /^\d{1,2}$/.test(parts[0])) {
    return { bold: `${parts[0]} ${parts[1]}`, suffix: ` ${parts.slice(2).join(' ')}` };
  }
  // Year-only or an unexpected shape: bold the lot rather than guess.
  return { bold: display, suffix: '' };
}

// "1122 - [18 July] 1192", with the bracketed run drawn bold on the device.
function buildDates(quote) {
  const birth = clean(quote.birthDisplay);
  const death = clean(quote.deathDisplay);
  const matchedIsBirth = quote.matched === 'birth';
  const matched = matchedIsBirth ? birth : death;
  const other = matchedIsBirth ? death : birth;

  if (!matched) return { prefix: '', bold: '', suffix: '' };

  const { bold, suffix } = splitMatchedDate(matched);

  // An en dash is outside Latin-1, which is all the device fonts cover.
  if (matchedIsBirth) {
    return { prefix: '', bold, suffix: other ? `${suffix} - ${other}` : suffix };
  }
  return { prefix: other ? `${other} - ` : '', bold, suffix };
}

function buildAttribution(quote) {
  if (!quote.attributionRequired) return '';

  let work = 'Wikiquote';
  try {
    const host = new URL(quote.sourceUrl).hostname;
    if (host.endsWith('wikiquote.org')) work = 'Wikiquote';
    else work = host.replace(/^www\./, '');
  } catch {
    // Keep the default.
  }
  // Middle dot is U+00B7, inside Latin-1 and so renderable on the device.
  return clean(`${work} · ${quote.license ?? 'CC BY-SA 4.0'}`);
}

async function main() {
  const dataset = JSON.parse(await readFile('data/quotes-by-day.json', 'utf8'));

  await rm(OUT_DIR, { recursive: true, force: true });
  await mkdir(OUT_DIR, { recursive: true });

  let totalBytes = 0;
  let totalQuotes = 0;
  let largest = { day: null, bytes: 0 };

  for (const day of Object.keys(dataset).sort()) {
    const lines = dataset[day].map((quote) => {
      const dates = buildDates(quote);
      const row = {
        text: clean(quote.text),
        author: clean(quote.author),
        datesPrefix: dates.prefix,
        datesBold: dates.bold,
        datesSuffix: dates.suffix,
        attribution: buildAttribution(quote),
      };
      return FIELDS.map((f) => strip(row[f])).join('\t');
    });

    const body = lines.join('\n') + '\n';
    const file = path.join(OUT_DIR, `${day}.tsv`);
    await writeFile(file, body, 'utf8');

    totalBytes += Buffer.byteLength(body);
    totalQuotes += lines.length;
    if (Buffer.byteLength(body) > largest.bytes) {
      largest = { day, bytes: Buffer.byteLength(body) };
    }
  }

  const mb = (n) => (n / 1024 / 1024).toFixed(1);
  console.log(`Wrote ${OUT_DIR}/ - 366 files, ${totalQuotes} quotes, ${mb(totalBytes)} MB total`);
  console.log(`Largest day: ${largest.day} at ${(largest.bytes / 1024).toFixed(0)} kB`);
  console.log('Copy data/device/quotes/ to the root of the SD card as /quotes/.');
}

main();
