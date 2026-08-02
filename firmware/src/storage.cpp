#include "storage.h"

#include <LittleFS.h>

#include "config.h"
#include "utilities.h"

// The dataset lives in internal flash, in the partition the stock 16 MB table
// labels "spiffs" - the label is historical, the contents are LittleFS.
// LittleFS::begin() defaults to that label, so no custom partition table is
// needed. See firmware/README.md.
static constexpr bool kFormatOnFail = false;

static bool sMounted = false;

bool storageBegin() {
    // runDailyUpdate() calls this on every render, and a button press can render
    // twice in one wake. Mounting an already-mounted filesystem is not free and
    // logs from inside the core.
    if (sMounted) return true;

    // Never format on failure. An empty filesystem and a missing one look the
    // same to the caller - both fall back to the built-in quote - but formatting
    // would throw the dataset away to fix a fault that is usually a bad flash.
    sMounted = LittleFS.begin(kFormatOnFail);
    if (!sMounted) {
        Serial.println("[fs] mount failed - run `pio run -t uploadfs`");
        return false;
    }

    Serial.printf("[fs] mounted: %s\n", storageDescription().c_str());
    return true;
}

bool storageMounted() {
    return sMounted;
}

String storageDescription() {
    if (!sMounted) return "not mounted";

    const size_t used = LittleFS.usedBytes();
    const size_t total = LittleFS.totalBytes();
    return String(used / 1024) + " kB used of " + String(total / 1024) + " kB";
}

// Counts newlines without holding the file in memory. Day files run to ~4 kB,
// so this is a single quick pass.
static int countLines(File &file) {
    file.seek(0);

    uint8_t buffer[512];
    int count = 0;
    int read;
    while ((read = file.read(buffer, sizeof(buffer))) > 0) {
        for (int i = 0; i < read; i++) {
            if (buffer[i] == '\n') count++;
        }
    }
    return count;
}

// Splits a tab-separated line. Field order must match pipeline/4-export-device.js.
static bool parseRow(const String &line, Quote &out) {
    String fields[6];
    int index = 0;
    int start = 0;

    while (index < 6) {
        int tab = line.indexOf('\t', start);
        if (tab < 0) {
            fields[index++] = line.substring(start);
            break;
        }
        fields[index++] = line.substring(start, tab);
        start = tab + 1;
    }

    if (fields[0].isEmpty()) return false;

    out.text        = fields[0];
    out.author      = fields[1];
    out.datesPrefix = fields[2];
    out.datesBold   = fields[3];
    out.datesSuffix = fields[4];
    out.attribution = fields[5];
    return true;
}

bool storageReadQuoteForDay(int month, int day, int year, Quote &out) {
    if (!sMounted) return false;

    char path[32];
    snprintf(path, sizeof(path), "%s/%02d-%02d.tsv", QUOTE_DIR, month, day);

    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        Serial.printf("[fs] %s not found\n", path);
        return false;
    }

    int lines = countLines(file);
    if (lines <= 0) {
        Serial.printf("[fs] %s is empty\n", path);
        file.close();
        return false;
    }

    // Stable within a day, rotating from year to year.
    int wanted = year % lines;

    file.seek(0);
    String line;
    for (int i = 0; i <= wanted; i++) {
        line = file.readStringUntil('\n');
    }
    file.close();

    line.replace("\r", "");
    if (!parseRow(line, out)) {
        Serial.printf("[fs] %s line %d did not parse\n", path, wanted);
        return false;
    }

    Serial.printf("[fs] %s: quote %d of %d, %s\n", path, wanted + 1, lines,
                  out.author.c_str());
    return true;
}

bool storageReadOverrideQuote(Quote &out) {
    if (!sMounted) return false;

    File file = LittleFS.open(QUOTE_FILE, FILE_READ);
    if (!file) return false;

    out.text   = file.readStringUntil('\n');
    out.author = file.readStringUntil('\n');
    out.datesSuffix = file.readStringUntil('\n');
    file.close();

    out.text.trim();
    out.author.trim();
    out.datesSuffix.trim();
    out.datesPrefix = "";
    out.datesBold = "";
    out.attribution = "";

    if (out.text.isEmpty()) return false;

    Serial.printf("[fs] using override %s (%s)\n", QUOTE_FILE, out.author.c_str());
    return true;
}
