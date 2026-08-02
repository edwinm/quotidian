// Shared helpers for the pipeline.

export const USER_AGENT =
  'QuoteOfTheDay/0.1 (https://github.com/; edwin@bitstorm.org) Node';

// Wikiquote text is licensed CC BY-SA; attribution is always required.
export const LICENSE = 'CC BY-SA 4.0';
export const LICENSE_URL = 'https://creativecommons.org/licenses/by-sa/4.0/';

export async function fetchJson(url, opts = {}) {
  const res = await fetch(url, {
    ...opts,
    headers: { 'User-Agent': USER_AGENT, Accept: 'application/json', ...(opts.headers || {}) },
  });
  if (!res.ok) throw new Error(`HTTP ${res.status} for ${url}`);
  return res.json();
}

export async function fetchText(url, opts = {}) {
  const res = await fetch(url, {
    ...opts,
    headers: { 'User-Agent': USER_AGENT, ...(opts.headers || {}) },
  });
  if (!res.ok) throw new Error(`HTTP ${res.status} for ${url}`);
  return res.text();
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Run tasks with limited concurrency and a small delay between starts (politeness).
export async function pool(items, worker, { concurrency = 4, onProgress } = {}) {
  const results = new Array(items.length);
  let i = 0;
  let done = 0;
  async function run() {
    while (i < items.length) {
      const idx = i++;
      try {
        results[idx] = await worker(items[idx], idx);
      } catch (err) {
        results[idx] = { error: String(err) };
      }
      done++;
      if (onProgress && done % 25 === 0) onProgress(done, items.length);
    }
  }
  await Promise.all(Array.from({ length: concurrency }, run));
  if (onProgress) onProgress(done, items.length);
  return results;
}

// --- Language ----------------------------------------------------------------

// Combining marks left by an NFD decomposition. Used only to tokenise for the
// language test - the text itself is never altered. The device is fed UTF-8
// exactly as Wikiquote wrote it, and the fonts are generated to cover it; see
// firmware/tools/fontconvert.py.
const COMBINING = /[\u0300-\u036f]/g;

// Function words dense enough in real English prose to identify it. Counting
// these beats looking for foreign markers, because the obvious foreign words
// are ambiguous: "die", "per", "con" and "non" are all English too.
const EN_STOPWORDS = new Set(
  ('the and of to a is in that it was for as with his be not this but are or my you i he she we they ' +
   'have has had will would can could should do does did at by from an their our your who what when which ' +
   'there here all no so if then than them me him her us its into out up down over about after before more ' +
   'most such only own same too very one two other some any each because while these those been being were am')
    .split(' '),
);

// The earlier version asked for two stopword hits anywhere in the quote, which
// Spanish clears without trying - "a", "no", "he" and "me" are all Spanish
// words too. Cervantes' "Y asi, del poco dormir y del mucho leer..." passed it
// and reached the device.
//
// Density is the fix. Real English runs 33-50% function words; the foreign text
// in this corpus runs 7-14%, so 20% separates them with room on both sides.
// Short quotes are exempt: "Genius is eternal patience" is a quarter stopwords
// by accident, and there is not enough text to judge.
//
// Diacritics are folded before tokenising, so that a word is bounded where it
// really ends. Without it "Duyen hoi ngo" written with its tone marks splits
// into the fragments between the accented letters, which measures nothing.
export function looksEnglish(text) {
  const folded = String(text ?? '').normalize('NFD').replace(COMBINING, '');
  const words = folded.toLowerCase().match(/[a-z']+/g) || [];
  if (words.length < 6) return true;
  const hits = words.filter((w) => EN_STOPWORDS.has(w)).length;
  return hits / words.length >= 0.2;
}

// --- Script coverage ---------------------------------------------------------

// The Unicode blocks the device fonts are generated from. Not a Latin-1 limit
// and not a substitution: text is shipped as UTF-8 exactly as written, and
// firmware/tools/charset.mjs derives the glyphs to build from the data itself.
//
// The boundary is the font family rather than the encoding. Cabin is a Latin
// face; it has outlines for these blocks and nothing for Japanese, so a CJK
// codepoint would come out of freetype as an empty box whatever intervals were
// requested. Rendering that honestly needs a second font and a fallback path in
// ui.cpp, which is a lot of machinery for the one quote in 7320 that needs it
// (Basho's haiku, which carries its own romaji transliteration anyway).
const SUPPORTED_BLOCKS = [
  [0x0020, 0x007e], // ASCII
  [0x00a0, 0x00ff], // Latin-1 Supplement
  [0x0100, 0x024f], // Latin Extended-A and Extended-B
  [0x02b0, 0x02ff], // Spacing modifier letters - the modifier apostrophe
  [0x1e00, 0x1eff], // Latin Extended Additional - Vietnamese
  [0x2000, 0x206f], // General Punctuation - dashes, curly quotes, ellipsis
];

export function isSupportedScript(text) {
  for (const ch of String(text ?? '')) {
    const cp = ch.codePointAt(0);
    if (!SUPPORTED_BLOCKS.some(([a, b]) => cp >= a && cp <= b)) return false;
  }
  return true;
}

// "1596-03-31" -> "MM-DD" key, plus formatted "31 March 1596".
const MONTHS = [
  'January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December',
];

// Wikidata dates look like "+1650-02-11T00:00:00Z" with a precision code.
// precision: 11 = day, 10 = month, 9 = year, etc.
export function parseWikidataDate(value, precision) {
  const m = /^([+-]?\d{1,})-(\d{2})-(\d{2})/.exec(value);
  if (!m) return null;
  let [, year, month, day] = m;
  const y = parseInt(year, 10);
  const mo = parseInt(month, 10);
  const d = parseInt(day, 10);
  const dayKnown = precision >= 11 && mo >= 1 && d >= 1;
  return {
    year: y,
    month: mo,
    day: d,
    dayKnown,
    dayKey: dayKnown ? `${String(mo).padStart(2, '0')}-${String(d).padStart(2, '0')}` : null,
    display: formatDate(y, mo, d, precision),
  };
}

export function formatDate(y, mo, d, precision) {
  const era = y < 0 ? ' BC' : '';
  const yy = Math.abs(y);
  if (precision >= 11 && mo >= 1 && d >= 1) return `${d} ${MONTHS[mo - 1]} ${yy}${era}`;
  if (precision >= 10 && mo >= 1) return `${MONTHS[mo - 1]} ${yy}${era}`;
  return `${yy}${era}`;
}
