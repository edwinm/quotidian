#pragma once

#include <Arduino.h>

// Logs the reset reason and, when it was a deep-sleep wake, which pin caused it.
void powerLogWakeReason();

// Which source ended the last deep sleep. A Timer wake is a failure signal: it
// means the RTC alarm did not arrive and the backstop had to recover the board.
// Ext1NoMask means the chip woke on ext1 but reported no pin, which would make
// a working alarm look like no alarm at all.
enum class WakeSource : uint8_t { Other = 0, Alarm, Timer, Button, Ext1NoMask };
WakeSource powerWakeSource();
const char *powerWakeSourceName(WakeSource source);

// Reset reason, wake cause and ext1 mask as one short line for the panel.
// Serial cannot be opened on this board without resetting it, so this is the
// only way to see what actually started a cycle. See the notes on the
// definition for how to read it.
String powerWakeReport();

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
