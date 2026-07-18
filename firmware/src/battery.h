#pragma once

#include <Arduino.h>

struct BatteryStatus {
    bool  present;   // false when the reading looks like USB-only power
    float volts;
    int   percent;   // 0..100, only meaningful when `present`
};

// Reads the ADC calibration from eFuse. Call once from setup().
void batteryBegin();

// Takes a fresh measurement.
//
// IMPORTANT: on the ESP32-S3 the battery sense pin sits on ADC2, and ADC2 is
// unavailable while the Wi-Fi radio is running. Sample this before bringing
// Wi-Fi up (or after WiFi.disconnect()), otherwise the reading comes back 0.
BatteryStatus batteryRead();
