<p align="center">
  <img src="assets/hydronode-logo.svg" alt="HydroNode" width="140">
</p>

<h1 align="center">HydroNode-Library</h1>

<p align="center">
  Arduino client (ESP32, ESP8266) for the <strong>HydroNode</strong> IoT backend by TexhFexLabs.<br>
  Signed sensor uploads, TLS out of the box, backend command callbacks — in three lines of code.
</p>

<p align="center">
  <a href="https://www.ardu-badge.com/HydroNode-Library"><img src="https://www.ardu-badge.com/badge/HydroNode-Library.svg" alt="Arduino Library Manager"></a>
  <a href="https://github.com/TexhFexLabs/HydroNode-Library/releases/latest"><img src="https://img.shields.io/github/v/release/TexhFexLabs/HydroNode-Library?label=release&color=2ea44f" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/platform-ESP32%20%7C%20ESP8266-e7352c.svg" alt="ESP32 | ESP8266">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT license"></a>
</p>

<p align="center">
  <a href="https://hydronode.tech/"><strong>Website</strong></a> ·
  <a href="https://hydronode.tech/docs/guide/arduino/">Arduino Guide</a> ·
  <a href="https://hydronode.tech/docs/guide/sensor-types/">Sensor Types</a> ·
  <a href="https://hydronode.tech/docs/faq/">FAQ</a> ·
  <a href="https://github.com/TexhFexLabs/hydronode-homeassistant">Home Assistant Integration</a>
</p>

---

```cpp
HydroNode hydro("sensor-id", "secret-key");
hydro.connectWiFi("ssid", "password");
hydro.sendValue("TEMPERATURE", 21.5);
```

