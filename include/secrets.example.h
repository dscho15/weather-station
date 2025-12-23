#pragma once

// Copy this file to `include/secrets.h` (it is gitignored) and fill in your Wi-Fi.
// Note: ESP32 only supports 2.4 GHz Wi-Fi.

#define WIFI_SSID "1b588c-2.4GHz"
#define WIFI_PASS "CP2306NA3A2"

// Optional: Weather ticker configuration (defaults to Copenhagen, Denmark).
// Find your lat/lon: e.g. Google Maps -> drop a pin.
#define WEATHER_LABEL "DK"
#define WEATHER_LATITUDE 55.6761f
#define WEATHER_LONGITUDE 12.5683f

// Optional: password for the Core2 setup AP ("Core2-Setup"). Must be 8..63 chars.
// Leave empty to keep the setup AP open.
#define PORTAL_AP_PASS "Edw52Lmao"
