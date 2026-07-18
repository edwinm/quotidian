#include "storage.h"

#include <SD.h>
#include <SPI.h>

#include "config.h"
#include "utilities.h"

static bool sMounted = false;

bool storageBegin() {
    // The card has its own SPI bus, separate from the e-paper's parallel bus.
    SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);

    sMounted = SD.begin(SD_CS, SPI);
    if (!sMounted) {
        Serial.println("[sd] mount failed (no card?)");
        SPI.end();
        return false;
    }

    Serial.printf("[sd] mounted: %s\n", storageDescription().c_str());
    return true;
}

bool storageMounted() {
    return sMounted;
}

String storageDescription() {
    if (!sMounted) return "no card";

    const char *type;
    switch (SD.cardType()) {
        case CARD_MMC:  type = "MMC";  break;
        case CARD_SD:   type = "SDSC"; break;
        case CARD_SDHC: type = "SDHC"; break;
        default:        type = "?";    break;
    }

    float gb = SD.cardSize() / (1024.0f * 1024.0f * 1024.0f);
    return String(gb, 1) + " GB " + type;
}

// Counts newlines without holding the file in memory. Day files run to ~40 kB,
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

    File file = SD.open(path, FILE_READ);
    if (!file) {
        Serial.printf("[sd] %s not found\n", path);
        return false;
    }

    int lines = countLines(file);
    if (lines <= 0) {
        Serial.printf("[sd] %s is empty\n", path);
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
        Serial.printf("[sd] %s line %d did not parse\n", path, wanted);
        return false;
    }

    Serial.printf("[sd] %s: quote %d of %d, %s\n", path, wanted + 1, lines,
                  out.author.c_str());
    return true;
}

bool storageReadOverrideQuote(Quote &out) {
    if (!sMounted) return false;

    File file = SD.open(QUOTE_FILE, FILE_READ);
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

    Serial.printf("[sd] using override %s (%s)\n", QUOTE_FILE, out.author.c_str());
    return true;
}
