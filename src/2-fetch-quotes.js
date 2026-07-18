// Step 2: for each person, fetch their English Wikiquote page wikitext and extract
// only quotes we can be confident are genuine:
//   - the quote must sit in a "safe" section (NOT Misattributed / Disputed /
//     Attributed / Unsourced / "about" / meta sections), and
//   - the quote must be followed by a source citation line in the wikitext.
// Both conditions must hold. This is deliberately strict: we drop uncertain quotes.

import { readFile, writeFile } from 'node:fs/promises';
import { fetchJson, pool, LICENSE, LICENSE_URL } from './common.js';

const API = 'https://en.wikiquote.org/w/api.php';
const MAX_QUOTES_PER_PERSON = 8;
const MIN_LEN = 15;
const MAX_LEN = 220;

// A section heading (any level) containing one of these words is NOT trusted.
const BLOCKED_HEADING = /(misattribut|disputed|attributed|unsourced|about|external|see also|references|further reading|bibliograph|works|notes|quotes about|posthumous attribut)/i;

// Common English function words — used to detect (and keep) English quotes.
const EN_STOPWORDS = new Set(
  ('the and of to a is in that it was for as with his be not this but are or my you i he she we they ' +
    'have has had will would can could should do does did at by from an their our your who what when ' +
    'which there here all no so if then than them me him her us')
    .split(' '),
);

