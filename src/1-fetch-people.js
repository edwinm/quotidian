// Step 1: fetch everyone from Wikidata who is a writer/poet/philosopher/playwright/
// novelist, has BOTH a birth and death date, and has an English Wikiquote page.
// We keep the raw dates + precision so step 3 can bucket by day.

import { writeFile } from 'node:fs/promises';
import { fetchJson, parseWikidataDate } from './common.js';

const ENDPOINT = 'https://query.wikidata.org/sparql';

// Occupations we consider "quotable authors":
// Q4964182 philosopher, Q49757 poet, Q36180 writer, Q214917 playwright,
// Q6625963 novelist, Q11774202 essayist.
const QUERY = `
SELECT ?person ?personLabel ?birth ?birthPrec ?death ?deathPrec ?title ?sitelinks WHERE {
  VALUES ?occ { wd:Q4964182 wd:Q49757 wd:Q36180 wd:Q214917 wd:Q6625963 wd:Q11774202 }
  ?person wdt:P106 ?occ .
  ?person p:P569/psv:P569 ?birthNode .
  ?birthNode wikibase:timeValue ?birth ; wikibase:timePrecision ?birthPrec .
  ?person p:P570/psv:P570 ?deathNode .
  ?deathNode wikibase:timeValue ?death ; wikibase:timePrecision ?deathPrec .
  ?person wikibase:sitelinks ?sitelinks .
  ?article schema:about ?person ;
           schema:isPartOf <https://en.wikiquote.org/> ;
           schema:name ?title .
  SERVICE wikibase:label { bd:serviceParam wikibase:language "en" }
}
`;

async function main() {
  console.log('Querying Wikidata…');
  const url = `${ENDPOINT}?query=${encodeURIComponent(QUERY)}&format=json`;
  // The query is heavy and Wikidata occasionally returns a timeout/HTML page,
  // which breaks JSON parsing. Retry a few times with backoff.
  let data;
  for (let attempt = 1; ; attempt++) {
    try {
      data = await fetchJson(url);
      break;
    } catch (err) {
      if (attempt >= 4) throw err;
      console.log(`  attempt ${attempt} failed (${err.message}); retrying…`);
      await new Promise((r) => setTimeout(r, attempt * 5000));
    }
  }
  const rows = data.results.bindings;
  console.log(`  ${rows.length} raw rows`);

  // A person can have several birth/death statements at different precisions
  // (e.g. a year-only and a full-date one). Keep the MOST precise of each.
  const byQid = new Map();
  for (const r of rows) {
    const qid = r.person.value.split('/').pop();
    const birth = parseWikidataDate(r.birth.value, parseInt(r.birthPrec.value, 10));
    const death = parseWikidataDate(r.death.value, parseInt(r.deathPrec.value, 10));
    let rec = byQid.get(qid);
    if (!rec) {
      rec = {
        qid,
        name: r.personLabel.value,
        title: r.title.value, // Wikiquote page title
        birth,
        death,
        sitelinks: parseInt(r.sitelinks.value, 10), // fame proxy for ordering
      };
      byQid.set(qid, rec);
    }
    if (birth?.dayKnown && !rec.birth?.dayKnown) rec.birth = birth;
    if (death?.dayKnown && !rec.death?.dayKnown) rec.death = death;
  }

  const people = [...byQid.values()]
    // We need at least one day-precise date (birth or death) to place them on a day.
    .filter((p) => p.birth?.dayKnown || p.death?.dayKnown)
    .sort((a, b) => b.sitelinks - a.sitelinks);
  await writeFile('data/people.json', JSON.stringify(people, null, 2));
  console.log(`Wrote data/people.json — ${people.length} people with a day-precise birth or death date`);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
