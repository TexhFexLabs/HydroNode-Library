# HydroNode-Library

**HydroNode-Library** is a flexible, secure Arduino C++ library for communicating with the HydroNode IoT backend.
It is designed to make it easy for makers, hobbyists, and professionals to send sensor data and receive remote control events for actuators in the HydroNode ecosystem.

HydroNode is a next-generation IoT network created by TexhFexLabs, providing reliable and secure sensor data collection, cloud-based automation, and remote actuation via your own backend server.
**To use this library, you need a valid API key ("secret"), which will soon be available for purchase in the official HydroNode app for iOS (coming soon to the Apple App Store).**


## Features

- **Secure, authenticated data upload** to your HydroNode backend, using HMAC-SHA256 signing.
- **Callback/event system:** easily define actions for remote actuator commands (like `"pump"`, `"fan"`, `"led"`, etc.).
- **Simple, flexible API:** add any sensor, actuator, or custom event with a single line of code.
- **WiFi agnostic:** works with both WiFiManager (captive portal setup) and hard-coded credentials.
- **Fully compatible** with ESP8266, ESP32, and other WiFi-enabled Arduino boards.
- **Lightweight** and RAM-efficient (optimized for microcontrollers).
- **Open source under MIT License**.


## Table of Contents

- [Installation](#installation)
- [Getting Started](#getting-started)
  - [WiFiManager Setup (Recommended)](#wifimanager-setup-recommended)
  - [Manual WiFi Setup](#manual-wifi-setup)
- [Usage](#usage)
  - [Sending Sensor Data](#sending-sensor-data)
  - [Handling Backend Events (Callbacks)](#handling-backend-events-callbacks)
  - [Advanced: Custom Events and Types](#advanced-custom-events-and-types)
- [How the HydroNode Network Works](#how-the-hydronode-network-works)
- [Obtaining an API Key](#obtaining-an-api-key)
- [License](#license)


## Installation

1. **Download or clone this repository**
   (or search for "HydroNode-Library" in the Arduino Library Manager once available).

2. **Copy the `HydroNode-Library` folder** into your Arduino `libraries/` directory,
   or use PlatformIO's library manager.

3. **Install dependencies:**
The following libraries are required (install via Arduino Library Manager or PlatformIO):

- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- [ArduinoHttpClient](https://github.com/arduino-libraries/ArduinoHttpClient)
- [NTPClient](https://github.com/arduino-libraries/NTPClient)
- [densaugeo/ArduinoBase64](https://github.com/Densaugeo/base64_arduino)
- [Crypto by Rhys Weatherley](https://github.com/rweather/arduinolibs/tree/master/libraries/Crypto)

## Getting Started

### WiFiManager Setup (Recommended)

**This method allows end-users to configure WiFi credentials over a captive portal, no hardcoding required.**

```cpp
#include <Wire.h>
#include <WiFiManager.h>
#include <HydroNode.h>

#define RELAY_PIN 5
#define FAN_PIN   6

// Instantiate HydroNode with your unique sensor ID and secret key
HydroNode hydro(
    "your-sensor-id-here",
    "your-secret-key-here"
);

// Optional: actuator callbacks
void pumpCallback(int ms) {
    digitalWrite(RELAY_PIN, HIGH);
    delay(ms);
    digitalWrite(RELAY_PIN, LOW);
}

void fanCallback(bool on) {
    digitalWrite(FAN_PIN, on ? LOW : HIGH);
}

void setup() {
    Serial.begin(115200);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
    pinMode(FAN_PIN, OUTPUT);
    digitalWrite(FAN_PIN, HIGH);

    // WiFiManager: user-friendly AP setup for WiFi credentials
    WiFiManager wm;
    String apName = hydro.getApName(); // "HydroNode-Setup-XXXX"
    bool res = wm.autoConnect(apName.c_str());
    if (!res) {
        Serial.println("WiFi failed, restarting");
        delay(3000);
        ESP.restart();
    }

    hydro.begin();

    // Register event handlers
    hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));
    hydro.on("fan",  HydroNode::bindCallback<bool>(fanCallback));
}

void loop() {
    // Example: send temperature
    hydro.sendValue("temperature", 22.3);
    delay(10000);
}
```


### Manual WiFi Setup

For developers who prefer to set SSID/password directly in code:

```cpp
#include <ESP8266WiFi.h> // Or #include <WiFi.h> for ESP32
#include <HydroNode.h>

#define RELAY_PIN 5
#define FAN_PIN   6

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

HydroNode hydro("your-sensor-id-here", "your-secret-key-here");

void setup() {
    Serial.begin(115200);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);
    pinMode(FAN_PIN, OUTPUT);
    digitalWrite(FAN_PIN, HIGH);

    // Standard WiFi connect
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");

    hydro.begin();
    // ...register callbacks as above
}
```

## Usage

### Sending Sensor Data

Send any sensor value to your HydroNode backend with:

```cpp
hydro.sendValue("type", value);
```
Examples:
```cpp
hydro.sendValue("temperature", 21.5);
hydro.sendValue("humidity", 43.2);
hydro.sendValue("soil", analogRead(SOIL_PIN));
```

### Handling Backend Events (Callbacks)

When your backend sends commands in its JSON response, you can react with callbacks:
```cpp
void pumpCallback(int ms) {
    // e.g., turn relay on for ms milliseconds
}
hydro.on("pump", HydroNode::bindCallback<int>(pumpCallback));

// Supports any type: int, bool, String, etc.
void ledCallback(bool state) { /* ... */ }
hydro.on("led", HydroNode::bindCallback<bool>(ledCallback));
```

The library automatically calls your function when a matching key is found in the backend response.

### Advanced: Custom Events and Types

You can add as many handlers as you want for any backend key.

```cpp
void alarmCallback(String msg) {
    Serial.println(msg);
}
hydro.on("alarm", HydroNode::bindCallback<String>(alarmCallback));
```

### Advanced: set max response size

#### Example Response Size

If two commands are sent via the app, the backend responds with:
```json
{"pump":1200,"fan":1}
```

This JSON string is 23 bytes long (including braces, quotes, colons, and commas).
ArduinoJson also needs a bit of extra buffer for internal parsing, but for simple responses like this,
a buffer size of 128 bytes is always sufficient.

#### setJsonBufferSize(size_t size)

Allows you to configure the internal JSON buffer size used for parsing backend responses.
Useful if your backend sends larger or more complex JSON objects. Default is 128 bytes.

#### Tip
For 2–4 simple commands like the example above, 128 bytes is always enough.
If you send more keys or longer messages, increase the buffer (e.g. 256 bytes) to avoid parsing errors.


## How the HydroNode Network Works

The HydroNode ecosystem allows you to connect microcontroller-based sensors and actuators to a secure cloud backend, designed and operated by TexhFexLabs. Each device identifies itself with a unique sensor ID and authenticates all traffic using a secret key and HMAC-SHA256 signatures.

Your HydroNode backend URL is:

https://hydronode.texhfexlabs.de/api/webhook/sensor-value

* Data is only accepted from devices with valid credentials.
* To use this network, you must purchase an access key via the official HydroNode app (coming soon to the Apple App Store).

Typical Use Cases:

* Remote monitoring of temperature, humidity, air quality, soil moisture, and more.
* Secure control of relays, fans, pumps, LEDs, etc. via backend commands.
* Cloud-based rules/automation possible via backend logic.


## Obtaining an API Key

To protect the HydroNode cloud network, all access requires a unique secret key.

* These keys will be available for purchase soon via the official HydroNode iOS app.
* For development/testing, contact contact@knollfelix.de for a trial key.


## License

This project is licensed under the MIT License – see the LICENSE file for details.

HydroNode-Library is developed and maintained by TexhFexLabs.
For support, feature requests, or business inquiries, please contact contact@knollfelix.de.
