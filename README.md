# diy-homestudio (Core2)

This project targets the **M5Stack Core2 (ESP32)** via PlatformIO + Arduino.

## Wi‑Fi setup options

### Option A: Captive portal (no hard-coded Wi‑Fi)
1. Flash the firmware.
2. If the Core2 can’t connect, it starts a setup AP named `Core2-Setup` (the device UI will say “SETUP PORTAL”).
3. Connect your phone/laptop to **that** Wi‑Fi network (not your home Wi‑Fi).
4. Open `http://192.168.4.1` to enter Wi‑Fi credentials.

To reconfigure later:
- Tap the `WiFi` tab and press `Portal` (or press `BtnA`) to start the portal.
- Press `Forget WiFi` to erase saved credentials and start the portal fresh.
- To password-protect the setup AP, set `PORTAL_AP_PASS` in `include/secrets.h` (8..63 chars).

### Option B: Hard-code Wi‑Fi (recommended for quick bring-up)
1. Copy `include/secrets.example.h` to `include/secrets.h`
2. Edit `include/secrets.h` and set `WIFI_SSID` / `WIFI_PASS`
3. Flash the firmware.

## Build / Upload
- `pio run`
- `pio run -t upload`
- `pio device monitor`

## Upload troubleshooting (Linux)

### `Permission denied: '/dev/ttyACM0'`
The serial device is typically owned by group `dialout`. Add your user to that group:
- `sudo usermod -aG dialout $USER`
- Log out/in (or reboot), then confirm: `groups | rg dialout` (or `groups | grep dialout`)

Then replug the Core2 and upload explicitly:
- `pio run -t upload --upload-port /dev/ttyACM0`

### “Port is busy”
- Close any serial monitor (`pio device monitor`, `screen`, Arduino IDE, etc.)
- On many Linux distros, `ModemManager` can grab ACM devices; try:
  - `sudo systemctl stop ModemManager`
  - (optional) `sudo systemctl disable ModemManager`

### udev rules warning
If PlatformIO warns about `99-platformio-udev.rules`, follow:
`https://docs.platformio.org/en/latest/core/installation/udev-rules.html`

## Notes
- ESP32 supports **2.4 GHz** Wi‑Fi only (not 5 GHz).
- `192.168.4.1` only works when your phone/laptop is connected to the Core2 setup AP (`Core2-Setup`).

## UI extras
- Footer shows a scrolling weather line (Open‑Meteo) plus a battery icon.
- Weather defaults to Copenhagen; override in `include/secrets.h` with `WEATHER_LATITUDE` / `WEATHER_LONGITUDE` / `WEATHER_LABEL`.

## Battery tips
- The screen backlight is the biggest drain; the firmware auto-dims after inactivity.
- Wi‑Fi modem sleep is enabled after connecting to reduce power.
# weather-station
