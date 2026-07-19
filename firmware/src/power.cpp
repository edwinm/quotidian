#include "power.h"

#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "epd_driver.h"
#include "utilities.h"

void powerLogResetReason() {
    // 1 = POWERON, 3 = SW, 4 = PANIC, 5 = INT_WDT, 8 = DEEPSLEEP, 9 = BROWNOUT.
    // On this board an alarm-driven start reports POWERON, identical to a
    // hand-pressed reset, which is why rtcAlarmFired() is what distinguishes
    // them.
    Serial.printf("[power] reset reason=%d\n", (int)esp_reset_reason());
}

// The SD card sits on VDD3V3, which is NOT switched by PWR_EN, so it stays
// powered through deep sleep and cannot be turned off in software. What can be
// controlled is how its lines are left.
//
// This matters more than it looks: the bus has 10k pull-ups to VDD3V3 (R5, R13,
// R16). Leaving a line driven low would sink 3.3 V / 10 k = 330 uA on that pin
// alone - most of the board's entire sleep budget, on one wire. Releasing the
// pins to high-impedance lets the pull-ups hold them high at no cost, and
// leaves the card deselected.
static void parkSdCardPins() {
    SPI.end();

    const gpio_num_t pins[] = {(gpio_num_t)SD_MISO, (gpio_num_t)SD_MOSI,
                               (gpio_num_t)SD_SCLK, (gpio_num_t)SD_CS};
    for (gpio_num_t pin : pins) {
        pinMode(pin, INPUT);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_pullup_dis(pin);
        gpio_pulldown_dis(pin);
    }
}

static void shutdownRadios() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();  // deinit() here just logs an error; stop is what matters

    // Harmless when BLE was never started.
    esp_bt_controller_disable();
}

[[noreturn]] void powerDown() {
    Serial.println("[power] powering down");
    Serial.flush();

    // Drops the whole PWR_EN rail: e-paper supply and the blue LED7 with it.
    // The panel holds its image with no power at all.
    epd_poweroff_all();

    shutdownRadios();
    parkSdCardPins();
    Wire.end();

    // esp_deep_sleep_start() is what actually drops the rail on this board. No
    // wake sources are configured because none can work: with the board off
    // there is no RTC domain to hold a pin state and no timer to run. The
    // PCF8563 alarm is wired to switch the board back on in hardware.
    esp_deep_sleep_start();
}
