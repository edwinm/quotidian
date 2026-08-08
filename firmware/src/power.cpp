#include "power.h"

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

// How this cycle started, captured once and reused.
//
// Read on first use rather than at each call site. powerDown() re-arms ext1
// before sleeping, and reading the wake status after that would report the
// configuration rather than the event - a footgun for anything that wants to
// display the reason late in the cycle, which drawPowerDiagnostics does.
namespace {
struct WakeState {
    esp_reset_reason_t reset;
    esp_sleep_wakeup_cause_t cause;
    uint64_t mask;
};
}  // namespace

static WakeState sWake;
static bool sWakeCaptured = false;

static const WakeState &wakeState() {
    if (!sWakeCaptured) {
        sWake.reset = esp_reset_reason();
        sWake.cause = esp_sleep_get_wakeup_cause();
        sWake.mask = esp_sleep_get_ext1_wakeup_status();
        sWakeCaptured = true;
    }
    return sWake;
}

static const char *resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

static const char *wakeCauseName(esp_sleep_wakeup_cause_t cause) {
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0";
        case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1";
        case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "TOUCH";
        case ESP_SLEEP_WAKEUP_ULP:       return "ULP";
        case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";
        case ESP_SLEEP_WAKEUP_UART:      return "UART";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "NONE";
        default:                         return "?";
    }
}

void powerLogWakeReason() {
    const WakeState &w = wakeState();
    Serial.printf("[power] reset reason=%d %s, wakeup cause=%d %s, ext1 mask=0x%llX "
                  "(RTC INT=%d button=%d)\n",
                  (int)w.reset, resetReasonName(w.reset),
                  (int)w.cause, wakeCauseName(w.cause), w.mask,
                  (int)((w.mask >> kRtcIntPin) & 1), (int)((w.mask >> kButtonPin) & 1));
}

// The same thing in one short line, for the panel. Serial cannot be opened on
// this board without resetting the chip, which destroys the very evidence being
// read, so the only place these numbers can be seen is the display.
//
// What to look for:
//   rst 8 DEEPSLEEP, wake 3 EXT1   the alarm woke the chip from deep sleep
//   rst 8 DEEPSLEEP, wake 4 TIMER  the alarm was missed, the backstop caught it
//   rst 1 POWERON,   wake 0 NONE   not a wake at all - the board was started
//   rst 9 BROWNOUT                 the supply dipped, despite the detector
//
// The third of those, arriving punctually every night, would mean the alarm is
// switching the board on rather than waking it.
String powerWakeReport() {
    const WakeState &w = wakeState();

    char buf[64];
    snprintf(buf, sizeof(buf), "rst %d %s  wake %d %s 0x%llX",
             (int)w.reset, resetReasonName(w.reset),
             (int)w.cause, wakeCauseName(w.cause), (unsigned long long)w.mask);
    return String(buf);
}

WakeSource powerWakeSource() {
    switch (wakeState().cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            return WakeSource::Timer;
        case ESP_SLEEP_WAKEUP_EXT1: {
            // Both pins can read low at once - the alarm fires while a finger is
            // on the button. A person waiting for a response is the more useful
            // reading, so the button wins.
            const uint64_t mask = wakeState().mask;
            if (mask & (1ULL << kButtonPin)) return WakeSource::Button;
            if (mask & (1ULL << kRtcIntPin)) return WakeSource::Alarm;
            // An ext1 wake with an empty mask. Counted apart from a plain reset
            // rather than lumped in, because "the alarm never fires" and "the
            // alarm fires but does not say which pin" need different fixes and
            // the counters were what made them indistinguishable.
            return WakeSource::Ext1NoMask;
        }
        default:
            return WakeSource::Other;  // power-on, reset, brownout
    }
}

const char *powerWakeSourceName(WakeSource source) {
    switch (source) {
        case WakeSource::Alarm:      return "RTC alarm";
        case WakeSource::Timer:      return "backstop timer";
        case WakeSource::Button:     return "button";
        case WakeSource::Ext1NoMask: return "ext1, no pin reported";
        default:                     return "power-on/reset";
    }
}

// The SD slot sits on VDD3V3, which is NOT switched by PWR_EN, so anything in
// it stays powered through deep sleep and cannot be turned off in software.
// That is why the dataset moved to internal flash - see storage.h.
//
// The pins are still parked, because the slot is still on the board and a card
// may still be in it. The bus has 10k pull-ups to VDD3V3 (R5, R13, R16), so
// leaving a line driven low would sink 3.3 V / 10 k = 330 uA on that pin alone.
// Releasing them to high-impedance lets the pull-ups hold them high at no cost,
// and leaves any card that is present deselected.
//
// SPI is no longer initialised at all now that storage is LittleFS, so there is
// no bus to end - only pads to release.
static void parkSdCardPins() {
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

[[noreturn]] void powerDown(long backstopSeconds) {
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

    // Second wake source, independent of the RTC chip and the I2C bus that talks
    // to it. Everything above depends on the PCF8563 having accepted its alarm:
    // a NAKed write, a flat backup cell, a pulled INT line, and ext1 never
    // fires. That failure is silent and total - the board simply never wakes
    // again, and the only way back is the button.
    //
    // The ESP32's RC oscillator is minutes-per-day out, which rules it out for
    // scheduling and is perfectly good for a net. Armed past the alarm, so it
    // only ever fires when the alarm did not.
    if (backstopSeconds > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)backstopSeconds * 1000000ULL);
        Serial.printf("[power] backstop timer armed for %ld s (%.1f h)\n",
                      backstopSeconds, backstopSeconds / 3600.0);
    }

    Serial.printf("[power] sleeping; wake pins RTC INT=%d button=%d\n",
                  digitalRead(kRtcIntPin), digitalRead(kButtonPin));

    esp_deep_sleep_start();
}
