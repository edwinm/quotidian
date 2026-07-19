#include "improv.h"

#include <WiFi.h>

#include <vector>

#include "config.h"

// --- Protocol constants (Improv Serial v1) ----------------------------------

static const char *kMagic = "IMPROV";
static const uint8_t kVersion = 1;

enum PacketType : uint8_t {
    TYPE_CURRENT_STATE = 0x01,
    TYPE_ERROR_STATE   = 0x02,
    TYPE_RPC_COMMAND   = 0x03,
    TYPE_RPC_RESULT    = 0x04,
};

enum State : uint8_t {
    STATE_READY        = 0x02,  // authorised, waiting for credentials
    STATE_PROVISIONING = 0x03,
    STATE_PROVISIONED  = 0x04,
};

enum Error : uint8_t {
    ERROR_NONE              = 0x00,
    ERROR_INVALID_RPC       = 0x01,
    ERROR_UNKNOWN_RPC       = 0x02,
    ERROR_UNABLE_TO_CONNECT = 0x03,
    ERROR_UNKNOWN           = 0xFF,
};

// Values taken from the official SDK (improv-wifi/sdk-cpp, src/improv.h) rather
// than from prose. An earlier version had everything from 0x02 onwards shifted
// by one, so the browser's very first probe - 0x02, "what is your state?" - was
// read as IDENTIFY and answered with nothing. The page simply reported no
// device. A hand-written test client repeated the same numbering and agreed
// with the firmware, which is exactly why it went unnoticed.
enum Command : uint8_t {
    // IDENTIFY and GET_CURRENT_STATE genuinely share 0x02 in the SDK. Answering
    // with the current state is what matters; identifying is optional.
    CMD_WIFI_SETTINGS     = 0x01,
    CMD_GET_CURRENT_STATE = 0x02,
    CMD_GET_DEVICE_INFO   = 0x03,
    CMD_GET_WIFI_NETWORKS = 0x04,
    CMD_HOSTNAME          = 0x05,
    CMD_DEVICE_NAME       = 0x06,
    CMD_GET_NETWORK_STATE = 0x07,
};

// Largest packet we accept: an SSID and password are 32 and 64 bytes plus
// their length prefixes, so 255 covers anything legal.
static constexpr size_t kMaxPayload = 255;

// --- Parser state -----------------------------------------------------------

static ImprovConnectCallback sOnConnect = nullptr;
static uint8_t sState = STATE_READY;

static uint8_t sBuffer[kMaxPayload + 16];
static size_t  sLength = 0;   // bytes currently held in sBuffer
static uint32_t sLastActivity = 0;

// --- Sending ----------------------------------------------------------------

static void sendPacket(uint8_t type, const uint8_t *payload, size_t len) {
    uint8_t packet[kMaxPayload + 16];
    size_t n = 0;

    memcpy(packet, kMagic, 6);
    n = 6;
    packet[n++] = kVersion;
    packet[n++] = type;
    packet[n++] = (uint8_t)len;
    memcpy(packet + n, payload, len);
    n += len;

    // Checksum covers every preceding byte, magic included.
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += packet[i];
    packet[n++] = sum;

    Serial.write(packet, n);
    Serial.write('\n');
    Serial.flush();
}

static void sendState(uint8_t state) {
    sState = state;
    sendPacket(TYPE_CURRENT_STATE, &state, 1);
}

static void sendError(uint8_t error) {
    sendPacket(TYPE_ERROR_STATE, &error, 1);
}

// An RPC result is the command byte, the length of what follows, then a series
// of length-prefixed strings.
static void sendRpcResult(uint8_t command, const String *strings, size_t count) {
    uint8_t payload[kMaxPayload];
    size_t n = 0;

    payload[n++] = command;
    payload[n++] = 0;  // patched below

    for (size_t i = 0; i < count; i++) {
        size_t len = strings[i].length();
        if (n + 1 + len > kMaxPayload) break;
        payload[n++] = (uint8_t)len;
        memcpy(payload + n, strings[i].c_str(), len);
        n += len;
    }

    payload[1] = (uint8_t)(n - 2);
    sendPacket(TYPE_RPC_RESULT, payload, n);
}

// --- Commands ---------------------------------------------------------------

static void handleWifiSettings(const uint8_t *data, size_t len) {
    // Layout: [ssid_len][ssid][pass_len][pass]
    if (len < 2) {
        sendError(ERROR_INVALID_RPC);
        return;
    }

    size_t ssidLen = data[0];
    if (1 + ssidLen + 1 > len) {
        sendError(ERROR_INVALID_RPC);
        return;
    }

    size_t passLen = data[1 + ssidLen];
    if (1 + ssidLen + 1 + passLen > len) {
        sendError(ERROR_INVALID_RPC);
        return;
    }

    // Built byte by byte because the fields are not NUL-terminated.
    String ssid;
    ssid.reserve(ssidLen);
    for (size_t i = 0; i < ssidLen; i++) ssid += (char)data[1 + i];

    String pass;
    pass.reserve(passLen);
    for (size_t i = 0; i < passLen; i++) pass += (char)data[1 + ssidLen + 1 + i];

    sendState(STATE_PROVISIONING);

    bool ok = sOnConnect && sOnConnect(ssid, pass);
    if (!ok) {
        sendState(STATE_READY);
        sendError(ERROR_UNABLE_TO_CONNECT);
        return;
    }

    sendState(STATE_PROVISIONED);
    // The device serves no web UI once running, so return an empty URL list
    // rather than a link that would 404.
    sendRpcResult(CMD_WIFI_SETTINGS, nullptr, 0);
}

