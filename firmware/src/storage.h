#pragma once

#include <Arduino.h>

struct Quote {
    String text;
    String author;
    String source;
};

// Mounts the microSD card over SPI. Safe to call when no card is inserted.
bool storageBegin();

bool storageMounted();

// "32.0 GB SDHC", or "no card".
String storageDescription();

// Reads QUOTE_FILE from the card. Returns false if there is no card, no file,
// or the file is empty; the caller should fall back to built-in content.
bool storageReadQuote(Quote &out);
