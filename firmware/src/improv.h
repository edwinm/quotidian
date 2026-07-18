#pragma once

#include <Arduino.h>

// Improv Wi-Fi over Serial (https://www.improv-wifi.com/serial/).
//
// Lets a Chromium browser hand Wi-Fi credentials to the device over the USB
// cable via the Web Serial API - no app, no access point. The protocol shares
// the port with our normal log output; the parser simply ignores anything that
// is not a well-formed Improv packet.

// Return true if the credentials worked. The callback is expected to block
// while it attempts the connection.
typedef bool (*ImprovConnectCallback)(const String &ssid, const String &password);

void improvBegin(ImprovConnectCallback onConnect);

// Feeds pending serial bytes through the parser. Call from loop().
void improvLoop();

// Marks the device as already provisioned, so a browser that connects later
// sees the correct state.
void improvSetProvisioned(bool provisioned);