static void handleGetDeviceInfo() {
    String info[4] = {
        "Quote of the Day",  // firmware name
        FIRMWARE_VERSION,    // version
        "ESP32-S3",          // chip family
        BLE_DEVICE_NAME,     // device name
    };
    sendRpcResult(CMD_GET_DEVICE_INFO, info, 4);
}

static void handleGetWifiNetworks() {
    int found = WiFi.scanNetworks();

    // A scan reports one result per access point, so a mesh or a dual-band
    // router shows up several times over. The reference implementation filters
    // these out and so does this - a list with the same name four times is
    // worse than useless to somebody picking their network.
    std::vector<String> seen;

    for (int i = 0; i < found; i++) {
        if (WiFi.SSID(i).isEmpty()) continue;

        bool duplicate = false;
        for (const String &s : seen) {
            if (s == WiFi.SSID(i)) { duplicate = true; break; }
        }
        if (duplicate) continue;
        seen.push_back(WiFi.SSID(i));

        String entry[3] = {
            WiFi.SSID(i),
            String(WiFi.RSSI(i)),
            WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES",
        };
        sendRpcResult(CMD_GET_WIFI_NETWORKS, entry, 3);
    }
    WiFi.scanDelete();

    // An empty result terminates the list.
    sendRpcResult(CMD_GET_WIFI_NETWORKS, nullptr, 0);
}

static void handleCommand(const uint8_t *payload, size_t len) {
    if (len < 2) {
        sendError(ERROR_INVALID_RPC);
        return;
    }

    uint8_t command = payload[0];
    uint8_t dataLen = payload[1];
    if (2 + dataLen > len) {
        sendError(ERROR_INVALID_RPC);
        return;
    }

    const uint8_t *data = payload + 2;

    switch (command) {
        case CMD_WIFI_SETTINGS:
            handleWifiSettings(data, dataLen);
            break;
        case CMD_GET_CURRENT_STATE:
            // Also IDENTIFY. There is nothing to blink - the panel is already
            // showing the setup instructions - so answering with the state is
            // the whole job, and it is what lets the browser find the device.
            sendState(sState);
            break;
        case CMD_GET_DEVICE_INFO:
            handleGetDeviceInfo();
            break;
        case CMD_GET_WIFI_NETWORKS:
            handleGetWifiNetworks();
            break;
        default:
            // 0x05 HOSTNAME, 0x06 DEVICE_NAME and 0x07 GET_NETWORK_STATE are
            // deliberately not implemented. The reference implementation
            // (esphome/improv_serial) handles the same four commands and
            // answers everything else exactly this way, so this is the correct
            // response rather than a gap to be filled in later.
            sendError(ERROR_UNKNOWN_RPC);
            break;
    }
}

// --- Incremental parser -----------------------------------------------------

// Consumes one byte. Resets on anything that cannot be a valid packet, which
// is what lets plain log output flow over the same port harmlessly.
static void feed(uint8_t byte) {
    // Bytes 0..5 must spell IMPROV.
    if (sLength < 6) {
        if (byte == (uint8_t)kMagic[sLength]) {
            sBuffer[sLength++] = byte;
        } else {
            sLength = 0;
            // The stray byte may itself start a new header.
            if (byte == (uint8_t)kMagic[0]) sBuffer[sLength++] = byte;
        }
        return;
    }

    sBuffer[sLength++] = byte;

    if (sLength == 7) {  // version
        if (byte != kVersion) sLength = 0;
        return;
    }
    if (sLength < 10) return;  // need type and length too

    size_t payloadLen = sBuffer[8];
    size_t total = 9 + payloadLen + 1;  // header+ver+type+len, payload, checksum
    if (sLength < total) return;

    uint8_t sum = 0;
    for (size_t i = 0; i < total - 1; i++) sum += sBuffer[i];

    if (sum == sBuffer[total - 1] && sBuffer[7] == TYPE_RPC_COMMAND) {
        sLastActivity = millis();
        handleCommand(sBuffer + 9, payloadLen);
    }

    sLength = 0;
}

// --- Public API -------------------------------------------------------------

void improvBegin(ImprovConnectCallback onConnect) {
    sOnConnect = onConnect;
    sLength = 0;
    Serial.println("[improv] listening on serial");
}

uint32_t improvLastActivityMs() {
    return sLastActivity;
}

void improvSetProvisioned(bool provisioned) {
    sState = provisioned ? STATE_PROVISIONED : STATE_READY;
}

void improvLoop() {
    while (Serial.available()) {
        feed((uint8_t)Serial.read());
    }
}
