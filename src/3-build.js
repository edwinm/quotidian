// Step 3: turn people+quotes into a per-day dataset the device can consume,
// and report which calendar days have no quote.
//
// A person's quotes are placed on their birth day AND/OR their death day
// (whichever is day-precise). Each entry records which date "today" is, so the
// device can bold it.

import { readFile, writeFile } from 'node:fs/promises';

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
        buckets.get(dayKey).push({
          text: q.text,
          author: p.name,
          ...lifespan,
          matched, // 'birth' | 'death' — the date to render bold
          source: q.source,
          sourceUrl: q.sourceUrl,
          license: q.license,
          licenseUrl: q.licenseUrl,
          attributionRequired: q.attributionRequired,
        });
      }
    }
  }

  // Sort each bucket for deterministic rotation and write final dataset.
  const dataset = {};
  for (const [k, arr] of buckets) {
    arr.sort((a, b) => a.author.localeCompare(b.author) || a.text.localeCompare(b.text));
    dataset[k] = arr;
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
  console.log(`Total quote-instances: ${total}`);
  console.log(`Days covered: ${keys.length - empty.length}/${keys.length}`);
  console.log(`Per-day min/avg/max: ${min} / ${(total / keys.length).toFixed(1)} / ${max}`);

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
