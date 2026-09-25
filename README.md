# Gas Leak Detection System

Intelligent LPG gas leak detection system on ESP32 with machine learning early detection, OpenAI alerts, MQTT, Telegram, and a local web dashboard.

## Features

- ML-based early leak detection (EMA gradient)
- OpenAI API for dynamic warning messages
- MQTT publishing for external dashboards
- Local web dashboard at ESP32 IP
- Telegram instant notifications
- OTA wireless firmware updates
- Watchdog timer for stability
- AP fallback mode if WiFi fails

## Hardware Required

| Component | Quantity |
|-----------|----------|
| ESP32 DevKit | 1 |
| MQ-2 Gas Sensor | 1 |
| MQ-7 Smoke Sensor | 1 |
| DHT22 Temperature/Humidity | 1 |
| Active Buzzer | 1 |
| LED Green / Yellow / Red | 3 |
| 220 ohm Resistors | 3 |
| Push Button | 1 |
| Relay Module | 1 |
| Solenoid Valve (optional) | 1 |

## Pinout

| Component | ESP32 Pin |
|-----------|-----------|
| Gas Sensor (Analog) | GPIO 34 |
| Smoke Sensor (Analog) | GPIO 35 |
| DHT22 Data | GPIO 4 |
| Solenoid | GPIO 26 |
| Relay | GPIO 27 |
| Buzzer | GPIO 14 |
| LED Green | GPIO 12 |
| LED Yellow | GPIO 13 |
| LED Red | GPIO 15 |
| Reset Button | GPIO 0 |
| Status LED | GPIO 2 |

## Setup

### 1. Clone the repository

    git clone https://github.com/nn0z/Gas-Detector.git
    cd Gas-Detector

### 2. Create secrets.h

    cp secrets.h.example gas_leak_detector/secrets.h

Edit `gas_leak_detector/secrets.h` and fill in your credentials:

    #define WIFI_SSID       "YOUR_WIFI_SSID"
    #define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
    #define OPENAI_API_KEY  "YOUR_OPENAI_API_KEY"
    #define TELEGRAM_TOKEN  "YOUR_TELEGRAM_BOT_TOKEN"
    #define TELEGRAM_CHAT   "YOUR_TELEGRAM_CHAT_ID"
    #define OTA_PASSWORD    "YOUR_OTA_PASSWORD"
    #define AP_PASSWORD     "YOUR_AP_PASSWORD"

OpenAI and Telegram are optional. Leave them empty if not used.

### 3. Install libraries

In Arduino IDE, go to Sketch > Include Library > Manage Libraries and install:

- PubSubClient
- ArduinoJson
- DHT sensor library
- Adafruit Unified Sensor

### 4. Install ESP32 board package

Go to File > Preferences and add this URL to "Additional Boards Manager URLs":

    https://espressif.github.io/arduino-esp32/package_esp32_index.json

Then go to Tools > Board > Boards Manager, search `esp32`, and install it.

### 5. Open and upload

1. Open `gas_leak_detector/gas_leak_detector.ino` in Arduino IDE
2. Select Tools > Board > ESP32 Dev Module
3. Select your COM port under Tools > Port
4. Click Upload

### 6. Open Serial Monitor

Set baud rate to 115200. Note the IP address shown.

### 7. Open web dashboard

Navigate to `http://<esp32-ip>` in your browser.

## MQTT Topics

| Topic | Description |
|-------|-------------|
| gas/leak/data | Sensor readings every 5s |
| gas/leak/alert | Critical alerts only |
| gas/leak/stats | Aggregated stats every 30s |

Subscribe via HiveMQ WebSocket Client (https://www.hivemq.com/mqtt-websocket-client/):

- Host: broker.hivemq.com
- Port: 8000

## Telegram Setup

1. Create a bot via @BotFather
2. Get your chat ID from @userinfobot
3. Fill them in `secrets.h`

## OTA Updates

After first upload, the board appears in Arduino IDE under:

Tools > Port > Network Ports > gas-detector

## Security

- Never commit `secrets.h`
- Regenerate any leaked API keys immediately
- Use `secrets.h.example` as a template only

---
## ⭐ Show Your Support
If you like this project, give it a **star** ⭐ on GitHub!
