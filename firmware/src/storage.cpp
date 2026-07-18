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

bool storageReadQuote(Quote &out) {
    if (!sMounted) return false;

    File file = SD.open(QUOTE_FILE, FILE_READ);
    if (!file) {
        Serial.printf("[sd] %s not found\n", QUOTE_FILE);
        return false;
    }

    out.text   = file.readStringUntil('\n');
    out.author = file.readStringUntil('\n');
    out.source = file.readStringUntil('\n');
    file.close();

    out.text.trim();
    out.author.trim();
    out.source.trim();

    if (out.text.isEmpty()) {
        Serial.printf("[sd] %s is empty\n", QUOTE_FILE);
        return false;
    }

    Serial.printf("[sd] loaded quote by %s\n", out.author.c_str());
    return true;
}
