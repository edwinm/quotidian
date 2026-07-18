#include "wireless.h"

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "settings.h"

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

static bool sTimeSynced = false;

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

    configTzTime(settingsTimezone().c_str(), NTP_SERVER);
    struct tm timeinfo;
    // getLocalTime() polls until the clock leaves 1970.
    sTimeSynced = getLocalTime(&timeinfo, 10000);
    Serial.printf("[ntp] %s\n", sTimeSynced ? "clock synced" : "sync failed");

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

bool timeSynced() {
    return sTimeSynced;
}

String todayLong() {
    struct tm timeinfo;
    if (!sTimeSynced || !getLocalTime(&timeinfo, 0)) {
        return String(__DATE__);
    }

    char buf[64];
    strftime(buf, sizeof(buf), "%A %e %B %Y", &timeinfo);

    String out(buf);
    // %e pads single digits with a space; collapse "Saturday  5 July".
    out.replace("  ", " ");
    return out;
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
