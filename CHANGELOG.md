# Changelog

All notable changes to HydroNode-Library are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the library uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.8.0] - 2026-10-09

### Added

- `setDeviceConfig(config)` reports the send interval and the four battery thresholds the board
  runs (pack mV), plus power source, gauge, cells and capacity, as
  `X-Device-Config: v=1 rev=7 int=300 save=3500 rec=3300 sby=3200 res=3600 src=bat cells=1 pwr=normal`.
  Sent with the first request after boot, after a change and after a refusal. HydroNode shows the
  values in the sensor settings under "On the device".
- `onSettings(callback)` takes changed values from HydroNode (answer key `settings`). Return
  `true` to keep them: the library reports them with their revision (`rev=`). Return `false` to
  refuse (`rej=`). Values that break the rules (interval 10 to 604800 s, gaps between the
  thresholds) are refused before the callback with `err=invalid`. One revision runs once; an
  offer of the same revision only reports the outcome again. Adds `caps=settings`.
- `setPowerState(state)`: `pwr=normal|save|recovery|standby` in `X-Device-Status` and
  `X-Device-Config`. HydroNode shows "Low battery standby" instead of "Offline" for a board that
  reported `recovery` or `standby` before going quiet.
- `HydroNodeBatteryGuard`: the battery state machine of the HydroNode station for your own sketch
  (NORMAL, SAVE, RECOVERY, STANDBY; Save exit at save + 150 mV, resume after 2 readings 60 s
  apart, invalid readings never lead to standby, 3 invalid ones turn the radio off). Plain C++
  with a `Memory` struct for RTC memory, presets per chemistry and `validate()` with the same
  rules HydroNode uses. The universal HydroNode firmware uses the same class.
- Example `PowerThresholds`: battery on an ADC divider, guard, deep sleep, settings kept in
  Preferences (ESP32) or EEPROM (ESP8266).
- Native tests in `extras/test` (`python3 extras/test/run_tests.py`), including the shared
  threshold rule table.

## [1.7.1] - 2026-10-07

### Changed

- `library.properties` points `url` to the Arduino guide on hydronode.tech, so the Arduino Library
  Manager and PlatformIO link to the documentation instead of the repository.
- README links the step-by-step guides on the HydroNode blog. No code changes.

## [1.7.0] - 2026-10-05

### Added

- `sendValues(values, count, codes, ageMs)` sends all values of one reading in one request to
  `/api/webhook/sensor-values`. They arrive together and carry one timestamp, the time they were
  measured (`ageMs` before the call). `codes` gets the status per value (202, 429, ...). One
  request instead of one per value saves a round trip per value and radio time. A backend without
  the batch endpoint (404/405) gets one `sendValue()` per value instead.
- `setTimeResyncInterval(seconds)`: how old the last time sync may get (default 30 min).

### Changed

- Time comes from the system clock, set by a built-in SNTP query to the millisecond on every sync
  (before: whole seconds, the system clock set once). Requests sync only when the clock is not
  valid or the last sync is older than the resync interval, no longer every 60 s with a blocking
  exchange. On the ESP32 the clock and the last sync survive deep sleep: a wake-up with a recent
  sync sends without an NTP exchange. The ESP8266 still syncs once per boot.
- A value the backend rejects with 401 "Invalid timestamp" triggers a resync and one retry.
- `epochMs()` reads the system clock (millisecond resolution).

### Removed

- The dependency on NTPClient.


## [1.6.0] - 2026-10-02

### Added

- Every request (values, command acks, update acks, downloads) carries
  `X-Firmware: hydronode-lib/1.6.0 <chip>` and
  `X-Device-Status: boot=<n>;reset=<reason>;uptime=<s>;rssi=<dBm>;net=wifi;readErr=<drivers>`
  for the HydroNode fleet view. Nothing to configure.
- Boot counter for cold starts: NVS namespace `hn-lib` on the ESP32, RTC user memory on the
  ESP8266 (starts again after a power cut). Deep sleep wake-ups do not count.
- `setFirmwareIdentity(product, version, flags)` replaces the default identity. The HydroNode
  universal firmware sends `hydronode/0.5.0 esp32c3 ota cfg=14`.
- `reportReadError(driverId)` and `clearReadErrors()` for `readErr=`.
- `setResetReason(reason)` to report a reason such as `ota` instead of the chip's own.
- `setExtraHeader(name, value)` and `clearExtraHeader(name)` for headers on every request, used
  for `X-Ota-State` and `X-Ota-Result`.
- `onResponseKey(key, callback)` for keys in the answer to `sendValue()` besides `commands`,
  for example `ota` and `config`. Runs after the commands of the same answer.
- `sendOtaAck(job, result, reason)` posts a signed `{"job","result","reason"}` to
  `/api/webhook/sensor-ota-ack`.
- `downloadSigned(pathAndQuery, offset, onChunk)` streams a signed GET in 1 KB pieces and resumes
  with `Range: bytes=<offset>-`. The signature covers the path with query and the timestamp.

### Notes

- A sketch built on the library shows up in the fleet view as "own sketch", with health, restarts
  and errors. HydroNode only updates its universal firmware over the air; the hooks above let a
  sketch implement its own update path.

## [1.5.0] - 2026-10-02

### Added

- Several value types per command name (`relay1 true` and `relay1 1400`).
- Keep-alive per round: values of one round share one TLS connection, `closeConnection()` ends it.
- `syncTime()` and `epochMs()`.

## [1.4.1] - 2026-10-01

### Fixed

- ESP8266 TLS fits the heap: only ECDSA roots are bundled. NTP falls back to further servers.

## [1.4.0] - 2026-09-30

### Added

- ESP8266 support through BearSSL.

## [1.3.0] - 2026-09-28

### Added

- Typed command callbacks (`onBool`, `onInt32`, `onUInt32`, `onInt64`, `onUInt64`, `onString`)
  that confirm or decline each command.
- mbedTLS signing, ArduinoJson 7, input checks, host `hydronode.tech`.
