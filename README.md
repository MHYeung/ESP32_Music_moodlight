# music_moodlight

ESP32-S3 firmware for a Wi-Fi–controlled mood light: WS2812 strip effects, microphone-reactive mode, settings persisted in NVS, and a small web UI served from LittleFS.

## Documentation

| Document | Description |
|----------|-------------|
| [Pinout](pinout.md) | GPIO mapping, power, and wiring notes |
| [Hardware constraints](docs/hardware_constraints.md) | Power budget, signal integrity, and safe defaults |
| [Bring-up validation](docs/bringup_validation.md) | Staged checks, tuning targets, and stress testing |

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html) (ESP32-S3 target)
- USB cable for flash and serial monitor

## Build and flash

From the project root (with ESP-IDF environment loaded):

```text
idf.py set-target esp32s3
idf.py build flash monitor
```

## Configuration

Wi-Fi station SSID and password are set in `main/main.c`. Adjust them for your network before deploying.

After a successful connection, the HTTP server listens on port 80. Open the device IP in a browser to use the control UI.
