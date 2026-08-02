#pragma once

#include <Arduino.h>

// Logs the reset reason and, when it was a deep-sleep wake, which pin caused it.
void powerLogWakeReason();

// Which source ended the last deep sleep. A Timer wake is a failure signal: it
// means the RTC alarm did not arrive and the backstop had to recover the board.
enum class WakeSource : uint8_t { Other = 0, Alarm, Timer, Button };
WakeSource powerWakeSource();
const char *powerWakeSourceName(WakeSource source);

// Ends the cycle in deep sleep. Does not return.
//
// The board does NOT power off - the ESP32 runs from the always-on VDD3V3 rail
// (LDO from VBAT/USB) and only the e-paper panel rail is switched. So the wake
// source is a GPIO: the PCF8563 alarm pulls its INT pin (GPIO9) low, and the
// front button (GPIO21) is wired the same way. Both are configured as ext1
// wake sources here.
//
// An earlier version removed this, on the mistaken belief that the board fully
// powered off. That belief came from POWERON reset reasons and lost RTC memory
// which were both artifacts of a monitoring script toggling the EN pin over
// USB - not the board's real behaviour. With no wake source configured, the
// board slept through every nightly alarm.
//
// `backstopSeconds` arms the ESP32's own timer as a second, independent wake
// source. The RTC alarm is the accurate one and should always win; the timer
// exists only so that a lost alarm costs one late update instead of silence for
// ever. Pass 0 to leave it disarmed.
[[noreturn]] void powerDown(long backstopSeconds);
