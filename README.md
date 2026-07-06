<p align="center">
  <img src="assets/hydronode-logo.svg" alt="HydroNode" width="140">
</p>

<h1 align="center">HydroNode-Library</h1>

<p align="center">
  Arduino/ESP32 client for the <strong>HydroNode</strong> IoT backend by TexhFexLabs.<br>
  Signed sensor uploads, TLS out of the box, backend command callbacks — in three lines of code.
</p>

---

```cpp
HydroNode hydro("sensor-id", "secret-key");
hydro.connectWiFi("ssid", "password");
hydro.sendValue("TEMPERATURE", 21.5);
```

HydroNode is a secure IoT network by TexhFexLabs for hydroponics, weather stations and environmental monitoring: cloud data collection, anomaly detection, automation and remote actuation. To use it you need a sensor ID and secret key from the official **HydroNode iOS app**.

## Features

- **Secure by default** — every request is signed with HMAC-SHA256 and sent over TLS. The root CA bundle ships with the library (valid until 2035+), no certificate handling needed.
- **Replay protection built in** — the backend only accepts requests within a ±2 minute window; the library syncs time via NTP automatically before every send.
- **Backend commands with delivery confirmation** — react to commands queued in the HydroNode app with simple typed callbacks. The library automatically acknowledges receipt (signed), so the app shows every command as *Confirmed*.
- **WiFi your way** — use the built-in `connectWiFi()` helper, a WiFiManager captive portal, or your own connection management. The library never touches WiFi unless you ask it to.
- **Honest error reporting** — `sendValue()` returns the HTTP status code or a descriptive error code, so your firmware can retry intelligently.
- **Lightweight** — no background tasks, no heap surprises, RAM-friendly.

**Board support: ESP32 (all variants).** The library uses the ESP32 TLS API (`WiFiClientSecure::setCACert`); ESP8266 is not supported.

## Installation

1. Install the library (Arduino IDE → Sketch → Include Library → Add .ZIP Library, or clone this repo into your `libraries/` folder).
2. Install the dependencies via the Arduino Library Manager:

| Library | Purpose |
|---|---|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | Parsing backend responses |
| [ArduinoHttpClient](https://github.com/arduino-libraries/ArduinoHttpClient) | HTTP transport |
| [NTPClient](https://github.com/arduino-libraries/NTPClient) | Time sync (required for signatures) |
| [base64_arduino](https://github.com/Densaugeo/base64_arduino) (Densaugeo) | Signature encoding |
| [Crypto](https://github.com/rweather/arduinolibs) (Rhys Weatherley) | SHA-256 |
| [WiFiManager](https://github.com/tzapu/WiFiManager) (tzapu) | Only for the captive-portal example |

## Quick Start

The complete minimal sketch — fill in four values, wire your sensor into `readMySensor()`, flash:

```cpp
#include <HydroNode.h>

const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SENSOR_ID     = "your-sensor-id-here";    // from the HydroNode app
const char* SECRET_KEY    = "your-secret-key-here";   // from the HydroNode app

HydroNode hydro(SENSOR_ID, SECRET_KEY);

float readMySensor() {
    return 21.5;  // TODO: your real measurement (DHT22, BME280, analog probe, ...)
}

void setup() {
    Serial.begin(115200);
    hydro.setDebug(Serial);  // optional

    while (!hydro.connectWiFi(WIFI_SSID, WIFI_PASSWORD)) {
        Serial.println("WiFi failed, retrying...");
    }
    hydro.begin();
}

void loop() {
    hydro.sendValue("TEMPERATURE", readMySensor());
    delay(10000);  // backend enforces min. 9 s between submissions
}
```

## Examples

All examples are complete sketches — open them via *File → Examples → HydroNode-Library*:

| Example | What it shows |
|---|---|
| **QuickStart** | Smallest complete sketch. Built-in WiFi helper, one sensor, done. |
| **WiFiManagerSetup** | No hardcoded WiFi: captive portal (`HydroNode-Setup-XXXX`) for end-user WiFi configuration. |
| **ExternalWiFi** | You own the WiFi lifecycle (custom reconnect logic); full error handling for every `sendValue()` result. |
| **ActuatorControl** | Backend commands drive a pump and a fan via callbacks; alternating sensor types within the rate limit. |

## Choosing a WiFi strategy

The library is deliberately WiFi-agnostic. Pick what fits:

1. **Built-in helper** — `hydro.connectWiFi(ssid, pass)` connects in station mode and blocks until connected (30 s default timeout, configurable). Great for simple firmware.
2. **WiFiManager captive portal** — no credentials in code. Use `hydro.getApName()` for a per-device AP name. Great for devices you hand to end users.
3. **Fully external** — connect however you like (ESP-IDF events, enterprise WPA2, ethernet). The library only requires that WiFi is up when you call `sendValue()`; if it isn't, you get `ERR_WIFI_DISCONNECTED` instead of a hang.

## API Reference

### `HydroNode(sensorId, secretKey, host?, path?)`
Constructor. `host` defaults to `hydronode.texhfexlabs.de`, `path` to `/api/webhook/sensor-value`.

### `void begin()`
Starts the NTP client. Call once in `setup()` after WiFi is available.

### `bool connectWiFi(ssid, password, timeoutMs = 30000)`
Optional WiFi helper. Returns `false` on timeout.

### `int sendValue(const char* type, float value)`
Sends one signed measurement. Returns the HTTP status code, or a negative error:

| Return | Meaning | What to do |
|---|---|---|
| `202` | Accepted | Nothing — data is queued for processing |
| `ERR_WIFI_DISCONNECTED` (−1) | WiFi down | Reconnect, retry |
| `ERR_TIME_NOT_SYNCED` (−2) | NTP failed | Check internet/UDP 123; signature would be rejected without valid time |
| `ERR_CONNECTION_FAILED` (−3) | TLS/TCP/transport error | Retry with backoff |
| `401` | Bad signature or timestamp | Check sensor ID + secret key |
| `429` | Rate limited | Min. 9 s between submissions per sensor — slow down |

`value` is transmitted with exactly 2 decimal places.

Common `type` values: `TEMPERATURE`, `HUMIDITY`, `PRESSURE`, `CO2`, `PM25`, `PM10`, `VOC`, `SOIL_MOISTURE`, `SOIL_TEMPERATURE`, `WATER_TEMPERATURE`, `WATER_PH`, `WATER_EC`, `BATTERY_VOLTAGE` — see the HydroNode app for the full list.

### `void on(key, handler)` — backend command callbacks
Commands queued for your sensor (via the HydroNode app or API) are delivered in the response of `sendValue()`:

```json
{"commands":[{"id":"6f9c...","command":"pump","value":4000},{"id":"a1b2...","command":"fan","value":true}]}
```

Registered handlers are called with the value:

```cpp
void pumpCallback(int ms)  { /* run pump for ms */ }
void fanCallback(bool on)  { /* switch fan */ }

hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));
hydro.on("fan",  HydroNode::bindCallback<bool>(fanCallback));
```

`bindCallback<T>` supports any JSON-convertible type: `int`, `bool`, `float`, `String`, …

**Delivery confirmation:** before dispatching, the library sends a signed acknowledgment to the backend (`/api/webhook/sensor-command-ack`). Commands then show up as *Confirmed* in the HydroNode app. This happens automatically — nothing to configure.

### Tuning

| Method | Default | Purpose |
|---|---|---|
| `setDebug(Serial)` | off | Log WiFi/NTP/HTTP activity to any `Stream` |
| `setHttpTimeout(ms)` | 10000 | HTTP response timeout |
| `setJsonBufferSize(bytes)` | 1024 | Response parse buffer; enough for ~8 commands per delivery |
| `getApName()` | — | `"HydroNode-Setup-XXXX"` (last 4 chars of sensor ID), for WiFiManager |

## Security model

- **Authentication**: every request (values and command ACKs) carries `X-Signature` = Base64(HMAC-SHA256(payload + timestamp)) computed with your secret key. The key never leaves the device.
- **Command delivery**: the backend hands out queued commands only in responses to correctly signed value submissions — an attacker who knows your sensor ID cannot fetch them.
- **Replay protection**: `X-Timestamp` must be within ±2 minutes of server time. The library syncs via NTP before each send and refuses to transmit with an unsynced clock (`ERR_TIME_NOT_SYNCED`) instead of sending a doomed request.
- **Transport**: TLS 1.2+ against a bundled root CA set (Google Trust Services, ISRG/Let's Encrypt, SSL.com — the roots Cloudflare Universal SSL chains to). All bundled roots are valid until at least 2035, so certificate rotation on the server side never requires a reflash.
- **Rate limiting**: the backend accepts max. one submission per sensor per 9 seconds.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `ERR_TIME_NOT_SYNCED` | No internet access or UDP port 123 blocked — NTP can't sync |
| `401` | Wrong secret key/sensor ID, or device clock drifted outside the ±2 min window |
| `429` | Sending faster than every 9 s (also applies when alternating types) |
| `ERR_CONNECTION_FAILED` | DNS/TLS/network issue — enable `setDebug(Serial)` and check the log |
| Compile error on ESP8266 | Not supported — the library requires an ESP32 |

## Obtaining Sensor Credentials

All access to the HydroNode network requires a sensor ID and secret key. **Sensor registration is currently free of charge** — create a sensor with just a name in the HydroNode iOS app or web app and you'll receive its UUID and secret immediately. No activation code purchase needed.

The activation-code system still exists for backward compatibility; previously purchased codes remain redeemable, but new sensors are created directly and at no cost. If running costs grow, a small fee may be introduced later.

For development or trial setups, contact **contact@knollfelix.de**.

## License

MIT — see [LICENSE](LICENSE).

HydroNode-Library is developed and maintained by **TexhFexLabs**.
Support, feature requests, business inquiries: contact@knollfelix.de
