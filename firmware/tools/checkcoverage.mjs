// Fails if the exported data uses a codepoint the generated fonts lack.
//
// The device substitutes '?' for a missing glyph, silently and only on the
// panel, so this is the only cheap place to catch it. Run after
// `npm run export:device`:
//
//   node firmware/tools/checkcoverage.mjs
//
// Reads the intervals back out of the generated header rather than out of
// fontconvert.py, so it verifies what was actually built.

import { readdir, readFile } from 'node:fs/promises';

const HEADER = 'firmware/src/fonts/font_large.h';
const DIR = 'data/device/quotes';

const header = await readFile(HEADER, 'utf8');
const block = /const UnicodeInterval \w+\[\] = \{([\s\S]*?)\};/.exec(header);
if (!block) {
  console.error(`could not find the interval table in ${HEADER}`);
  process.exit(1);
}

const intervals = [...block[1].matchAll(/\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)/g)].map(
  (m) => [parseInt(m[1], 16), parseInt(m[2], 16)],
);

const covered = (cp) => intervals.some(([a, b]) => cp >= a && cp <= b);

const missing = new Map();
let scanned = 0;
for (const file of (await readdir(DIR)).filter((f) => f.endsWith('.tsv'))) {
  const body = await readFile(`${DIR}/${file}`, 'utf8');
  scanned++;
  for (const ch of body) {
    const cp = ch.codePointAt(0);
    if (cp === 0x09 || cp === 0x0a) continue; // field and row separators
    if (!covered(cp)) {
      const seen = missing.get(cp) ?? { count: 0, file };
      seen.count++;
      missing.set(cp, seen);
    }
  }
}

console.log(`${HEADER}: ${intervals.length} intervals`);
console.log(`scanned ${scanned} day files`);

if (!missing.size) {
  console.log('OK - every character in the data has a glyph.');
  process.exit(0);
}

console.error(`\nFAIL - ${missing.size} codepoints have no glyph and will render as '?':`);
for (const [cp, { count, file }] of [...missing].sort((a, b) => b[1].count - a[1].count)) {
  const hex = 'U+' + cp.toString(16).toUpperCase().padStart(4, '0');
  console.error(`  ${hex}  ${JSON.stringify(String.fromCodePoint(cp))}  x${count}  (e.g. ${file})`);
}
console.error('\nRe-run: node firmware/tools/charset.mjs, paste INTERVALS into');
console.error('firmware/tools/fontconvert.py, and regenerate the headers.');
process.exit(1);
