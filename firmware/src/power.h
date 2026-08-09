#pragma once

#include <Arduino.h>

// Logs the reset reason and, when it was a deep-sleep wake, which pin caused it.
void powerLogWakeReason();

// What ended the last cycle, as the ESP32 sees it.
//
// On this board the answer is always Other: it is switched off between updates,
// so there is no wake to report. The other cases are kept because they are what
// proves that - if Alarm, Timer or Ext1NoMask ever became non-zero, the board
// would be doing something different from what is documented in powerDown().
enum class WakeSource : uint8_t { Other = 0, Alarm, Timer, Button, Ext1NoMask };
WakeSource powerWakeSource();
const char *powerWakeSourceName(WakeSource source);

// Reset reason, wake cause and ext1 mask as one short line for the panel.
// Serial cannot be opened on this board without resetting it, so this is the
// only way to see what actually started a cycle. See the notes on the
// definition for how to read it.
String powerWakeReport();

// Ends the cycle. Does not return.
//
// This calls esp_deep_sleep_start(), but the board does not sleep: the rail
// drops and the PCF8563 alarm switches it back on. Measured on the boot the
// alarm caused, untouched: reset reason POWERON, wake cause NONE, ext1 mask 0.
//
// So the ext1 configuration below is not what brings the board back - the alarm
// is, by restoring power. It is left in place because it costs nothing and
// documents the intent, but nothing should be built on it firing.
//
// This was believed to be a real deep sleep for a while, on the strength of a
// port-presence test that could not tell the two apart. Anything relying on
// deep-sleep behaviour - RTC memory, timer wakeups, a GPIO wake from the button
// - does not work here. See firmware/README.md.
[[noreturn]] void powerDown();