[HydroNode](https://hydronode.tech/) is a secure IoT platform by TexhFexLabs for hydroponics, weather stations and environmental monitoring: cloud data collection, anomaly detection, AI analysis, automation and remote actuation. To use it you need a sensor ID and secret key from the [HydroNode web app](https://hydronode.tech/) or the mobile apps. Using HydroNode is free.

## Features

- **Secure by default** — every request is signed with HMAC-SHA256 and sent over TLS. The root CA bundle ships with the library (valid until 2035+), no certificate handling needed.
- **Replay protection built in** — the backend only accepts requests within a ±2 minute window; the library syncs time via NTP automatically before every send.
- **Typed backend commands** — switch a lamp, run a pump or write a calibration value from the HydroNode app. Each command has a value type (`BOOL`, `INT32`, `UINT32`, `INT64`, `UINT64`, `STRING`) that must match your callback. The app shows every command as *Confirmed* or *Declined* with the reason.
- **WiFi your way** — use the built-in `connectWiFi()` helper, a WiFiManager captive portal, or your own connection management. The library never touches WiFi unless you ask it to.
- **Honest error reporting** — `sendValue()` returns the HTTP status code or a descriptive error code, so your firmware can retry intelligently.
- **Lightweight** — no background tasks, no heap surprises, RAM-friendly.
- **Shows up in the fleet view** — since 1.6.0 every request says which firmware runs and how the board is doing (boot counter, reset reason, signal). Hooks for updates over the air are built in.

**Board support: ESP32 (all variants) and ESP8266 (4 MB flash recommended).** TLS uses `WiFiClientSecure` with the bundled root certificates: mbedTLS on ESP32, BearSSL on ESP8266. The ESP8266 has little RAM for TLS; keep the rest of the sketch lean.

## Installation

### Arduino Library Manager (recommended)

Arduino IDE → **Tools → Manage Libraries** → search for **HydroNode-Library** → Install. The IDE offers to install all dependencies automatically.

### Manual

Install the library via Arduino IDE → Sketch → Include Library → Add .ZIP Library, or clone this repo into your `libraries/` folder. Then install the dependencies via the Library Manager:

| Library | Purpose |
|---|---|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) 7.x | Parsing backend responses |
| [ArduinoHttpClient](https://github.com/arduino-libraries/ArduinoHttpClient) | HTTP transport |
| [NTPClient](https://github.com/arduino-libraries/NTPClient) | Time sync (required for signatures) |
| [WiFiManager](https://github.com/tzapu/WiFiManager) (tzapu) | Only for the captive-portal example |

Signing (HMAC-SHA256) and Base64 use the TLS library that ships with the board package: mbedTLS on ESP32, BearSSL on ESP8266. Nothing extra to install. Up to version 1.2.0 the library needed `Crypto` and `base64_arduino`; you can uninstall them if nothing else uses them.

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
    delay(10000);  // the backend accepts one value per type every 10 s
}
```

## Examples

All examples are complete sketches — open them via *File → Examples → HydroNode-Library*:

| Example | What it shows |
|---|---|
| **QuickStart** | Smallest complete sketch. Built-in WiFi helper, one sensor, done. |
| **WiFiManagerSetup** | No hardcoded WiFi: captive portal (`HydroNode-Setup-XXXX`) for end-user WiFi configuration. |
| **ExternalWiFi** | You own the WiFi lifecycle (custom reconnect logic); full error handling for every `sendValue()` result. |
| **ActuatorControl** | Backend commands drive a pump and a fan via callbacks, with a safety limit for the pump run time. |

## Choosing a WiFi strategy

The library is deliberately WiFi-agnostic. Pick what fits:

1. **Built-in helper** — `hydro.connectWiFi(ssid, pass)` connects in station mode and blocks until connected (30 s default timeout, configurable). Great for simple firmware.
2. **WiFiManager captive portal** — no credentials in code. Use `hydro.getApName()` for a per-device AP name. Great for devices you hand to end users.
3. **Fully external** — connect however you like (ESP-IDF events, enterprise WPA2, ethernet). The library only requires that WiFi is up when you call `sendValue()`; if it isn't, you get `ERR_WIFI_DISCONNECTED` instead of a hang.

## API Reference

### `HydroNode(sensorId, secretKey, host?, path?)`
Constructor. `host` defaults to `hydronode.tech`, `path` to `/api/webhook/sensor-value`.
Versions up to 1.2.0 used `hydronode.texhfexlabs.de`. That host stays available, so devices already in the field keep working without a reflash.

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
| `ERR_INVALID_TYPE` (−4) | `type` is not upper case letters, digits and `_` | Use e.g. `TEMPERATURE`, not `temperature`. Nothing was sent |
| `ERR_INVALID_VALUE` (−5) | `value` is NaN or infinite | Usually a failed sensor read. Nothing was sent |
| `400` | Request rejected as malformed | Enable `setDebug(Serial)`; should not happen with valid input |
| `401` | Bad signature or timestamp | Check sensor ID + secret key |
| `429` | Rate limited | One value per sensor **and type** every 10 s. Slow down |
| `503` | Backend temporarily unavailable | Retry after about 10 s; the value was not stored |

`value` is transmitted with exactly 2 decimal places.

`type` must start with an upper case letter and may contain `A`–`Z`, `0`–`9` and `_`, at most 64 characters. The library checks this before sending. Common `type` values: `TEMPERATURE`, `HUMIDITY`, `PRESSURE`, `CO2`, `PM25`, `PM10`, `VOC`, `SOIL_MOISTURE`, `SOIL_TEMPERATURE`, `WATER_TEMPERATURE`, `WATER_PH`, `WATER_EC`, `BATTERY_VOLTAGE` — see the [sensor type reference](https://hydronode.tech/docs/guide/sensor-types/) for the full list of 30+ types with units and scales.

### `void closeConnection()`

Values sent in a row share one TLS connection (HTTP keep-alive): the handshake takes seconds on an ESP8266, every further value a fraction of that. Close the connection after the last value of a round, before the board sleeps or waits for a long time. The next `sendValue()` connects again.

### `bool syncTime()` and `uint64_t epochMs()`

`sendValue()` keeps the time in sync on its own. In ESP8266 light sleep the clock stands still, so call `syncTime()` after waking up and before sending. `epochMs()` returns the time of the last sync plus `millis()` since then (0 before the first sync), handy to start readings on a fixed schedule.

### Commands: `onBool`, `onInt32`, `onUInt32`, `onInt64`, `onUInt64`, `onString`

In the HydroNode app you send a command to your device with a **name**, a **value type** and a **value**, for example:

| Name | Type | Value | Use |
|---|---|---|---|
| `lamp` | `BOOL` | `true` / `false` | Switch a relay |
| `pump` | `UINT32` | `4000` | Run a pump for 4000 ms |
| `co2_calibration` | `UINT32` | `0x20124` | Write a calibration register (hex is fine) |
| `offset` | `INT32` | `-15` | Signed setting, e.g. tenths of a degree |
| `display` | `STRING` | `Hello` | Show text |

On the device you register one callback per name and type:

```cpp
hydro.onBool("lamp", [](bool on) { digitalWrite(LAMP_PIN, on); });
hydro.onUInt32("pump", [](uint32_t ms) { runPump(ms > 10000 ? 10000 : ms); });
hydro.onUInt32("co2_calibration", [](uint32_t value) { co2.setCalibration(value); });
hydro.onInt32("offset", [](int32_t tenths) { offset = tenths / 10.0; });
hydro.onString("display", [](const String& text) { lcd.print(text); });
```

| Type | Callback | Range |
|---|---|---|
| `BOOL` | `onBool(name, void(bool))` | `true`, `false` |
| `INT32` | `onInt32(name, void(int32_t))` | −2147483648 to 2147483647 |
| `UINT32` | `onUInt32(name, void(uint32_t))` | 0 to 4294967295 |
| `INT64` | `onInt64(name, void(int64_t))` | full signed 64-bit range |
| `UINT64` | `onUInt64(name, void(uint64_t))` | full unsigned 64-bit range |
| `STRING` | `onString(name, void(const String&))` | text up to about 500 characters |

**One name, several types.** Since 1.5.0 a name can have one callback per type. A relay can then be switched with `relay1 true` and pulsed with `relay1 1400`:

```cpp
hydro.onBool("relay1", [](bool on) { digitalWrite(RELAY_PIN, on); });
hydro.onUInt32("relay1", [](uint32_t ms) { pulseFor(RELAY_PIN, ms); });
```

A typed command runs the callback of its type. A command without a type (older app versions) runs the first callback whose type fits the value.

There is no float type on purpose: send a whole number in a fixed unit (tenths, milliseconds, ...) so the app and the device agree on the exact value.

**What the app shows.** Before any callback runs, the library answers the backend with a signed request:

| Status in the app | Meaning |
|---|---|
| *Confirmed* | A callback with the matching type exists and runs now |
| *Declined: no handler* | No callback is registered for this name. Check the spelling |
| *Declined: type mismatch* | Callbacks exist for this name, but none for the type picked in the app |
| *Declined: invalid value* | The value does not fit the type, e.g. `-1` for `UINT32` |

A declined command is never run. Commands without a type (sent by older app versions) run when the value fits the callback's type.

**Timing and safety.**

- Commands arrive with the next `sendValue()`. If your device sends every 5 minutes, a command may take up to 5 minutes to arrive. Unpicked commands expire after 24 hours.
- Every command is delivered at most once. If the response is lost on the way, the command is not repeated; send it again from the app.
- Values come over the network. Clamp them to what your hardware can safely do, as the `ActuatorControl` example does for the pump.
- Callbacks run one after another inside `sendValue()`. Long blocking work (like `delay()` in a pump callback) delays your next measurement.

**Untyped callbacks.** `on(name, void(JsonVariant))` receives any value without a type check, for commands that accept several types. Sketches written for version 1.2.0 and older (`hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback))`) keep compiling and working.

### Fleet and OTA hooks (1.6.0)

Every request now carries two headers for the HydroNode fleet view. A sketch needs nothing for that:

```
X-Firmware: hydronode-lib/1.6.0 esp32c3
X-Device-Status: boot=12;reset=poweron;uptime=45;rssi=-61;net=wifi;readErr=
```

`boot` counts cold starts (power-on, crash, watchdog, restart), not wake-ups from deep sleep. On the ESP32 it lives in NVS (namespace `hn-lib`), on the ESP8266 in the last 8 bytes of the RTC user memory, so there it starts again at 1 after a power cut. `reset` is one of `poweron`, `software`, `panic`, `watchdog`, `brownout`, `deepsleep`, `external`, `ota`, `unknown`.

| Method | Purpose |
|---|---|
| `setFirmwareIdentity(product, version, flags)` | Replaces `hydronode-lib/1.6.0`. The universal firmware sends `hydronode/0.5.0 esp32c3 ota cfg=14`. The chip family is always added |
| `reportReadError("bme280")` / `clearReadErrors()` | Drivers that failed to read this round, sent as `readErr=bme280` |
| `setResetReason("ota")` | Reports this reset reason instead of the chip's own (nullptr: the chip's again) |
| `setExtraHeader(name, value)` / `clearExtraHeader(name)` | A header on every request until cleared, e.g. `X-Ota-State` |
| `onResponseKey("ota", fn)` | Callback for a key in the answer to `sendValue()` besides `commands`. Runs after the commands of the same answer |
| `sendOtaAck(job, result, reason)` | Signed `POST /api/webhook/sensor-ota-ack` with `{"job","result","reason"}`. `result`: `downloaded`, `verified`, `failed`, `config_applied` |
| `downloadSigned(pathAndQuery, offset, onChunk)` | Signed GET that streams a file in 1 KB pieces to `onChunk`, resuming with `Range: bytes=<offset>-`. Returns status, bytes and total size |

```cpp
hydro.onResponseKey("ota", [](JsonVariantConst offer) {
  const char* job = offer["job"];
  size_t done = 0;
  auto result = hydro.downloadSigned(offer["url"], done, [&](const uint8_t* data, size_t n) {
    return writeToUpdateSlot(data, n);   // return false to stop
  });
  hydro.sendOtaAck(job, result.status == 200 || result.status == 206 ? "downloaded" : "failed");
});
```

A GET has no body, so its signature covers the path with query and the timestamp: `X-Signature` = Base64(HMAC-SHA256(`/api/device-ota/v1/image?job=…` + timestamp)). The `Range` header is not signed.

### Tuning

| Method | Default | Purpose |
|---|---|---|
| `setDebug(Serial)` | off | Log WiFi/NTP/HTTP activity to any `Stream` |
| `setHttpTimeout(ms)` | 10000 | HTTP response timeout |
| `setJsonBufferSize(bytes)` | 8192 | Largest response the library parses. Fits the backend maximum of 8 commands per delivery |
| `getApName()` | — | `"HydroNode-Setup-XXXX"` (last 4 chars of sensor ID), for WiFiManager |

## Security model

- **Authentication**: every request (values, command and update ACKs) carries `X-Signature` = Base64(HMAC-SHA256(payload + timestamp)) computed with your secret key; a signed download signs its path and query instead of a body. The key never leaves the device.
- **Command delivery**: the backend hands out queued commands only in responses to correctly signed value submissions — an attacker who knows your sensor ID cannot fetch them.
- **Replay protection**: `X-Timestamp` must be within ±2 minutes of server time. The library syncs via NTP before each send and refuses to transmit with an unsynced clock (`ERR_TIME_NOT_SYNCED`) instead of sending a doomed request.
- **Transport**: TLS 1.2+ against a bundled root CA set (Google Trust Services, ISRG/Let's Encrypt, SSL.com — the roots Cloudflare Universal SSL chains to). All bundled roots are valid until at least 2035, so certificate rotation on the server side never requires a reflash.
- **Rate limiting**: the backend accepts one value per sensor and type every 10 seconds. Different types may be sent right after each other.
- **Actuators**: command values come over the network. Clamp them in your handler to what your hardware can safely do (see the `ActuatorControl` example).

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `ERR_TIME_NOT_SYNCED` | No internet access or UDP port 123 blocked — NTP can't sync |
| `401` | Wrong secret key/sensor ID, or device clock drifted outside the ±2 min window |
| `429` | Sending the same type more often than every 10 s |
| `ERR_INVALID_TYPE` | Lower case or special characters in `type`, e.g. `"temperature"` or `"PM2.5"` |
| `ERR_INVALID_VALUE` | Sensor read failed and returned NaN, e.g. a DHT22 without pull-up |
| `ERR_CONNECTION_FAILED` | DNS/TLS/network issue — enable `setDebug(Serial)` and check the log |

## Obtaining Sensor Credentials

Every device needs a sensor ID and a secret key. Create a sensor with just a name in the [HydroNode web app](https://hydronode.tech/), the iOS app or the Android app, and you receive its ID and secret immediately.

Using HydroNode is free. Each account can have up to 20 sensors.

Treat the secret key like a password. If you publish your sketch, move the credentials into a separate `secrets.h` and keep that file out of version control.

For development or trial setups, contact **support@hydronode.tech**.

## Related

- [HydroNode website & docs](https://hydronode.tech/) — dashboard, guides, FAQ
- [Getting started with Arduino/ESP32](https://hydronode.tech/docs/guide/arduino/) — step-by-step guide for this library
- [LoRaWAN integration](https://hydronode.tech/docs/guide/lorawan/) — battery-powered sensors without WiFi
- [Home Assistant integration](https://github.com/TexhFexLabs/hydronode-homeassistant) — your HydroNode sensors as native HA entities (HACS)

## License

MIT — see [LICENSE](LICENSE).

HydroNode-Library is developed and maintained by **TexhFexLabs**.
Support, feature requests, business inquiries: support@hydronode.tech
