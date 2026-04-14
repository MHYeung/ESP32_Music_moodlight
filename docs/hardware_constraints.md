# Hardware Constraints and Safety Defaults

This project assumes an ESP32-S3 SuperMini controls a 10-pixel WS2812B chain from a dedicated 5V/2A rail.

## Power Constraints

- LED rail: 5V, external supply rated 2A.
- Worst-case WS2812B white current can exceed the supply budget, so firmware must enforce a global brightness cap.
- Start with `brightness_cap <= 128` (out of 255) during bring-up and raise only after thermal/current validation.
- Use shared ground between PSU, ESP32-S3 board, and LED strip.

## Signal Integrity

- Add a series resistor (220R to 470R) between ESP32-S3 data GPIO and the first WS2812B DIN.
- Place a bulk capacitor (470uF to 1000uF) across LED strip 5V/GND near strip input.
- Keep LED data line short; if cable length/noise is high, use a 3.3V to 5V level shifter.
- Keep LED data and power return paths short to reduce color glitches.

## Boot and Runtime Safety Defaults

- On boot, start with safe mode settings:
  - low brightness cap
  - static low-power color
  - no aggressive animation spikes
- Apply persisted user settings only after peripherals initialize successfully.
- When brownout/recovery is detected, force a lower brightness safety profile.

## Validation Checklist

- Verify strip operation at low brightness before enabling dynamic effects.
- Confirm no random color glitches under Wi-Fi + animation load.
- Confirm SoftAP + web control remains stable for at least 30 minutes.
