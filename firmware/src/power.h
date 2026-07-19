#pragma once

#include <Arduino.h>

// Logs the reset reason. Kept because it is what revealed that this board does
// not sleep at all: the reason is POWERON, never DEEPSLEEP.
void powerLogResetReason();

// Ends the cycle. Despite calling esp_deep_sleep_start(), the board does not
// sleep - dropping the rail switches it off completely, and the PCF8563 alarm
// switches it back on. Does not return.
//
// There are consequently NO other ways back. No GPIO wake: a button cannot be
// noticed by a chip with no power. No timer: nothing is running to count.
// Whether the alarm was armed correctly is the single thing standing between a
// working display and a dark one, which is why rtcSetDailyAlarmUtc() reads its
// registers back and only then reports "verified".
[[noreturn]] void powerDown();
