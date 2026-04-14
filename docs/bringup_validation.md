# Bring-up and Tuning Notes

## Validation Status

- Static project integration complete (component structure, tasks, API routes, NVS/LittleFS split).
- Local IDE lint check: no diagnostics.
- Build command could not be executed on this machine because `idf.py` is not available in PATH.

## Stage-Based Bring-up Procedure

1. Power-only check
   - Boot with default brightness cap (`<= 128`).
   - Confirm no brownout resets and no LED flicker at idle.
2. LED path check
   - Verify static single color from web UI color wheel.
   - Verify palette breathing mode runs and loops color picks from selected palette.
3. SoftAP path check
   - Connect to device AP and open control page.
   - Confirm control latency remains smooth while animation is active.
4. Persistence check
   - Change mode/color/brightness from UI.
   - Reboot and confirm values restore from NVS.
5. Stress check
   - Keep Wi-Fi + palette breathing active for at least 30 minutes.
   - Watch for task starvation, WDT resets, and unstable colors.

## Initial Tuning Targets

- `brightness_cap`: 128 (raise cautiously after thermal/current checks)
- Breathing `period_ms`: 2600
- Breathing palette default: `sunset`
- LED count: 20 on `GPIO10`

## Next Hardware-in-loop Tweaks

- Add persisted SoftAP SSID/password settings (optional setup mode).
- Add brightness slider in UI with explicit power budget warning.
- Add palette preview chips in the web UI list for better usability.
