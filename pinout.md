# ESP32-S3 Moodlight Pinout

Recommended default pin mapping for first bring-up.  
Update this file if your PCB/wiring changes.

## Pinout Table

| Subsystem | Signal | ESP32-S3 GPIO | Connected Device Pin | Function |
|---|---|---:|---|---|
| WS2812B LED Strip | LED_DATA | GPIO10 | DIN (first LED) | RMT output for 20-LED chain |
| Status LED (optional) | STATUS_LED | GPIO48 | LED anode/cathode path | Device status and boot indication |
| User Button (optional) | USER_BTN | GPIO0 | Push button | Local mode cycle / safe reset trigger |

## Power and Ground

| Net | Connection | Notes |
|---|---|---|
| 5V | PSU 5V -> WS2812B VCC | Main LED power rail (current-limited in firmware) |
| 5V | PSU 5V -> ESP32-S3 5V/VBUS (if supported by board) | Board input rail (check board wiring limits) |
| GND | Common GND across PSU, ESP32-S3, WS2812B | Required for data signal reference and noise stability |

## Integration Notes

- Add a series resistor (around 220-470R) on `LED_DATA` near the first LED DIN.
- Add a bulk capacitor (for example 470-1000uF) across LED strip `5V/GND` near strip input.
- If LED data wiring is long/noisy, use a 3.3V->5V level shifter for `LED_DATA`.
- Keep LED data line away from switching power traces where possible.

## Firmware Signal Names

- `LED_DATA` -> `led_engine`
- `STATUS_LED`, `USER_BTN` -> `mode_manager` / diagnostics

