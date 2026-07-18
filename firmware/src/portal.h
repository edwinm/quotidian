#pragma once

#include <Arduino.h>

// SoftAP + captive portal. This is the universal fallback: unlike Improv it
// needs no Web Serial or Web Bluetooth, so it also works from an iPhone, where
// Safari supports neither.
//
// The access point is WPA2 protected with a per-boot random password. The user
// never types it - it is embedded in the QR code shown on the e-paper, and
// phones join straight from a scan. That keeps the home Wi-Fi password off an
// open, sniffable network.

// Called when the form is submitted. `posixTz` and `ianaTz` come from the
// browser and may be empty.
//
// Deliberately does NOT attempt the connection inline. Bringing the station up
// retunes the radio to the home network's channel, which kicks the phone off
// our access point - so an inline attempt would often mean the user never sees
// the response. Instead the portal answers "saved" immediately and the caller
// connects afterwards, reporting the outcome on the e-paper.
typedef void (*PortalSubmitCallback)(const String &ssid, const String &password,
                                     const String &posixTz, const String &ianaTz);

void portalBegin(PortalSubmitCallback onSubmit);

// Services DNS and HTTP. Call from loop() while in setup mode.
void portalLoop();

void portalStop();

String portalSsid();
String portalPassword();

// Standard Wi-Fi QR payload, understood natively by iOS and Android cameras.
String portalQrPayload();
