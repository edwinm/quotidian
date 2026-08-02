#pragma once

#include <Arduino.h>

struct Quote {
    String text;
    String author;

    // The dates line is pre-split into three runs so the device does not have
    // to parse date strings. The middle run is the day and month that match
    // today, and is drawn bold; the rest stays regular.
    //   "1122 - " + "18 July" + " 1192"
    String datesPrefix;
    String datesBold;
    String datesSuffix;

    // Empty when the source does not require attribution.
    String attribution;
};

// Mounts the dataset partition in internal flash. Safe to call when it has
// never been written; the caller falls back to built-in content.
//
// This used to be a microSD card. The card was dropped because it sits on the
// unswitched 3V3 rail and cannot be powered down, which made it the prime
// suspect for the sleep current that flattened a 2000 mAh cell in a week - and
// because the dataset, once cut to the best 20 quotes a day, is 1.2 MB and fits
// the stock partition table with room to spare.
bool storageBegin();

bool storageMounted();

// "612 kB used of 3456 kB", or "not mounted".
String storageDescription();

// Reads the quote for a calendar day from /quotes/MM-DD.tsv.
//
// Every day holds 20 quotes, so one is chosen by `year % count`: stable within
// a day, different from one year to the next, and a twenty-year cycle before it
// repeats. The count is read from the file rather than assumed, so a day that
// ends up with fewer still rotates correctly.
//
// Returns false when the filesystem is absent, there is no file for that day,
// or the file is empty - the caller should then fall back to built-in content.
bool storageReadQuoteForDay(int month, int day, int year, Quote &out);

// Manual override at /quote.txt: quote, author, dates on three lines. Only
// consulted when the day file is unavailable, so a filesystem without the
// dataset still shows something chosen.
bool storageReadOverrideQuote(Quote &out);
