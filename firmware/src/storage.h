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

// Mounts the microSD card over SPI. Safe to call when no card is inserted.
bool storageBegin();

bool storageMounted();

// "32.0 GB SDHC", or "no card".
String storageDescription();

// Reads the quote for a calendar day from /quotes/MM-DD.tsv.
//
// Days hold between 22 and 212 quotes, so one is chosen by `year % count`:
// stable within a day, different from one year to the next.
//
// Returns false when there is no card, no file for that day, or the file is
// empty - the caller should then fall back to built-in content.
bool storageReadQuoteForDay(int month, int day, int year, Quote &out);

// Manual override at /quote.txt: quote, author, dates on three lines. Only
// consulted when the day file is unavailable, so a card without the dataset
// still shows something chosen.
bool storageReadOverrideQuote(Quote &out);
