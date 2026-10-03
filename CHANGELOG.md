# Changelog

All notable changes to HydroNode-Library are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the library uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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
