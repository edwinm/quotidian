#pragma once

#include <Arduino.h>

enum WakeCause {
    WAKE_POWER_ON,   // cold boot or reset
    WAKE_RTC_ALARM,  // PCF8563 INT went low - the nightly update
    WAKE_BUTTON,     // front button
    WAKE_TIMER,      // the backstop fired, meaning the alarm did not
};

WakeCause powerWakeCause();
const char *powerWakeCauseName(WakeCause cause);

// Shuts down everything that draws current, then enters deep sleep. Does not
// return.
//
// Wake sources are the RTC alarm (GPIO9) and the button (GPIO21), both active
// low, both on RTC-capable pins.
[[noreturn]] void powerDeepSleep();
