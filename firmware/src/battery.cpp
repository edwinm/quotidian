#include "battery.h"

#include "epd_driver.h"
#include "esp_adc_cal.h"
#include "utilities.h"

// The divider feeding BATT_PIN halves the cell voltage.
static constexpr float kDividerRatio = 2.0f;

// A single 18650/LiPo cell. Below ~3.0 V the protection circuit cuts out.
static constexpr float kEmptyVolts = 3.30f;
static constexpr float kFullVolts  = 4.20f;

// Anything under this is not a battery, it is the divider floating while the
// board runs off USB.
static constexpr float kPresentThreshold = 2.50f;

static uint32_t sVrefMillivolts = 1100;

void batteryBegin() {
    esp_adc_cal_characteristics_t chars;
    esp_adc_cal_value_t type = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &chars);

    if (type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        sVrefMillivolts = chars.vref;
        Serial.printf("[battery] using eFuse Vref: %u mV\n", sVrefMillivolts);
    } else {
        Serial.println("[battery] no eFuse Vref, assuming 1100 mV");
    }
}

BatteryStatus batteryRead() {
    // The sense divider is only powered when the panel rail is on.
    epd_poweron();
    delay(10);

    uint32_t raw = 0;
    for (int i = 0; i < 16; i++) {
        raw += analogRead(BATT_PIN);
    }
    raw /= 16;

    epd_poweroff_all();

    BatteryStatus status;
    status.volts = ((float)raw / 4095.0f) * kDividerRatio * 3.3f * (sVrefMillivolts / 1000.0f);
    status.present = status.volts > kPresentThreshold;

    if (!status.present) {
        status.percent = -1;
        return status;
    }

    if (status.volts > kFullVolts) status.volts = kFullVolts;

    float fraction = (status.volts - kEmptyVolts) / (kFullVolts - kEmptyVolts);
    status.percent = (int)roundf(constrain(fraction, 0.0f, 1.0f) * 100.0f);
    return status;
}
