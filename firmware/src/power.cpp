#include "power.h"

#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include "epd_driver.h"
#include "utilities.h"

// PCF8563 alarm INT (active low, 10k pull-up) and the front button, both on
// RTC-capable pins so they can wake deep sleep.
static constexpr gpio_num_t kRtcIntPin = GPIO_NUM_9;
static constexpr gpio_num_t kButtonPin = (gpio_num_t)BUTTON_1;

void powerLogWakeReason() {
    // reset reason: 1 POWERON, 3 SW, 5 INT_WDT, 8 DEEPSLEEP, 9 BROWNOUT.
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    uint64_t mask = esp_sleep_get_ext1_wakeup_status();
    Serial.printf("[power] reset reason=%d, wakeup cause=%d, ext1 mask=0x%llX "
                  "(RTC INT=%d button=%d)\n",
                  (int)esp_reset_reason(), (int)cause, mask,
                  (int)((mask >> kRtcIntPin) & 1), (int)((mask >> kButtonPin) & 1));
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

    // Drops the whole PWR_EN rail: e-paper supply and the blue LED7 with it.
    // The panel holds its image with no power at all.
    epd_poweroff_all();
    shutdownRadios();
    parkSdCardPins();
    Wire.end();

    // Wake on either pin going low: the RTC alarm (GPIO9) or the button (GPIO21).
    // ext1 reconfigures the pads, so the pull-ups are applied afterwards, and
    // RTC_PERIPH is kept powered so those pull-ups survive into sleep.
    esp_sleep_enable_ext1_wakeup((1ULL << kRtcIntPin) | (1ULL << kButtonPin),
                                 ESP_EXT1_WAKEUP_ANY_LOW);
    for (gpio_num_t pin : {kRtcIntPin, kButtonPin}) {
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(pin);
        rtc_gpio_pulldown_dis(pin);
    }
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    Serial.printf("[power] sleeping; wake pins RTC INT=%d button=%d\n",
                  digitalRead(kRtcIntPin), digitalRead(kButtonPin));

    esp_deep_sleep_start();
}
