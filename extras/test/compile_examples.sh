#!/usr/bin/env bash
# Compiles every example for the boards it is written for, with arduino-cli in a private
# sketchbook (your own Arduino libraries are not touched).
#
#   bash extras/test/compile_examples.sh            all boards
#   bash extras/test/compile_examples.sh esp32      one board family
#
# Needs arduino-cli with the cores esp32:esp32, esp8266:esp8266 and arduino:samd installed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$(cd "$HERE/../.." && pwd)"
WORK="${HN_ARDUINO_DIR:-/tmp/hydronode-arduino}"
export ARDUINO_DIRECTORIES_USER="$WORK/user"
ONLY="${1:-}"

mkdir -p "$ARDUINO_DIRECTORIES_USER/libraries"
ln -sfn "$LIB" "$ARDUINO_DIRECTORIES_USER/libraries/HydroNode-Library"
for dep in ArduinoJson ArduinoHttpClient WiFiManager TinyGSM ESP_SSLClient Ethernet WiFiNINA; do
  [ -d "$ARDUINO_DIRECTORIES_USER/libraries/$dep" ] || arduino-cli lib install "$dep" >/dev/null
done

# board | examples
MATRIX=(
  "esp32:esp32:esp32|QuickStart ExternalWiFi WiFiManagerSetup ActuatorControl PowerThresholds Transport_WiFi Ethernet_ESP32 Ethernet_W5500 Cellular_SendNow Cellular_ModemTLS WiFi_Fallback_LTE Board_AnyClient_Template Cellular_CollectHourly Buffer_OfflineSafe Buffer_DeepSleep"
  "esp32:esp32:esp32c3|QuickStart Transport_WiFi"
  "esp8266:esp8266:nodemcuv2|QuickStart ActuatorControl PowerThresholds Transport_WiFi Board_AnyClient_Template Buffer_OfflineSafe Buffer_DeepSleep"
  "arduino:samd:mkrwifi1010|Board_MKR_WiFi1010 Board_AnyClient_Template"
)

failed=0
for entry in "${MATRIX[@]}"; do
  board="${entry%%|*}"
  [ -n "$ONLY" ] && [[ "$board" != *"$ONLY"* ]] && continue
  for example in ${entry#*|}; do
    [ -d "$LIB/examples/$example" ] || continue
    if out=$(arduino-cli compile --clean -b "$board" "$LIB/examples/$example" --warnings all 2>&1); then
      size=$(printf '%s\n' "$out" | grep -Eo 'Sketch uses [0-9]+ bytes|IROM +[0-9]+' | head -1)
      warn=$(printf '%s\n' "$out" | grep -E "HydroNode-Library/(src|examples)|examples/$example/.*warning" | grep -c "warning" || true)
      printf 'ok    %-28s %-32s %s%s\n' "$board" "$example" "$size" "$( [ "$warn" != 0 ] && echo "  ($warn warnings)")"
      [ "$warn" != 0 ] && printf '%s\n' "$out" | grep -E "HydroNode-Library/(src|examples).*warning|examples/$example/.*warning"
    else
      failed=$((failed + 1))
      printf 'FAIL  %-28s %s\n' "$board" "$example"
      printf '%s\n' "$out" | grep -E "error|Error" | head -15
    fi
  done
done
exit $failed
