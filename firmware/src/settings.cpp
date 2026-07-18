#include "settings.h"

#include <Preferences.h>

#include "config.h"

static Preferences sPrefs;

// Keys are limited to 15 characters by NVS.
static const char *kNamespace = "qotd";
static const char *kKeySsid   = "ssid";
static const char *kKeyPass   = "pass";
static const char *kKeyTz     = "tz";
static const char *kKeyTzName = "tzname";
static const char *kKeySyncDays = "syncdays";
static const char *kKeyLastDay  = "lastday";

void settingsBegin() {
    sPrefs.begin(kNamespace, false);
    Serial.printf("[settings] credentials %s\n",
                  settingsHasCredentials() ? "present" : "absent");
}

bool settingsHasCredentials() {
    return sPrefs.getString(kKeySsid, "").length() > 0;
}

String settingsSsid() {
    return sPrefs.getString(kKeySsid, "");
}

String settingsPassword() {
    return sPrefs.getString(kKeyPass, "");
}

String settingsTimezone() {
    return sPrefs.getString(kKeyTz, DEFAULT_TIMEZONE);
}

String settingsTimezoneName() {
    return sPrefs.getString(kKeyTzName, "");
}

void settingsSaveCredentials(const String &ssid, const String &password) {
    sPrefs.putString(kKeySsid, ssid);
    sPrefs.putString(kKeyPass, password);
    Serial.printf("[settings] saved credentials for \"%s\"\n", ssid.c_str());
}

void settingsSaveTimezone(const String &posix, const String &ianaName) {
    if (posix.isEmpty()) return;

    sPrefs.putString(kKeyTz, posix);
    sPrefs.putString(kKeyTzName, ianaName);
    Serial.printf("[settings] saved timezone %s (%s)\n", ianaName.c_str(), posix.c_str());
}

int settingsDaysSinceSync() {
    // Large default so the first boot after a flash always syncs.
    return sPrefs.getInt(kKeySyncDays, 9999);
}

void settingsSetDaysSinceSync(int days) {
    sPrefs.putInt(kKeySyncDays, days);
}

String settingsLastRendered() {
    return sPrefs.getString(kKeyLastDay, "");
}

void settingsSetLastRendered(const String &day) {
    sPrefs.putString(kKeyLastDay, day);
}

void settingsClear() {
    sPrefs.clear();
    Serial.println("[settings] cleared");
}
