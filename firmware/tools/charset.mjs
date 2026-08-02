// Reports every codepoint the exported device data actually uses, as the
// INTERVALS list that fontconvert.py wants.
//
// The fonts are generated to fit the text, not the other way round. Run this
// after `npm run export:device`; if it prints intervals that differ from the
// ones in fontconvert.py, the fonts need regenerating or something will render
// as '?'.
//
//   node firmware/tools/charset.mjs
//   node firmware/tools/charset.mjs --list      also list the characters

import { readdir, readFile } from 'node:fs/promises';

const DIR = 'data/device/quotes';

// Always present regardless of the data, so the UI's own strings and any future
// quote keep working: printable ASCII plus the Latin-1 supplement.
const BASE = [
  [0x20, 0x7e],
  [0xa0, 0xff],
];

const used = new Set();
for (const file of (await readdir(DIR)).filter((f) => f.endsWith('.tsv'))) {
  for (const ch of await readFile(`${DIR}/${file}`, 'utf8')) {
    const cp = ch.codePointAt(0);
    if (cp >= 0x20) used.add(cp);
  }
}

// Anything the UI itself draws that the dataset might not contain.
for (const ch of 'QUOTIDIAN Set up Wi-Fi Network Key Unknown 0123456789%·-') {
  used.add(ch.codePointAt(0));
}

const covered = (cp) => BASE.some(([a, b]) => cp >= a && cp <= b);
const extra = [...used].filter((cp) => !covered(cp)).sort((a, b) => a - b);

// Collapse to runs, allowing a small gap: one interval costs three numbers in
// the header, a wasted glyph costs its whole bitmap, so only bridge tiny holes.
//
// Kept at 2. At 4 the punctuation run swallowed U+2016, U+2017 and U+201B,
// which the data never uses and Cabin has no outline for - so they would have
// been baked in as .notdef boxes.
const MAX_GAP = 2;
const runs = [];
for (const cp of extra) {
  const last = runs[runs.length - 1];
  if (last && cp - last[1] <= MAX_GAP) last[1] = cp;
  else runs.push([cp, cp]);
}

const hex = (n) => '0x' + n.toString(16).toUpperCase().padStart(4, '0');
const glyphs = [...BASE, ...runs].reduce((n, [a, b]) => n + (b - a + 1), 0);

console.log(`distinct codepoints in use: ${used.size}`);
console.log(`outside ASCII+Latin-1: ${extra.length}, in ${runs.length} runs`);
console.log(`\nINTERVALS = [`);
for (const [a, b] of [...BASE, ...runs]) {
  const sample = [...Array(Math.min(b - a + 1, 12))].map((_, i) => String.fromCodePoint(a + i)).join('');
  console.log(`    (${hex(a)}, ${hex(b)}),`.padEnd(28) + `# ${sample}`);
}
console.log(`]`);
console.log(`\ntotal glyphs per font: ${glyphs} (was 191)`);

if (process.argv.includes('--list')) {
  console.log('\ncharacters beyond Latin-1:');
  console.log('  ' + extra.map((cp) => String.fromCodePoint(cp)).join(' '));
}
