// Step 3: turn people+quotes into a per-day dataset the device can consume,
// and report which calendar days have no quote.
//
// A person's quotes are placed on their birth day AND/OR their death day
// (whichever is day-precise). Each entry records which date "today" is, so the
// device can bold it.
//
// Each day is then cut to the best QUOTES_PER_DAY, ranked by score.js. The cut
// happens here rather than at the device export so that the dataset and the
// device agree: pick.js previews from this file and claims to show "exactly as
// the device would", which stops being true the moment the two hold different
// lists. data/quotes-raw.json keeps the full corpus if the ranking needs
// revisiting.

import { readFile, writeFile } from 'node:fs/promises';

import { isSupportedScript, looksEnglish } from './common.js';
import { pickBest, scoreQuote } from './score.js';

// Twenty is what the device's filesystem budget allows, and is also about the
// most a rotation can use: one a year, so twenty is a twenty-year cycle.
const QUOTES_PER_DAY = 20;

// No author may hold more than this many of a day's slots. See pickBest().
const MAX_PER_PERSON = 3;

const MONTHS = [
  'January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December',
];
const DAYS_IN_MONTH = [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];

function allDayKeys() {
  const keys = [];
  for (let m = 1; m <= 12; m++)
    for (let d = 1; d <= DAYS_IN_MONTH[m - 1]; d++)
      keys.push(`${String(m).padStart(2, '0')}-${String(d).padStart(2, '0')}`);
  return keys;
}

async function main() {
  const people = JSON.parse(await readFile('data/quotes-raw.json', 'utf8'));

  const buckets = new Map(allDayKeys().map((k) => [k, []]));
  let rejected = 0;

  for (const p of people) {
    const lifespan = {
      birthDisplay: p.birth?.display ?? null,
      deathDisplay: p.death?.display ?? null,
    };
    const targets = [];
    if (p.birth?.dayKnown) targets.push([p.birth.dayKey, 'birth']);
    if (p.death?.dayKnown) targets.push([p.death.dayKey, 'death']);

    for (const [dayKey, matched] of targets) {
      for (const q of p.quotes) {
        // Step 2 applies this too, but only to what it fetches. The corpus on
        // disk predates the stricter test, and re-running step 2 means refetching
        // 5000 Wikiquote pages - so it is enforced here as well. Cheap, and it
        // keeps a stale data/quotes-raw.json from reaching the device.
        if (!looksEnglish(q.text) || !isSupportedScript(q.text)) {
          rejected++;
          continue;
        }
        buckets.get(dayKey).push({
          text: q.text,
          author: p.name,
          ...lifespan,
          matched, // 'birth' | 'death' — the date to render bold
          sitelinks: p.sitelinks, // fame proxy; ranked on in score.js
          source: q.source,
          sourceUrl: q.sourceUrl,
          license: q.license,
          licenseUrl: q.licenseUrl,
          attributionRequired: q.attributionRequired,
        });
      }
    }
  }

  // Rank each day and keep the best, best first. pickBest is deterministic, so
  // a rebuild produces the same list and the rotation does not shift under the
  // device's feet.
  const dataset = {};
  let considered = 0;
  for (const [k, arr] of buckets) {
    considered += arr.length;
    dataset[k] = pickBest(arr, { limit: QUOTES_PER_DAY, maxPerPerson: MAX_PER_PERSON });
  }
  await writeFile('data/quotes-by-day.json', JSON.stringify(dataset, null, 2));

  // Coverage report.
  const keys = allDayKeys();
  const empty = keys.filter((k) => dataset[k].length === 0);
  const counts = keys.map((k) => dataset[k].length);
  const total = counts.reduce((a, b) => a + b, 0);
  const min = Math.min(...counts);
  const max = Math.max(...counts);

  console.log('\n=== Coverage ===');
  console.log(`Rejected as non-English or unsupported script: ${rejected} quote-instances`);
  console.log(`Considered: ${considered} quote-instances`);
  console.log(`Kept: ${total} (best ${QUOTES_PER_DAY}/day, max ${MAX_PER_PERSON} per author)`);
  console.log(`Days covered: ${keys.length - empty.length}/${keys.length}`);
  console.log(`Per-day min/avg/max: ${min} / ${(total / keys.length).toFixed(1)} / ${max}`);

  // The weakest quote that still made it in. If this is high the ranking has
  // room to be stricter; if it is low, some days are scraping the barrel.
  const worstKept = keys
    .filter((k) => dataset[k].length)
    .map((k) => ({ day: k, q: dataset[k][dataset[k].length - 1] }))
    .sort((a, b) => scoreQuote(a.q) - scoreQuote(b.q));
  if (worstKept.length) {
    const { day, q } = worstKept[0];
    console.log(
      `\nWeakest quote kept: ${day}, score ${scoreQuote(q).toFixed(0)} — ${q.author}`,
    );
    console.log(`  "${q.text.slice(0, 80)}${q.text.length > 80 ? '…' : ''}"`);
  }

  if (empty.length) {
    console.log(`\nDays with NO quote (${empty.length}):`);
    const pretty = empty.map((k) => {
      const [m, d] = k.split('-').map(Number);
      return `${d} ${MONTHS[m - 1]}`;
    });
    console.log('  ' + pretty.join(', '));
  } else {
    console.log('\nEvery calendar day has at least one quote. 🎉');
  }

  // Also flag thin days (only 1 quote) — vulnerable if we later reject a quote.
  const thin = keys.filter((k) => dataset[k].length === 1);
  if (thin.length) {
    console.log(`\nThin days (exactly 1 quote, ${thin.length}): may want backups`);
  }

  await writeFile(
    'data/coverage-report.json',
    JSON.stringify({ total, covered: keys.length - empty.length, empty, thin: thin }, null, 2),
  );
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
