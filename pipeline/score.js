// Ranks the quotes competing for one calendar day, so only the best survive to
// the device.
//
// Every day has more quotes than the device keeps - 113 on average, 212 at the
// worst - and they are not of equal worth. Wikiquote will happily supply a
// couplet from a forgotten Victorian versifier alongside Shakespeare. The
// display shows exactly one quote a day, rotating per year, so a day's list is
// really a twenty-year rotation: everything in it will be somebody's whole
// experience of that date one year, and there is no scrolling past a dud.
//
// Two signals decide it, in the order the brief asked for:
//
//   1. Is the author someone you have heard of? Wikidata sitelinks - the number
//      of language editions with an article - is a good proxy and is already
//      fetched in step 1. It runs from 6 at the tenth percentile to 335 for
//      Shakespeare, so it is used on a log scale: the gap between 6 and 30
//      sitelinks means far more than the gap between 300 and 330.
//
//   2. Does the quote still read today? This is what "outdated" turns into in
//      practice - not the author's dates, since Aristotle in modern translation
//      reads better than most Edwardians, but the diction of the quote itself.
//      Archaic English is the strongest available marker of a line that has
//      aged out.
//
// Everything here is a heuristic over text, so it is deliberately tuned to be
// blunt rather than clever: it decides which 20 of 113 to keep, and being
// slightly wrong costs one merely-good quote instead of a bad one.

// Fame at the top of the range. Shakespeare is 335; anything at or above this
// is treated as equally famous, since the difference stops meaning anything.
const SITELINKS_CEILING = 350;

// Archaic diction. This is the "outdated" filter, and the one that does the
// most work: it is what separates "War hath no fury like a non-combatant" and
// "Perchance - He knows - canst thou not trust His love?" from a line that
// could have been written this morning.
//
// The penalty is meant to be survivable rather than fatal. Shakespeare writing
// `thou` is not a stale quote, it is Shakespeare, and he should still outrank a
// forgotten poet writing plainly - so a famous author can carry an archaic line
// into the list, while a clean line by the same author beats it.
const ARCHAIC =
  /\b(thou|thee|thy|thine|hath|doth|dost|hast|shalt|wilt|canst|couldst|wouldst|'tis|'twas|'twixt|o'er|e'er|ne'er|'gainst|whilst|unto|whence|whither|hither|thence|betwixt|methinks|perchance|ofttimes|oft|verily|forsooth|prithee|saith|cometh|goeth|nay)\b/i;

// Elided past participles - ador'd, lov'd, crost. Modern contractions are all
// short pronouns ("he'd", "I'd"), so requiring three letters before the
// apostrophe avoids them.
const ELIDED = /\b[a-z]{3,}'d\b/i;

// Openers that only make sense with the sentence before them still attached.
// Wikiquote bullets are often the second half of a thought.
const DANGLING_OPENER = /^(and|but|or|yet|so|for|nor|then|thus|therefore|however|besides)\b/i;

// A relative pronoun in the opening position is not a sentence at all, it is
// the tail of one: "Which I have earned with the sweat of my brows."
const RELATIVE_OPENER = /^(which|who|whom|whose|that)\b/i;

// A bare third-person pronoun opening a quote is pointing at somebody the
// reader cannot see - "She wasn't doing a thing that I could see" is a novel
// excerpt, not a quotation.
//
// "He who fights monsters" is the exception and a common aphoristic form, so
// the relative clause is let through.
const ORPHAN_PRONOUN = /^(he|she|they|him|her|them|his|their)\b(?!\s+who\b)/i;

// An excerpt, not a quote: the elision marks the missing part that made it make
// sense. Step 2 already rejects square brackets; this catches the rest.
const ELLIPSIS = /\.\.\.|…/;

// Poetry pulled out of its line breaks. The give-away is a line addressing
// something at high pitch - "The despot's heel is on thy shore, Maryland! His
// torch is at thy temple-door, Maryland!" - which reads as doggerel on a wall.
const EXCLAMATORY = /!.*!/;

// The length that reads best on the panel. Below this a quote is usually a
// fragment; above it the text drops to the narrow margin and fills the screen,
// which is the layout working hard rather than the quote being good.
const IDEAL_MIN = 45;
const IDEAL_MAX = 160;
const TOO_LONG = 185;
const TOO_SHORT = 35;

// 0..1, where 1 is a household name. Log scale: see the note above.
export function fame(sitelinks) {
  const n = Math.max(0, Math.min(SITELINKS_CEILING, sitelinks ?? 0));
  return Math.log10(1 + n) / Math.log10(1 + SITELINKS_CEILING);
}

// Points added to or taken off the fame score, which is worth 100. Sized so
// that no single flaw outweighs being genuinely well known, but two or three
// together do.
export function readability(text) {
  let points = 0;

  if (ARCHAIC.test(text) || ELIDED.test(text)) points -= 30;
  if (RELATIVE_OPENER.test(text)) points -= 40;
  if (ORPHAN_PRONOUN.test(text)) points -= 30;
  if (DANGLING_OPENER.test(text)) points -= 25;
  if (ELLIPSIS.test(text)) points -= 20;
  if (EXCLAMATORY.test(text)) points -= 15;

  const len = text.length;
  if (len < TOO_SHORT) points -= 20;
  else if (len > TOO_LONG) points -= 15;
  else if (len >= IDEAL_MIN && len <= IDEAL_MAX) points += 15;

  // A complete thought, opened and closed.
  if (/^[A-Z"']/.test(text)) points += 5;
  if (/[.!?"']$/.test(text)) points += 5;

  return points;
}

export function scoreQuote(quote) {
  return 100 * fame(quote.sitelinks) + readability(quote.text);
}

// Picks the day's list: highest scoring first, but no author may take more than
// `maxPerPerson` of the slots.
//
// Without the cap a single famous name fills the day - Wikiquote gives up to
// eight quotes per person and there are only about four people per day, so
// Shakespeare's birthday would be Shakespeare eight years running. The rotation
// is per year, so variety within a day is variety across two decades of the
// display, which is the whole point of keeping more than one.
export function pickBest(quotes, { limit, maxPerPerson }) {
  const ranked = [...quotes]
    .map((q) => ({ q, score: scoreQuote(q) }))
    // Deterministic: two runs of the pipeline must produce the same file, or
    // the year-based rotation silently shows a different quote after a rebuild.
    .sort(
      (a, b) =>
        b.score - a.score ||
        a.q.author.localeCompare(b.q.author) ||
        a.q.text.localeCompare(b.q.text),
    );

  const used = new Map();
  const kept = [];
  for (const { q } of ranked) {
    if (kept.length >= limit) break;
    const n = used.get(q.author) ?? 0;
    if (n >= maxPerPerson) continue;
    used.set(q.author, n + 1);
    kept.push(q);
  }

  // A day thin enough that the cap left it short is better served by repeats
  // than by blanks, so relax and top up. Does not happen with the present
  // dataset - every day has at least 20 - but a stricter fetch could cause it.
  if (kept.length < limit) {
    for (const { q } of ranked) {
      if (kept.length >= limit) break;
      if (!kept.includes(q)) kept.push(q);
    }
  }

  return kept;
}
