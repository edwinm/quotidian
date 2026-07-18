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