function looksEnglish(text) {
  const words = text.toLowerCase().match(/[a-z']+/g) || [];
  if (words.length < 4) return true; // too short to judge; length filter handles junk
  const hits = words.filter((w) => EN_STOPWORDS.has(w)).length;
  return words.length > 8 ? hits >= 2 : hits >= 1;
}

function cleanWikitext(s) {
  return s
    .replace(/<ref[^>]*\/>/gi, '')
    .replace(/<ref[^>]*>[\s\S]*?<\/ref>/gi, '')
    .replace(/<br\s*\/?>/gi, ' ') // line breaks (poetry) -> space
    .replace(/<!--[\s\S]*?-->/g, '')
    .replace(/\{\{\s*(pbr|p|break|-|nbsp|clear|spaces?)\b[^{}]*\}\}/gi, ' ') // line-break templates -> space
    .replace(/\{\{[^{}]*\}\}/g, '') // remaining simple templates
    .replace(/\[\[[^\]|]*\|([^\]]*)\]\]/g, '$1') // [[a|b]] -> b
    .replace(/\[\[([^\]]*)\]\]/g, '$1') // [[a]] -> a
    .replace(/\[https?:\/\/\S+\s+([^\]]*)\]/g, '$1') // [url text] -> text
    .replace(/\[https?:\/\/\S+\]/g, '')
    .replace(/'''''|'''|''/g, '') // bold/italic markup
    .replace(/<[^>]+>/g, ' ') // stray HTML (block tags too) -> space
    .replace(/&nbsp;/g, ' ')
    .replace(/&mdash;/g, '—')
    .replace(/([a-z][.!?;,])([A-Z"'])/g, '$1 $2') // restore space at run-together sentence breaks
    .replace(/\s+/g, ' ')
    .trim();
}

// Turn a citation into readable text. Handles {{cite book|title=..|last=..|year=..}}
// and {{citation|...}} templates; otherwise falls back to normal cleaning.
function parseSource(raw) {
  if (/\{\{\s*(cite|citation)/i.test(raw)) {
    const get = (name) => {
      const m = new RegExp(`\\|\\s*${name}\\s*=\\s*([^|}]+)`, 'i').exec(raw);
      return m ? cleanWikitext(m[1]) : '';
    };
    const title = get('title') || get('chapter') || get('work');
    const author = get('author') || [get('first'), get('last')].filter(Boolean).join(' ');
    const year = (get('year') || get('date')).match(/\d{4}/)?.[0] || '';
    const parts = [author, title].filter(Boolean).join(', ');
    const out = [parts, year ? `(${year})` : ''].filter(Boolean).join(' ').trim();
    return out;
  }
  return cleanWikitext(raw);
}

// Parse the wikitext of one page into trustworthy { text, source } quotes.
function extractQuotes(wikitext) {
  const lines = wikitext.split('\n');
  const out = [];
  // Track blocking at the top (level-2) section AND the current subsection.
  // A blocked top section (e.g. "Quotes about X") must stay blocked even if it
  // contains innocuous-looking subheadings (e.g. "=== Isaac Newton ===").
  let topBlocked = false;
  let safe = true;
  let currentWork = ''; // nearest specific-work heading, used to enrich thin sources
  const GENERIC = /^(quotes?|sourced|quotations?|poems?|poetry|verse|misc\.?|main|works?|prose|letters?)$/i;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    const heading = /^(={2,})\s*(.*?)\s*\1\s*$/.exec(line);
    if (heading) {
      const level = heading[1].length;
      const name = cleanWikitext(heading[2]);
      const blocked = BLOCKED_HEADING.test(heading[2]);
      if (level === 2) { topBlocked = blocked; currentWork = ''; }
      safe = !(topBlocked || blocked);
      currentWork = safe && name && !GENERIC.test(name) ? name : level === 2 ? '' : currentWork;
      continue;
    }
    if (!safe) continue;

    // A top-level quote bullet: "* ..." but not "**" / "*:" (those are sources/notes).
    if (/^\*[^*:]/.test(line)) {
      const text = cleanWikitext(line.replace(/^\*\s*/, ''));
      // Require a source citation on the following sub-bullet line ("** ..." or "*: ...").
      const next = lines[i + 1] || '';
      const hasSource = /^\*[*:]/.test(next);
      if (!hasSource) continue;
      // Gather the source, joining continuation lines if it opens a template
      // that spans several lines (common with {{cite ...}}).
      let rawSource = next.replace(/^\*[*:]\s*/, '');
      let j = i + 1;
      const balanced = (s) => (s.match(/\{\{/g) || []).length <= (s.match(/\}\}/g) || []).length;
      while (!balanced(rawSource) && j - i < 8 && lines[j + 1] != null) {
        j++;
        rawSource += ' ' + lines[j].replace(/^[*:]+\s*/, '');
      }
      let source = parseSource(rawSource);
      // Guarantee a clean, readable citation — drop anything still messy.
      if (!source || source.length < 3 || /[{}]/.test(source)) continue;
      // Reject uncertain sourcing: "Attributed to…", "misattributed", etc.
      if (/attribut/i.test(source)) continue;
      if (text.length < MIN_LEN || text.length > MAX_LEN) continue;
      if (/^[A-Z\s]+$/.test(text)) continue; // all-caps junk
      // Reject editorial insertions / paraphrase markers like "[Still an atheist…]".
      if (/[[\]]/.test(text)) continue;
      // Require it to read like a sentence: has a space and some lowercase.
      if (!/\s/.test(text) || !/[a-z]/.test(text)) continue;
      // English-only.
      if (!looksEnglish(text)) continue;
      // Enrich thin citations ("p. 12") with the nearest work title.
      if (currentWork && (source.length < 10 || /^(pp?\.?|pages?|ch\.?|chapter|no\.?|vol|st\.|line)/i.test(source))) {
        source = `${currentWork}, ${source}`;
      }
      out.push({ text, source });
      if (out.length >= MAX_QUOTES_PER_PERSON) break;
    }
  }
  return out;
}

// Fetch wikitext for up to 20 titles in one API call, with retry/backoff so a
// transient error or rate-limit doesn't silently drop 20 people from the dataset.
async function fetchBatch(titles) {
  const url =
    `${API}?action=query&format=json&prop=revisions&rvprop=content&rvslots=main` +
    `&formatversion=2&redirects=1&titles=${titles.map(encodeURIComponent).join('|')}`;
  let data;
  for (let attempt = 1; ; attempt++) {
    try {
      data = await fetchJson(url);
      break;
    } catch (err) {
      if (attempt >= 6) throw err;
      // Exponential backoff, generous enough to outlast API rate-limiting.
      await new Promise((r) => setTimeout(r, attempt * attempt * 3000));
    }
  }
  const map = new Map();
  // Handle title normalization / redirects so we can map back to input titles.
  const alias = new Map();
  for (const n of data.query?.normalized || []) alias.set(n.from, n.to);
  for (const r of data.query?.redirects || []) alias.set(r.from, r.to);
  for (const p of data.query?.pages || []) {
    const content = p.revisions?.[0]?.slots?.main?.content;
    if (content) map.set(p.title, content);
  }
  return { map, alias };
}

async function main() {
  const people = JSON.parse(await readFile('data/people.json', 'utf8'));
  console.log(`Loaded ${people.length} people. Fetching Wikiquote pages…`);

  // Group into batches of 20 titles.
  const BATCH = 20;
  const batches = [];
  for (let i = 0; i < people.length; i += BATCH) batches.push(people.slice(i, i + BATCH));

  const result = [];
  let withQuotes = 0;
  let failedBatches = 0;

  await pool(
    batches,
    async (group) => {
      const titles = group.map((p) => p.title);
      let map, alias;
      try {
        ({ map, alias } = await fetchBatch(titles));
      } catch (err) {
        failedBatches++;
        return;
      }
      for (const person of group) {
        let title = person.title;
        while (alias.has(title)) title = alias.get(title);
        const content = map.get(title);
        if (!content) continue;
        const quotes = extractQuotes(content).map((q) => ({
          ...q,
          sourceUrl: `https://en.wikiquote.org/wiki/${encodeURIComponent(person.title.replace(/ /g, '_'))}`,
          license: LICENSE,
          licenseUrl: LICENSE_URL,
          attributionRequired: true,
        }));
        if (quotes.length) {
          withQuotes++;
          result.push({ ...person, quotes });
        }
      }
    },
    {
      concurrency: 3,
      onProgress: (d, t) => process.stdout.write(`\r  batch ${d}/${t}`),
    },
  );

  process.stdout.write('\n');
  const totalQuotes = result.reduce((n, p) => n + p.quotes.length, 0);
  await writeFile('data/quotes-raw.json', JSON.stringify(result, null, 2));
  console.log(`Wrote data/quotes-raw.json — ${withQuotes} people with quotes, ${totalQuotes} quotes total`);
  if (failedBatches) console.warn(`WARNING: ${failedBatches} batch(es) failed after retries — data is incomplete, re-run`);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
