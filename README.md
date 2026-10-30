<p align="center">
  <img src="assets/hydronode-logo.svg" alt="HydroNode" width="140">
</p>

<h1 align="center">HydroNode-Library</h1>

<p align="center">
  Arduino client for the <strong>HydroNode</strong> IoT backend by TexhFexLabs.<br>
  Signed sensor uploads over WiFi, Ethernet, LTE-M, NB-IoT or any Arduino <code>Client</code>, TLS out of the box,
  backend command callbacks — in three lines of code.
</p>

<p align="center">
  <a href="https://www.ardu-badge.com/HydroNode-Library"><img src="https://www.ardu-badge.com/badge/HydroNode-Library.svg" alt="Arduino Library Manager"></a>
  <a href="https://github.com/TexhFexLabs/HydroNode-Library/releases/latest"><img src="https://img.shields.io/github/v/release/TexhFexLabs/HydroNode-Library?label=release&color=2ea44f" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/platform-ESP32%20%7C%20ESP8266%20%7C%20any%20Arduino%20Client-e7352c.svg" alt="ESP32 | ESP8266 | any Arduino Client">
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
- **Replay protection built in** — the backend only accepts requests within a ±2 minute window; the library keeps the clock in sync via NTP on its own (to the millisecond, only when the last sync is older than 30 minutes).
- **Typed backend commands** — switch a lamp, run a pump or write a calibration value from the HydroNode app. Each command has a value type (`BOOL`, `INT32`, `UINT32`, `INT64`, `UINT64`, `STRING`) that must match your callback. The app shows every command as *Confirmed* or *Declined* with the reason.
- **Any network** — WiFi, Ethernet (built-in or W5500), LTE-M, NB-IoT, LTE Cat-1 or anything that offers an Arduino `Client`. The signature does not care how the bytes travel; you pick a transport, the rest of the sketch stays the same. Only what you include is compiled: a WiFi sketch carries no modem code.
- **WiFi fallback to LTE** — `HydroNodeFailover` uses WiFi while it is up and switches to the modem when it is not.
- **Never lose a reading** — attach a `HydroNodeBuffer` and readings that could not be sent wait (in RAM, RTC memory or flash) and go out later with the time they were measured.
- **Send less often** — `record()` stores a reading without radio, `flush()` sends many in one request: a cellular board measures every 5 minutes and sends once an hour.
- **WiFi your way** — use the built-in `connectWiFi()` helper, a WiFiManager captive portal, or your own connection management. The library never touches WiFi unless you ask it to.
- **One request per reading** — `sendValues()` sends all values of a reading together, with the time they were measured. Saves a round trip per value.
- **Honest error reporting** — `sendValue()` returns the HTTP status code or a descriptive error code, so your firmware can retry intelligently.
- **Lightweight** — no background tasks, no heap surprises, RAM-friendly.
- **Battery thresholds** — `HydroNodeBatteryGuard` saves power below Save, turns WiFi off below Recovery and sleeps deep below Standby, like the HydroNode station. Interval and thresholds show up in HydroNode and can be changed there (`onSettings()`).
- **Shows up in the fleet view** — since 1.6.0 every request says which firmware runs and how the board is doing (boot counter, reset reason, signal). Hooks for updates over the air are built in.

**Board support.** ESP32 (all variants) and ESP8266 (4 MB flash recommended) work out of the box: `HydroNode` brings WiFi, TLS with the bundled root certificates (mbedTLS on ESP32, BearSSL on ESP8266) and SNTP. Since 1.9.0 every other 32-bit board with a TLS-capable `Client` works through `HydroNodeClient`: RP2040 (Pico W), SAMD21/51 (MKR, Nano 33 IoT, Feather M0/M4), nRF52, STM32, Renesas (UNO R4 WiFi). AVR boards (UNO R3, Mega) have too little RAM for TLS. The ESP8266 has little RAM for TLS; keep the rest of the sketch lean.

## Installation

### Arduino Library Manager (recommended)

Arduino IDE → **Tools → Manage Libraries** → search for **HydroNode-Library** → Install. The IDE offers to install all dependencies automatically.

### Manual

Install the library via Arduino IDE → Sketch → Include Library → Add .ZIP Library, or clone this repo into your `libraries/` folder. Then install the dependencies via the Library Manager:

