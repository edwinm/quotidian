#include "wireless.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include "config.h"
#include "rtc.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

// Set from the SNTP callback, which is the only reliable signal that the time
// actually came from the network rather than from the hardware clock.
static volatile bool sNtpUpdated = false;

static void onNtpSync(struct timeval *tv) {
    (void)tv;
    sNtpUpdated = true;
}

bool wifiBegin() {
    return wifiConnect(settingsSsid(), settingsPassword());
}

bool wifiConnect(const String &ssid, const String &password) {
    if (ssid.isEmpty()) {
        Serial.println("[wifi] no credentials, staying offline");
        return false;
    }

    Serial.printf("[wifi] connecting to %s", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    uint32_t deadline = millis() + WIFI_TIMEOUT_MS;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wifi] connection timed out");
        WiFi.mode(WIFI_OFF);
        return false;
    }

    Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());

    // getLocalTime() is no good here: it returns true the moment the clock looks
    // plausible, and the hardware clock has already made it plausible. It would
    // report success without SNTP having answered, and the stale time would then
    // be written straight back into the chip - which is exactly what stopped the
    // wake alarm from firing at the right moment.
    sNtpUpdated = false;
    sntp_set_time_sync_notification_cb(onNtpSync);
    configTzTime(settingsTimezone().c_str(), NTP_SERVER);

    uint32_t ntpDeadline = millis() + 15000;
    while (!sNtpUpdated && (int32_t)(millis() - ntpDeadline) < 0) {
        delay(50);
    }

    if (!sNtpUpdated) {
        Serial.println("[ntp] no response, keeping the hardware clock as-is");
        return true;
    }

    struct tm now;
    getLocalTime(&now, 0);
    Serial.printf("[ntp] synced: %04d-%02d-%02d %02d:%02d:%02d local\n",
                  now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                  now.tm_hour, now.tm_min, now.tm_sec);

    // Only now is it worth carrying into the hardware clock.
    rtcStoreSystemClock();

    return true;
}

bool wifiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifiStop() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
}

String wifiDescription() {
    return wifiConnected() ? WiFi.SSID() : String("offline");
}

// The clock is valid whether it came from NTP this boot or from the PCF8563 on
// wake, so this asks the clock itself rather than tracking how it was set.
bool timeSynced() {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    return (lt.tm_year + 1900) >= 2024;
}

String todayLong() {
    struct tm timeinfo;
    if (!timeSynced() || !getLocalTime(&timeinfo, 0)) {
        return String(__DATE__);
    }

    char buf[64];
    strftime(buf, sizeof(buf), "%A %e %B %Y", &timeinfo);

    String out(buf);
    // %e pads single digits with a space; collapse "Saturday  5 July".
    out.replace("  ", " ");
    return out;
}

bool todayParts(int *year, int *month, int *day) {
    struct tm timeinfo;
    if (!timeSynced() || !getLocalTime(&timeinfo, 0)) return false;

    *year  = timeinfo.tm_year + 1900;
    *month = timeinfo.tm_mon + 1;
    *day   = timeinfo.tm_mday;
    return true;
}

// ---------------------------------------------------------------------------
// BLE
// ---------------------------------------------------------------------------

// Bluetooth SIG assigned numbers for the Battery Service.
static const char *kBatteryService = "0000180F-0000-1000-8000-00805F9B34FB";
static const char *kBatteryLevelChar = "00002A19-0000-1000-8000-00805F9B34FB";

static BLECharacteristic *sBatteryChar = nullptr;
static bool sBleActive = false;

void bleBegin() {
    BLEDevice::init(BLE_DEVICE_NAME);

    BLEServer *server = BLEDevice::createServer();
    BLEService *service = server->createService(kBatteryService);

    sBatteryChar = service->createCharacteristic(
        kBatteryLevelChar,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    sBatteryChar->addDescriptor(new BLE2902());

    service->start();

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(kBatteryService);
    advertising->setScanResponse(true);
    BLEDevice::startAdvertising();

    sBleActive = true;
    Serial.printf("[ble] advertising as \"%s\"\n", BLE_DEVICE_NAME);
}

bool bleActive() {
    return sBleActive;
}

void bleSetBatteryLevel(int percent) {
    if (!sBatteryChar || percent < 0) return;

    uint8_t level = (uint8_t)constrain(percent, 0, 100);
    sBatteryChar->setValue(&level, 1);
    sBatteryChar->notify();
}
