// Pick and render the quote for a given date, exactly as the device would.
//   node src/pick.js            -> today
//   node src/pick.js 2026-02-11 -> a specific date
//
// Rotation: if a day has several quotes they rotate per year, so the device
// shows a different one each year while staying stable within a day.

import { readFile } from 'node:fs/promises';

function pickForDate(dataset, date) {
  const key = `${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
  const list = dataset[key] || [];
  if (!list.length) return null;
  const idx = date.getFullYear() % list.length; // rotate per year
  return list[idx];
}

// ANSI bold for terminal preview.
const bold = (s) => `\x1b[1m${s}\x1b[0m`;

function render(entry) {
  const birth = entry.matched === 'birth' ? bold(entry.birthDisplay) : entry.birthDisplay;
  const death = entry.matched === 'death' ? bold(entry.deathDisplay) : entry.deathDisplay;
  return [
    '',
    `  ${entry.text}`,
    `  — ${entry.author}`,
    '',
    `  ${birth} – ${death}`,
    '',
    `  \x1b[2msource: ${entry.source} · ${entry.license}\x1b[0m`,
    '',
  ].join('\n');
}

async function main() {
  const dataset = JSON.parse(await readFile('data/quotes-by-day.json', 'utf8'));
  const arg = process.argv[2];
  const date = arg ? new Date(arg + 'T12:00:00') : new Date();
  const entry = pickForDate(dataset, date);
  if (!entry) {
    console.log('No quote for', date.toDateString());
    return;
  }
  console.log(render(entry));
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});

export { pickForDate };