| Library | Purpose |
|---|---|
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) 7.x | Parsing backend responses |
| [ArduinoHttpClient](https://github.com/arduino-libraries/ArduinoHttpClient) | HTTP transport |
| [WiFiManager](https://github.com/tzapu/WiFiManager) (tzapu) | Only for the captive-portal example |
| [TinyGSM](https://github.com/vshymanskyy/TinyGSM) (LGPL-3.0) | Only for cellular (`HydroNodeCellular.h`) |
| [ESP_SSLClient](https://github.com/mobizt/ESP_SSLClient) (MIT) | Only for TLS on the microcontroller over a plain `Client` (W5500, modems) |
| [Ethernet](https://github.com/arduino-libraries/Ethernet) | Only for W5500/W5100 modules |

Signing (HMAC-SHA256) uses the TLS library that ships with the board package: mbedTLS on ESP32, BearSSL on ESP8266, a small built-in SHA-256 on every other board. Nothing extra to install. Up to version 1.2.0 the library needed `Crypto` and `base64_arduino`, up to 1.6.0 `NTPClient`; you can uninstall them if nothing else uses them.

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
| **PowerThresholds** | Battery on an ADC divider, `HydroNodeBatteryGuard`, deep sleep. Interval and thresholds come from HydroNode and survive a power cut. |
| **Transport_WiFi** | QuickStart written the 1.9.0 way: transport and client as two objects. Start here before swapping WiFi for something else. |
| **Ethernet_ESP32** | Built-in Ethernet (WT32-ETH01, Olimex ESP32-POE): a cable instead of WiFi. |
| **Ethernet_W5500** | W5500 module on SPI, TLS on the microcontroller with ESP_SSLClient. Any board. |
| **Cellular_SendNow** | LTE-M / NB-IoT with a SIM7080G: every reading goes out right away. |
| **Cellular_ModemTLS** | LTE Cat-1 (A7670) with TLS inside the modem: less RAM on the microcontroller. |
| **WiFi_Fallback_LTE** | WiFi first, LTE-M when WiFi is gone, back to WiFi when it returns. |
| **Board_MKR_WiFi1010** | A board that is no ESP (SAMD21 + NINA): the pattern for UNO R4 WiFi, Pico W, MKR NB 1500. |
| **Board_AnyClient_Template** | Fill-in template for any board and network: four questions, done. |
| **Cellular_CollectHourly** | LTE-M on a battery: measure every 6 minutes, send the 10 readings once an hour, deep sleep in between. |
| **Buffer_OfflineSafe** | WiFi as usual, but readings wait in a buffer (saved to flash) while the network is down. |
| **Buffer_DeepSleep** | WiFi battery sensor: measure every 5 minutes, WiFi only every 30 minutes. ESP32 and ESP8266. |

## Choosing a network

`HydroNode` is the WiFi client you know. Underneath it is `HydroNodeClient`, which sends over any
**transport**. The transport answers four questions: is the link up, which `Client` speaks TLS,
where does the time come from, how strong is the signal. Pick one, the rest of the sketch stays
the same:

```cpp
HydroNodeWiFi wifi;                                    // HOW the board reaches the internet
HydroNodeClient hydro(wifi, SENSOR_ID, SECRET_KEY);    // WHAT it sends, always the same
```

| Network | Transport | Header | TLS | Time | Example |
|---|---|---|---|---|---|
| ESP32 / ESP8266 WiFi | `HydroNodeWiFi` (inside `HydroNode`) | `HydroNodeWiFi.h` | built in | SNTP | QuickStart, Transport_WiFi |
| ESP32 Ethernet (LAN8720) | `HydroNodeEsp32Ethernet` | `HydroNodeEsp32Ethernet.h` | built in | SNTP | Ethernet_ESP32 |
| W5500 / W5100 module | `HydroNodeClientTransport` | `HydroNodeTransport.h` | ESP_SSLClient | SNTP (`setUdp`) | Ethernet_W5500 |
| LTE-M / NB-IoT / Cat-1 / 2G | `HydroNodeCellular<TinyGsm>` | `HydroNodeCellular.h` | ESP_SSLClient or the modem | modem clock | Cellular_SendNow, Cellular_ModemTLS |
| WiFi, else LTE | `HydroNodeFailover` | `HydroNodeTransport.h` | both | both | WiFi_Fallback_LTE |
| Anything else | `HydroNodeClientTransport` | `HydroNodeTransport.h` | your `Client` | your callback | Board_MKR_WiFi1010, Board_AnyClient_Template |

Only included headers are compiled. A WiFi-only sketch is as small as before 1.9.0 (+2 KB);
the cellular code exists only in sketches that include `HydroNodeCellular.h`.

**Writing your own transport** takes one class: derive from `HydroNodeTransport` and implement
`network()`, `isUp()` and `client()`; `udp()`, `networkTimeMs()`, `signalDbm()` and
`beforeConnect()` are optional. `HydroNodeClientTransport` does the same with callbacks, without a class.

**Time sources**, tried in this order: `hydro.setTimeProvider(fn)` (RTC chip, GPS), SNTP over the
transport's UDP socket, the transport's network clock (modem). The signature carries the time and
HydroNode accepts ±2 minutes, so any of them is precise enough.

### WiFi strategies

The WiFi transport is deliberately hands-off. Pick what fits:

1. **Built-in helper** — `hydro.connectWiFi(ssid, pass)` connects in station mode and blocks until connected (30 s default timeout, configurable). Great for simple firmware.
2. **WiFiManager captive portal** — no credentials in code. Use `hydro.getApName()` for a per-device AP name. Great for devices you hand to end users.
3. **Fully external** — connect however you like (ESP-IDF events, enterprise WPA2). The library only requires that WiFi is up when you call `sendValue()`; if it isn't, you get `ERR_NETWORK_DOWN` (formerly `ERR_WIFI_DISCONNECTED`, same value) instead of a hang.

## API Reference

### `HydroNode(sensorId, secretKey, host?, path?)`
Constructor of the WiFi client (ESP32, ESP8266). `host` defaults to `hydronode.tech`, `path` to `/api/webhook/sensor-value`.
Versions up to 1.2.0 used `hydronode.texhfexlabs.de`. That host stays available, so devices already in the field keep working without a reflash.

### `HydroNodeClient(transport, sensorId, secretKey, host?, path?, port?)`
The same client for any transport (since 1.9.0). Every method below exists on both; `HydroNode`
adds `connectWiFi()` and `wifi()`.

### `void begin()`
Call once in `setup()`. Since 1.7.0 there is nothing to start; the time is fetched before the first request.

### `bool connectWiFi(ssid, password, timeoutMs = 30000)`
Optional WiFi helper. Returns `false` on timeout.

### `int sendValue(const char* type, float value)`
Sends one signed measurement. Returns the HTTP status code, or a negative error:

| Return | Meaning | What to do |
|---|---|---|
| `202` | Accepted | Nothing — data is queued for processing |
| `ERR_NETWORK_DOWN` (−1) | Network down (WiFi not joined, cable out, modem not attached). Also known as `ERR_WIFI_DISCONNECTED` | Reconnect, retry |
| `ERR_TIME_NOT_SYNCED` (−2) | No time source answered | Check internet/UDP 123 or the modem clock; signature would be rejected without valid time |
| `ERR_CONNECTION_FAILED` (−3) | TLS/TCP/transport error | Retry with backoff |
| `ERR_INVALID_TYPE` (−4) | `type` is not upper case letters, digits and `_` | Use e.g. `TEMPERATURE`, not `temperature`. Nothing was sent |
| `ERR_INVALID_VALUE` (−5) | `value` is NaN or infinite | Usually a failed sensor read. Nothing was sent |
| `ERR_STORED` (−7) | Not sent, kept in the attached buffer | Nothing: it goes out with the next successful send or `flush()` |
| `400` | Request rejected as malformed | Enable `setDebug(Serial)`; should not happen with valid input |
| `401` | Bad signature or timestamp | Check sensor ID + secret key |
| `429` | Rate limited | One value per sensor **and type** every 10 s. Slow down |
| `503` | Backend temporarily unavailable | Retry after about 10 s; the value was not stored |

`value` is transmitted with exactly 2 decimal places.

`type` must start with an upper case letter and may contain `A`–`Z`, `0`–`9` and `_`, at most 64 characters. The library checks this before sending. Common `type` values: `TEMPERATURE`, `HUMIDITY`, `PRESSURE`, `CO2`, `PM25`, `PM10`, `VOC`, `SOIL_MOISTURE`, `SOIL_TEMPERATURE`, `WATER_TEMPERATURE`, `WATER_PH`, `WATER_EC`, `BATTERY_VOLTAGE` — see the [sensor type reference](https://hydronode.tech/docs/guide/sensor-types/) for the full list of 30+ types with units and scales.

### `int sendValues(const HydroNodeValue* values, size_t count, int* codes = nullptr, uint32_t ageMs = 0)`

Sends all values of one reading in one request (since 1.7.0). They arrive together and carry one timestamp: the time they were measured, `ageMs` milliseconds before the call (0 = now).

```cpp
HydroNodeValue v[] = {{"TEMPERATURE", t}, {"HUMIDITY", h}};
int codes[2];
int status = hydro.sendValues(v, 2, codes);
```

Returns the HTTP status (`202` when at least one value was accepted) or a negative error as for `sendValue()`. `codes` gets the result per value: `202`, the status the backend gave that value (`429` rate limited, `400` invalid, `503`), or `ERR_INVALID_TYPE` / `ERR_INVALID_VALUE` for a value the library skipped without sending. Each type may appear once per call, at most 64 values. Commands arrive in the answer as with `sendValue()`. Against a backend without the batch endpoint the library falls back to one `sendValue()` per value.

### Buffer: store now, send later (1.9.0)

A `HydroNodeBuffer` keeps rounds until they reach HydroNode. It lives in memory you give it, so
you decide where it survives: plain RAM, ESP32 RTC memory (deep sleep) or flash (power cut, save
`data()`/`size()` yourself).

```cpp
RTC_DATA_ATTR uint8_t mem[4096];              // survives deep sleep on the ESP32
HydroNodeBuffer buffer(mem, sizeof(mem));

void setup() {
  buffer.begin();                             // keeps what was stored before the sleep
  hydro.attachBuffer(buffer);
}

// A: never lose a value. sendValues() stores what it could not send (returns ERR_STORED)
//    and sends the stored rounds after the next success.
hydro.sendValues(v, 2);

// B: send less often. Store without radio, send many in one request.
hydro.record(v, 2);
if (hydro.flushDue(10, 3600)) hydro.flush();  // 10 rounds stored, or the oldest is 1 h old
```

| Method | Purpose |
|---|---|
| `HydroNodeBuffer(memory, size, whenFull)` | `DROP_OLDEST` (default: newest data wins) or `DROP_NEWEST` |
| `buffer.begin()` | Checks the memory (magic, checksum, structure); keeps intact rounds, else starts empty |
| `attachBuffer(buffer)` | Use it for `record()`, `flush()` and failed sends |
| `record(values, count, ageMs)` | Store one round, no network. Works before the first time sync: the round is dated once the clock is set |
| `flush(maxRequests)` | Send the stored rounds, oldest first, `setRoundsPerRequest()` per request (default 12). Rounds the server took or refused for good leave the buffer, rounds it could not take now (429, 503) stay. Returns the last HTTP status, 0 when empty |
| `flushDue(minRounds, maxAgeSeconds)` | True when enough rounds are stored or the oldest is old enough |
| `setStoreOnFailure(bool)` / `setFlushAfterSend(bool)` | Turn the automatic parts of A off (default on) |
| `buffer.rounds()`, `fillPercent()`, `dropped()`, `oldestTime()`, `clear()` | State for logs and decisions |

A round with two values (`TEMPERATURE`, `HUMIDITY`) takes 39 bytes: 2 KB hold about 50 rounds.
Stored rounds go to `POST /api/webhook/sensor-rounds`: each keeps its measuring time (up to
7 days back) and its own signature, and a round sent twice is stored once. HydroNode learns the
sending rhythm from the deliveries, so a board that sends once an hour is not reported as delayed.
`sendValues(..., ageMs)` with an `ageMs` over 90 s takes this way on its own when a buffer is attached.

Code: `ERR_STORED` (−7) not sent but kept, `ERR_NO_BUFFER` (−6) `record()`/`flush()` without a buffer.

### `void closeConnection()`

Values sent in a row share one TLS connection (HTTP keep-alive): the handshake takes seconds on an ESP8266, every further value a fraction of that. Close the connection after the last value of a round, before the board sleeps or waits for a long time. The next `sendValue()` connects again.

### `bool syncTime()` and `uint64_t epochMs()`

Sending keeps the time in sync on its own: when the clock is not valid yet or the last sync is older than 30 minutes (`setTimeResyncInterval(seconds)`), it asks an NTP server (or the modem, or your `setTimeProvider()`) and sets the clock to the millisecond. `setTimeServers(a, b, c)` replaces the default NTP servers. On the ESP32 the clock keeps running through deep sleep, so a wake-up sends without an NTP exchange; the ESP8266 syncs once per boot. A timestamp the backend rejects triggers a resync and one retry. In ESP8266 light sleep the clock stands still, so call `syncTime()` after waking up and before sending. `epochMs()` returns the system clock in milliseconds (0 while it is not valid), handy to start readings on a fixed schedule.

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
X-Firmware: hydronode-lib/1.9.0 esp32c3
X-Device-Status: boot=12;reset=poweron;uptime=45;rssi=-61;net=wifi;readErr=
```

`boot` counts cold starts (power-on, crash, watchdog, restart), not wake-ups from deep sleep. On the ESP32 it lives in NVS (namespace `hn-lib`), on the ESP8266 in the last 8 bytes of the RTC user memory, so there it starts again at 1 after a power cut. `reset` is one of `poweron`, `software`, `panic`, `watchdog`, `brownout`, `deepsleep`, `external`, `ota`, `unknown`. `net` is the transport's network (`wifi`, `eth`, `lte-m`, `nb-iot`, `lte`, `2g`, `cellular`) and `rssi` its signal in dBm (cellular: from CSQ). Boards other than the ESP send no `boot` counter and `reset=unknown` unless you call `setResetReason()`.

| Method | Purpose |
|---|---|
| `setFirmwareIdentity(product, version, flags)` | Replaces `hydronode-lib/1.9.0`. The universal firmware sends `hydronode/0.5.0 esp32c3 ota cfg=14`. The chip family is always added |
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

A sketch built on the library appears in the HydroNode fleet view as **own sketch**, with its health, restarts, signal and read errors. HydroNode updates only its universal firmware over the air (flashed with the device builder); the hooks above are the building blocks if a sketch wants its own update path. How the universal firmware uses them, including the offer format, verification and rollback, is described in [hydronode-firmware/docs/OTA.md](https://github.com/TexhFexLabs/hydronode-firmware/blob/main/docs/OTA.md). Changes per version: [CHANGELOG.md](CHANGELOG.md).

### Device settings and battery thresholds (1.8.0)

A battery board can report what it runs and take changes from the sensor settings in HydroNode
("On the device"): the send interval and four thresholds in pack millivolts (volts per cell × cells).

| Threshold | Below it the board ... |
|---|---|
| Save | sends half as often. Back to normal at Save + 0.15 V |
| Recovery | turns WiFi and outputs off and only checks the battery. Also after 3 failed readings |
| Standby | sleeps as deep as it can and wakes once an hour (2 valid readings in Recovery) |
| Resume | runs again at or above it: 2 valid readings at least 60 s apart |

Rules (pack mV): Standby + 50 ≤ Recovery, Recovery + 50 ≤ Save, Recovery + 100 ≤ Resume ≤ Save + 400.
LiPo preset per cell: 3.50 / 3.30 / 3.20 / 3.60 V.

```cpp
HydroNodeBatteryGuard guard;          // LiPo, one cell

HydroNodeDeviceConfig config;
config.intervalSeconds = 300;
config.saveMv = 3500; config.recoveryMv = 3300; config.standbyMv = 3200; config.resumeMv = 3600;
config.source = "bat";                // usb, bat or solar
config.gauge = "adc";                 // or max17048, ina226, ...
config.cells = 1;
config.powerState = guard.stateName();
config.revision = storedRevision;     // 0 until HydroNode sent values
hydro.setDeviceConfig(config);

hydro.onSettings([](const HydroNodeSettings& s) {
  store(s);                           // e.g. Preferences, so a power cut keeps them
  guard.setThresholds({s.saveMv, s.recoveryMv, s.standbyMv, s.resumeMv});
  return true;                        // false refuses, HydroNode shows it as failed
});

guard.update(batteryMv, readingOk, secondsIncludingSleep);
if (guard.radioAllowed()) sendRound();
sleep(guard.sleepSeconds(interval));  // interval, 2x in SAVE, 60 s in RECOVERY, 1 h in STANDBY
```

| Method | Purpose |
|---|---|
| `setDeviceConfig(config)` | Values the board runs, sent as `X-Device-Config` with the first request after boot and after a change |
| `setPowerState("save")` | `pwr=` in `X-Device-Status`, so HydroNode can tell a low battery from a dead board |
| `onSettings(fn)` | Takes changes from HydroNode (answer key `settings`). Adds `caps=settings`; without it the values are read only in HydroNode |
| `HydroNodeBatteryGuard` | `update(mv, valid, nowSeconds)`, `state()`, `changed()`, `radioAllowed()`, `sleepSeconds(interval)`, `memory()`/`restore()` for RTC memory, `defaults(chemistry, cells)`, `validate(...)` |

Send one last round when the guard leaves NORMAL/SAVE (`changed()`), so HydroNode shows
"Low battery standby" instead of "Offline". The guard never sleeps or switches pins itself;
the `PowerThresholds` example shows the whole loop.

### Tuning

| Method | Default | Purpose |
|---|---|---|
| `setDebug(Serial)` | off | Log WiFi/NTP/HTTP activity to any `Stream` |
| `setHttpTimeout(ms)` | 10000 | HTTP response timeout (cellular: 30000) |
| `setTimeProvider(fn)` | — | Your own clock (RTC, GPS) in Unix ms, tried before NTP |
| `setTimeServers(a, b, c)` | pool.ntp.org, time.google.com, time.cloudflare.com | NTP servers for the SNTP sync |
| `setTimeResyncInterval(s)` | 1800 | How old the last time sync may get before a request syncs again |
| `setJsonBufferSize(bytes)` | 8192 | Largest response the library parses. Fits the backend maximum of 8 commands per delivery |
| `getApName()` | — | `"HydroNode-Setup-XXXX"` (last 4 chars of sensor ID), for WiFiManager |

## Security model

- **Authentication**: every request (values, command and update ACKs) carries `X-Signature` = Base64(HMAC-SHA256(payload + timestamp)) computed with your secret key; a signed download signs its path and query instead of a body. The key never leaves the device.
- **Command delivery**: the backend hands out queued commands only in responses to correctly signed value submissions — an attacker who knows your sensor ID cannot fetch them.
- **Replay protection**: `X-Timestamp` must be within ±2 minutes of server time. The library keeps the clock synced via NTP and refuses to transmit with an unsynced clock (`ERR_TIME_NOT_SYNCED`) instead of sending a doomed request.
- **Transport**: TLS 1.2+ against a bundled root CA set (`HydroNodeCerts.h`; your own TLS client should use it too) (Google Trust Services, ISRG/Let's Encrypt, SSL.com — the roots Cloudflare Universal SSL chains to). All bundled roots are valid until at least 2035, so certificate rotation on the server side never requires a reflash.
- **Rate limiting**: the backend accepts one value per sensor and type every 10 seconds. Different types may be sent right after each other.
- **Actuators**: command values come over the network. Clamp them in your handler to what your hardware can safely do (see the `ActuatorControl` example).

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `ERR_NETWORK_DOWN` | The transport says the link is down: WiFi not joined, cable out, modem not attached (`modem.gprsConnect()`) |
| `ERR_TIME_NOT_SYNCED` | No internet access or UDP port 123 blocked — NTP can't sync. Cellular: the modem has no network time yet |
| `401` | Wrong secret key/sensor ID, or device clock drifted outside the ±2 min window |
| `429` | Sending the same type more often than every 10 s |
| `ERR_INVALID_TYPE` | Lower case or special characters in `type`, e.g. `"temperature"` or `"PM2.5"` |
| `ERR_INVALID_VALUE` | Sensor read failed and returned NaN, e.g. a DHT22 without pull-up |
| `ERR_CONNECTION_FAILED` | DNS/TLS/network issue — enable `setDebug(Serial)` and check the log. With TLS on the microcontroller: is `setX509Time()` set in `onBeforeConnect()`? |

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

## Guides

Step-by-step walkthroughs with screenshots on the [HydroNode blog](https://hydronode.tech/blog/):

- [Your first ESP32 sensor with this library](https://hydronode.tech/blog/esp32-arduino-library-quickstart/): install, wire a BME280, send a whole reading with `sendValues()`, switch a relay with typed commands
- [Debugging with the device console](https://hydronode.tech/blog/debug-with-device-console/): read 401, 429 and type errors, check the device clock, test commands from a prompt
- [Anomalies, ranges and AI analysis](https://hydronode.tech/blog/anomaly-detection-and-ai-analysis/): what HydroNode does with your readings
- [No code at all](https://hydronode.tech/blog/esp32-c3-without-code/): the Device Builder flashes the same kind of sensor from the browser and updates it over the air

## License

MIT — see [LICENSE](LICENSE).

HydroNode-Library is developed and maintained by **TexhFexLabs**.
Support, feature requests, business inquiries: support@hydronode.tech
