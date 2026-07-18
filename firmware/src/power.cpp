#include "power.h"

#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_bt.h>
#include <esp_sleep.h>
#include <esp_wifi.h>

#include "epd_driver.h"
#include "utilities.h"

// PCF8563 INT (active low, 10k pull-up to VDD3V3) and the front button.
static constexpr gpio_num_t kRtcIntPin = GPIO_NUM_9;
static constexpr gpio_num_t kButtonPin = (gpio_num_t)BUTTON_1;

WakeCause powerWakeCause() {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT1: {
            uint64_t mask = esp_sleep_get_ext1_wakeup_status();
            if (mask & (1ULL << kRtcIntPin)) return WAKE_RTC_ALARM;
            if (mask & (1ULL << kButtonPin)) return WAKE_BUTTON;
            return WAKE_POWER_ON;
        }
        default:
            return WAKE_POWER_ON;
    }
}

const char *powerWakeCauseName(WakeCause cause) {
    switch (cause) {
        case WAKE_RTC_ALARM: return "RTC alarm";
        case WAKE_BUTTON:    return "button";
        default:             return "power-on";
    }
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

[[noreturn]] void powerDeepSleep() {
    Serial.println("[power] entering deep sleep");
    Serial.flush();

    // Drops the whole PWR_EN rail: e-paper supply and the blue LED7 with it.
    // The panel holds its image with no power at all.
    epd_poweroff_all();

    shutdownRadios();
    parkSdCardPins();
    Wire.end();

    // Order matters: enabling ext1 reconfigures the pads, so pull-ups have to be
    // applied afterwards or they are discarded. Both pins are active low and
    // would otherwise float and wake the board at random.
    esp_sleep_enable_ext1_wakeup((1ULL << kRtcIntPin) | (1ULL << kButtonPin),
                                 ESP_EXT1_WAKEUP_ANY_LOW);

    for (gpio_num_t pin : {kRtcIntPin, kButtonPin}) {
        rtc_gpio_init(pin);
        rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_en(pin);
        rtc_gpio_pulldown_dis(pin);
    }

    Serial.printf("[power] wake pins: RTC INT=%d button=%d (both must read 1)\n",
                  digitalRead(kRtcIntPin), digitalRead(kButtonPin));
    Serial.flush();

    // Keeping the pull-ups alive means RTC_PERIPH has to stay powered; the
    // alternative saves a few uA and loses the wake sources.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    esp_deep_sleep_start();
}
