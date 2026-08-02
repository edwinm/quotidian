#pragma once

#include <Arduino.h>
#include <time.h>

// PCF8563 hardware clock (U8 on the board, I2C 0x51).
//
// This is what makes deep sleep viable. The ESP32's own RTC runs off an
// internal RC oscillator - measured at -3.9% against the crystal, and varying
// by ~0.2% between boots at constant temperature, so minutes per day. The
// PCF8563 has its own 32.768 kHz crystal (Y1, FC-135) and drifts seconds per
// day, and it has a backup cell (MS412FE) so it keeps time even with the
// battery removed.
//
// Time is stored in the chip as UTC. Local time is derived through the
// timezone, so DST needs no help from the clock itself.

bool rtcBegin();
bool rtcAvailable();

// True when the chip holds a plausible time (its voltage-low flag is clear and
// the year is sane).
bool rtcTimeValid();

// Copies the chip's time into the ESP32 system clock, and applies the stored
// timezone. After this, localtime() and todayParts() work with no network.
bool rtcApplyToSystemClock();

// Writes the current system time back to the chip. Call after an NTP sync.
void rtcStoreSystemClock();

// Arms a daily alarm at the given UTC hour and minute. The INT pin (GPIO9)
// goes low when it fires, which is what wakes the ESP32 from deep sleep.
void rtcSetDailyAlarmUtc(int utcHour, int utcMinute);

// Arms the alarm a number of minutes ahead of whatever the chip currently
// reads. Used when the clock is not trustworthy: the absolute time may be
// wrong, but the chip still counts, so a relative alarm still brings the board
// back at the right interval - which the ESP32's own backstop timer, accurate to
// minutes a day, cannot do.
void rtcSetAlarmInMinutes(int minutes);

// True if the chip's alarm flag is set, meaning the alarm is what started this
// cycle. This is the authoritative answer, and not the same question as
// powerWakeSource(): that reports what pulled the ESP32 out of deep sleep, so a
// backstop-timer wake or a button press still leaves this flag set if the alarm
// had fired. A reset also loses the ESP32's wake cause entirely, while the flag
// survives in the chip.
//
// Read it before rtcClearAlarm(), which is what resets the flag.
bool rtcAlarmFired();

// The chip's own view of things, as one line: its UTC time, the armed alarm,
// and the interrupt/flag bits. Rendered on screen because opening the serial
// port resets this board, which makes serial useless for observing it.
String rtcDiagnostics();

// Logs the chip's alarm registers and control byte. An alarm that never fires
// is otherwise invisible - this is what caught the first version arming nothing
// at all when the clock was not yet set.
void rtcLogAlarmState();

// Clears a pending alarm flag, releasing INT back to high. Must be called
// after waking, or the pin stays low and the next sleep returns immediately.
void rtcClearAlarm();

