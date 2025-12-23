#include <Arduino.h>
#include <HTTPClient.h>
#include <M5Core2.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

#ifndef WEATHER_LATITUDE
#define WEATHER_LATITUDE 55.6761f  // Copenhagen
#endif

#ifndef WEATHER_LONGITUDE
#define WEATHER_LONGITUDE 12.5683f
#endif

#ifndef WEATHER_LABEL
#define WEATHER_LABEL "DK"
#endif

// Optional: password for the Core2 setup AP ("Core2-Setup").
// Leave empty to keep the setup AP open.
// Note: WPA2 AP passwords must be 8..63 chars.
#ifndef PORTAL_AP_PASS
#define PORTAL_AP_PASS ""
#endif

static constexpr const char* kHostname = "core2-ha";
static constexpr const char* kPortalApName = "Core2-Setup";
static constexpr uint32_t kConnectTimeoutMs = 30000;
static constexpr uint32_t kPortalTimeoutMs = 180000;

static const char* portalPasswordOrNull();

static uint16_t kColorBg = 0;
static uint16_t kColorPanel = 0;
static uint16_t kColorText = 0;
static uint16_t kColorMuted = 0;
static uint16_t kColorAccent = 0;
static uint16_t kColorGood = 0;
static uint16_t kColorWarn = 0;
static uint16_t kColorBad = 0;

static constexpr int16_t kStatusPillH = 28;
static constexpr int16_t kWiFiPillH = 24;

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;

  constexpr Rect() = default;
  constexpr Rect(int16_t x_, int16_t y_, int16_t w_, int16_t h_) : x(x_), y(y_), w(w_), h(h_) {}

  bool contains(int16_t px, int16_t py) const {
    return px >= x && py >= y && px < (x + w) && py < (y + h);
  }
};

enum class View : uint8_t { Status = 0, WiFi = 1, About = 2 };
enum class WifiState : uint8_t { Connecting = 0, Connected = 1, Portal = 2, Error = 3 };

static View gView = View::Status;
static WifiState gWifiState = WifiState::Connecting;
static WiFiManager gWiFiManager;
static String gConnectTarget = "";
static bool gConnectUsingSecrets = false;
static wl_status_t gLastStaStatus = WL_DISCONNECTED;

static bool gPortalActive = false;
static bool gUiDirty = true;
static String gLastError;
static uint32_t gWifiDeadlineMs = 0;
static uint32_t gPortalDeadlineMs = 0;
static uint32_t gUiNextRefreshMs = 0;
static View gLastDrawnView = View::Status;
static WifiState gLastDrawnWifiState = WifiState::Error;
static String gLastDrawnSSID;
static String gLastDrawnIP;
static int32_t gLastDrawnRSSI = INT32_MIN;
static String gLastDrawnError;
static int8_t gLastDrawnBatteryPct = -1;
static bool gLastDrawnCharging = false;
static uint32_t gBatteryNextSampleMs = 0;
static uint8_t gBatteryPctCached = 0;
static bool gBatteryChargingCached = false;
static bool gBatteryCachedValid = false;

static constexpr int16_t kTopBarH = 34;
static constexpr int16_t kFooterH = 24;
static Rect gTabStatus;
static Rect gTabWifi;
static Rect gTabAbout;
static Rect gBtnPortal;
static Rect gBtnRetry;
static Rect gBtnForget;
static Rect gFooterRect;

static TFT_eSprite* gTickerSprite = nullptr;
static int16_t gTickerW = 0;
static int16_t gTickerH = 0;

static Button* gHitTabStatus = nullptr;
static Button* gHitTabWiFi = nullptr;
static Button* gHitTabAbout = nullptr;
static Button* gHitPortal = nullptr;
static Button* gHitRetry = nullptr;
static Button* gHitForget = nullptr;

static Gesture gSwipeLeft("swipe left", 90, DIR_LEFT, 35);
static Gesture gSwipeRight("swipe right", 90, DIR_RIGHT, 35);

static constexpr uint8_t kBrightnessActive = 60;
static constexpr uint8_t kBrightnessDim = 12;
static constexpr uint32_t kDimAfterMs = 20000;
static uint32_t gLastInteractionMs = 0;
static uint8_t gCurrentBrightness = 255;

static portMUX_TYPE gWeatherMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t gWeatherTask = nullptr;
static volatile bool gWeatherTaskRunning = false;
static uint32_t gWeatherNextFetchMs = 0;
static uint32_t gFooterNextTickMs = 0;
static int16_t gWeatherScrollPx = 0;
static char gWeatherText[160] = "Weather: (waiting for WiFi)";
static bool gWeatherHasData = false;

static void uiMarkDirty() { gUiDirty = true; }

static void inputInit() {
  auto reset = [](Button*& btn, const Rect& r, const char* name) {
    if (btn) delete btn;
    btn = new Button(r.x, r.y, r.w, r.h, false, name);
  };

  reset(gHitTabStatus, gTabStatus, "tabStatus");
  reset(gHitTabWiFi, gTabWifi, "tabWiFi");
  reset(gHitTabAbout, gTabAbout, "tabAbout");

  reset(gHitPortal, gBtnPortal, "btnPortal");
  reset(gHitRetry, gBtnRetry, "btnRetry");
  reset(gHitForget, gBtnForget, "btnForget");
}

static void uiInit() {
  const int16_t w = M5.Lcd.width();
  const int16_t h = M5.Lcd.height();

  kColorBg = M5.Lcd.color565(10, 10, 16);
  kColorPanel = M5.Lcd.color565(18, 18, 28);
  kColorText = M5.Lcd.color565(240, 240, 245);
  kColorMuted = M5.Lcd.color565(150, 155, 170);
  kColorAccent = M5.Lcd.color565(0, 190, 210);
  kColorGood = M5.Lcd.color565(40, 200, 120);
  kColorWarn = M5.Lcd.color565(250, 180, 50);
  kColorBad = M5.Lcd.color565(250, 80, 80);

  M5.Lcd.setTextFont(2);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(kColorText, kColorBg);

  const int16_t tabW = w / 3;
  gTabStatus = Rect{0, 0, tabW, kTopBarH};
  gTabWifi = Rect{tabW, 0, tabW, kTopBarH};
  gTabAbout =
      Rect{static_cast<int16_t>(tabW * 2), 0, static_cast<int16_t>(w - tabW * 2), kTopBarH};

  gFooterRect = Rect{0, static_cast<int16_t>(h - kFooterH), w, kFooterH};

  const int16_t btnGap = 8;
  const int16_t btnW = (w - 24 - btnGap * 2) / 3;
  const int16_t btnH = 34;
  const int16_t btnY = static_cast<int16_t>(h - kFooterH - btnH - 8);

  gBtnPortal = Rect{12, btnY, btnW, btnH};
  gBtnRetry = Rect{static_cast<int16_t>(12 + btnW + btnGap), btnY, btnW, btnH};
  gBtnForget = Rect{static_cast<int16_t>(12 + (btnW + btnGap) * 2), btnY, btnW, btnH};

  const int16_t padX = 8;
  const int16_t batX = static_cast<int16_t>(w - padX - 28 - 3);  // battery + nub
  const int16_t textMaxW = static_cast<int16_t>(batX - padX - 8);
  gTickerW = textMaxW;
  gTickerH = static_cast<int16_t>(gFooterRect.h - 1);
  if (!gTickerSprite) gTickerSprite = new TFT_eSprite(&M5.Lcd);
  gTickerSprite->setColorDepth(16);
  gTickerSprite->createSprite(gTickerW, gTickerH);

  inputInit();
}

static void drawTab(const Rect& r, const char* label, bool active) {
  const uint16_t bg = active ? kColorAccent : kColorPanel;
  const uint16_t fg = active ? kColorBg : kColorMuted;
  M5.Lcd.fillRect(r.x, r.y, r.w, r.h, bg);
  M5.Lcd.setTextColor(fg, bg);
  M5.Lcd.drawCentreString(label, r.x + (r.w / 2), r.y + 9, 2);
  M5.Lcd.setTextColor(kColorText, kColorBg);
}

static void drawTopBar() {
  drawTab(gTabStatus, "Status", gView == View::Status);
  drawTab(gTabWifi, "WiFi", gView == View::WiFi);
  drawTab(gTabAbout, "About", gView == View::About);
}

static void drawPill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg, const char* label) {
  M5.Lcd.fillRoundRect(x, y, w, h, 12, bg);
  M5.Lcd.setTextColor(kColorBg, bg);
  const int16_t textY = static_cast<int16_t>(y + (h - 16) / 2);
  M5.Lcd.drawCentreString(label, x + (w / 2), textY, 2);
  M5.Lcd.setTextColor(kColorText, kColorBg);
}

static void drawButton(const Rect& r, uint16_t bg, const char* label, bool enabled = true) {
  const uint16_t fill = enabled ? bg : kColorPanel;
  const uint16_t text = enabled ? kColorBg : kColorMuted;
  M5.Lcd.fillRoundRect(r.x, r.y, r.w, r.h, 10, fill);
  M5.Lcd.drawRoundRect(r.x, r.y, r.w, r.h, 10, enabled ? bg : kColorMuted);
  M5.Lcd.setTextColor(text, fill);
  const int16_t textY = static_cast<int16_t>(r.y + (r.h - 16) / 2);
  M5.Lcd.drawCentreString(label, r.x + (r.w / 2), textY, 2);
  M5.Lcd.setTextColor(kColorText, kColorBg);
}

static constexpr int16_t kInfoLabelX = 12;
static constexpr int16_t kInfoValueX = 108;
static constexpr int16_t kInfoRowH = 24;

static void drawInfoRow(int16_t y, const char* label, const String& value) {
  M5.Lcd.setTextColor(kColorMuted, kColorBg);
  M5.Lcd.drawString(label, kInfoLabelX, y, 2);
  M5.Lcd.setTextColor(kColorText, kColorBg);
  M5.Lcd.drawString(value, kInfoValueX, y, 2);
}

static const char* wifiStateLabel() {
  switch (gWifiState) {
    case WifiState::Connecting:
      return "CONNECTING";
    case WifiState::Connected:
      return "CONNECTED";
    case WifiState::Portal:
      return "SETUP PORTAL";
    case WifiState::Error:
      return "ERROR";
  }
  return "";
}

static uint16_t wifiStateColor() {
  switch (gWifiState) {
    case WifiState::Connecting:
      return kColorWarn;
    case WifiState::Connected:
      return kColorGood;
    case WifiState::Portal:
      return kColorAccent;
    case WifiState::Error:
      return kColorBad;
  }
  return kColorMuted;
}

static const char* staStatusToString(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN DONE";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "AUTH FAIL";
    case WL_CONNECTION_LOST:
      return "LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

static void drawStatusView() {
  const int16_t w = M5.Lcd.width();
  const int16_t h = M5.Lcd.height();
  int16_t y = kTopBarH + 14;

  drawPill(12, y, w - 24, kStatusPillH, wifiStateColor(), wifiStateLabel());
  y += kStatusPillH + 12;

  drawInfoRow(y, "Host", kHostname);
  y += kInfoRowH;

  if (WiFi.status() == WL_CONNECTED) {
    drawInfoRow(y, "SSID", WiFi.SSID());
    y += kInfoRowH;
    drawInfoRow(y, "IP", WiFi.localIP().toString());
    y += kInfoRowH;
    drawInfoRow(y, "RSSI", String(WiFi.RSSI()) + " dBm");
    y += kInfoRowH;
  } else if (gWifiState == WifiState::Portal) {
    M5.Lcd.setTextColor(kColorMuted, kColorBg);
    M5.Lcd.drawString("Setup:", 12, y, 2);
    y += kInfoRowH;
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(String("1) Join ") + kPortalApName, 12, y, 2);
    y += kInfoRowH;
    M5.Lcd.drawString("2) Open http://192.168.4.1", 12, y, 2);
    y += kInfoRowH;
  } else if (gWifiState == WifiState::Error) {
    M5.Lcd.setTextColor(kColorMuted, kColorBg);
    M5.Lcd.drawString("Error:", 12, y, 2);
    y += kInfoRowH;
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(gLastError, 12, y, 2);
    y += kInfoRowH;
  } else {
    M5.Lcd.setTextColor(kColorMuted, kColorBg);
    const String target = gConnectUsingSecrets ? (String("Connecting: ") + gConnectTarget)
                                               : String("Connecting: (saved)");
    M5.Lcd.drawString(target, 12, y, 2);
    y += kInfoRowH;
    M5.Lcd.drawString(String("State: ") + staStatusToString(WiFi.status()), 12, y, 2);
    y += kInfoRowH;
    M5.Lcd.drawString("Tip: WiFi tab (or BtnA) for setup portal.", 12, y, 2);
    y += kInfoRowH;
  }

  (void)h;
}

static void drawWiFiView() {
  const int16_t w = M5.Lcd.width();
  int16_t y = kTopBarH + 14;

  M5.Lcd.setTextColor(kColorText, kColorBg);
  M5.Lcd.drawString("Wi-Fi", 12, y, 4);
  y += 34;

  drawPill(12, y, w - 24, kWiFiPillH, wifiStateColor(), wifiStateLabel());
  y += kWiFiPillH + 12;

  if (WiFi.status() == WL_CONNECTED) {
    drawInfoRow(y, "SSID", WiFi.SSID());
    y += kInfoRowH;
    drawInfoRow(y, "IP", WiFi.localIP().toString());
  } else if (gWifiState == WifiState::Connecting) {
    const String target = gConnectUsingSecrets ? gConnectTarget : "(saved)";
    drawInfoRow(y, "Try", target);
    y += kInfoRowH;
    drawInfoRow(y, "State", staStatusToString(WiFi.status()));
  } else if (gWifiState == WifiState::Portal) {
    M5.Lcd.setTextColor(kColorMuted, kColorBg);
    M5.Lcd.drawString("Setup portal is running.", 12, y, 2);
    M5.Lcd.drawString(String("Join AP: ") + kPortalApName, 12, y + 18, 2);
    if (portalPasswordOrNull() != nullptr) M5.Lcd.drawString("AP password: set", 12, y + 36, 2);
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString("http://192.168.4.1", 12, y + 58, 4);
    M5.Lcd.setTextColor(kColorMuted, kColorBg);
  } else if (gWifiState == WifiState::Error) {
    M5.Lcd.setTextColor(kColorMuted, kColorBg);
    M5.Lcd.drawString("WiFi error", 12, y, 2);
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(gLastError, 12, y + 18, 2);
  } else {
    M5.Lcd.setTextColor(kColorMuted, kColorBg);
    M5.Lcd.drawString("Connecting...", 12, y, 2);
  }

  drawButton(gBtnPortal, kColorAccent, "Portal");
  drawButton(gBtnRetry, kColorGood, "Retry");
  drawButton(gBtnForget, kColorBad, "Forget");
}

static void drawAboutView() {
  int16_t y = kTopBarH + 14;

  M5.Lcd.setTextColor(kColorText, kColorBg);
  M5.Lcd.drawString("Core2 Home Automation", 12, y, 4);
  y += 40;

  M5.Lcd.setTextColor(kColorMuted, kColorBg);
  M5.Lcd.drawString("Wi-Fi setup portal", 12, y, 2);
  y += 20;
  M5.Lcd.drawString(String("AP: ") + kPortalApName, 12, y, 2);
  y += 20;
  M5.Lcd.drawString("URL: http://192.168.4.1", 12, y, 2);
  y += 30;

  M5.Lcd.drawString("Tip: press BtnA for portal.", 12, y, 2);
  y += 20;
  M5.Lcd.drawString(String("Build: ") + __DATE__ + " " + __TIME__, 12, y, 2);
}

static uint8_t clampU8(int v, int lo, int hi) {
  if (v < lo) return static_cast<uint8_t>(lo);
  if (v > hi) return static_cast<uint8_t>(hi);
  return static_cast<uint8_t>(v);
}

static uint8_t getBatteryPercent() {
  const float level = M5.Axp.GetBatteryLevel();  // 0..100
  const int pct = static_cast<int>(level + 0.5f);
  return clampU8(pct, 0, 100);
}

static void batterySampleTick() {
  const uint32_t now = millis();
  if (gBatteryNextSampleMs != 0 && now < gBatteryNextSampleMs) return;
  gBatteryNextSampleMs = now + 30000;

  gBatteryPctCached = getBatteryPercent();
  gBatteryChargingCached = M5.Axp.isCharging();
  gBatteryCachedValid = true;
}

static void drawBatteryIcon(int16_t x, int16_t y, uint8_t pct, bool charging) {
  const int16_t w = 28;
  const int16_t h = 12;
  const int16_t nubW = 3;
  const int16_t nubH = 6;
  const int16_t nubX = x + w;
  const int16_t nubY = y + (h - nubH) / 2;

  const uint16_t outline = kColorMuted;
  const uint16_t fillBg = kColorPanel;
  const uint16_t fillFg = (pct <= 15) ? kColorBad : (pct <= 35 ? kColorWarn : kColorGood);

  M5.Lcd.fillRect(x, y, w, h, fillBg);
  M5.Lcd.drawRect(x, y, w, h, outline);
  M5.Lcd.fillRect(nubX, nubY, nubW, nubH, outline);

  const int16_t innerX = x + 2;
  const int16_t innerY = y + 2;
  const int16_t innerW = w - 4;
  const int16_t innerH = h - 4;
  const int16_t filledW = static_cast<int16_t>((innerW * pct) / 100);
  M5.Lcd.fillRect(innerX, innerY, innerW, innerH, fillBg);
  if (filledW > 0) M5.Lcd.fillRect(innerX, innerY, filledW, innerH, fillFg);

  if (charging) {
    // Simple bolt overlay.
    const int16_t bx = x + 12;
    const int16_t by = y + 2;
    M5.Lcd.drawLine(bx + 2, by, bx - 1, by + 5, kColorText);
    M5.Lcd.drawLine(bx - 1, by + 5, bx + 2, by + 5, kColorText);
    M5.Lcd.drawLine(bx + 2, by + 5, bx - 1, by + 10, kColorText);
  }
}

static void uiDrawFooterWeatherOnly(const char* weatherText) {
  const int16_t w = M5.Lcd.width();

  const int16_t padX = 8;
  const int16_t batX = static_cast<int16_t>(w - padX - 28 - 3);  // battery + nub
  const int16_t textX0 = padX;
  const int16_t textMaxW = static_cast<int16_t>(batX - padX - 8);

  if (!gTickerSprite || gTickerW != textMaxW) {
    // Fallback: draw directly without clipping.
    const int16_t textY = static_cast<int16_t>(gFooterRect.y + 5);
    M5.Lcd.setTextColor(kColorText, kColorPanel);
    M5.Lcd.drawString(weatherText, textX0, textY, 2);
    return;
  }

  gTickerSprite->fillSprite(kColorPanel);
  gTickerSprite->setTextColor(kColorText, kColorPanel);
  const int16_t textY = static_cast<int16_t>((gTickerH - 16) / 2);
  const int16_t textW = gTickerSprite->textWidth(weatherText, 2);

  if (textW <= textMaxW) {
    gTickerSprite->drawString(weatherText, 0, textY, 2);
    gWeatherScrollPx = 0;
  } else {
    const int16_t gap = 24;
    const int16_t total = textW + gap;
    const int16_t scroll = gWeatherScrollPx % total;
    gTickerSprite->drawString(weatherText, -scroll, textY, 2);
    gTickerSprite->drawString(weatherText, -scroll + total, textY, 2);
  }

  gTickerSprite->pushSprite(textX0, gFooterRect.y + 1);
}

static void uiDrawFooterFull(bool forceFull) {
  const int16_t w = M5.Lcd.width();
  const int16_t h = M5.Lcd.height();

  char weatherLocal[sizeof(gWeatherText)];
  portENTER_CRITICAL(&gWeatherMux);
  strncpy(weatherLocal, gWeatherText, sizeof(weatherLocal));
  weatherLocal[sizeof(weatherLocal) - 1] = '\0';
  portEXIT_CRITICAL(&gWeatherMux);

  const uint8_t batPct = gBatteryCachedValid ? gBatteryPctCached : 0;
  const bool charging = gBatteryCachedValid ? gBatteryChargingCached : false;

  const bool batChanged = (static_cast<int8_t>(batPct) != gLastDrawnBatteryPct) ||
                          (charging != gLastDrawnCharging);
  if (!forceFull && !batChanged && strlen(weatherLocal) == 0) return;

  M5.Lcd.fillRect(gFooterRect.x, gFooterRect.y, gFooterRect.w, gFooterRect.h, kColorPanel);
  M5.Lcd.drawFastHLine(gFooterRect.x, gFooterRect.y, gFooterRect.w, kColorMuted);

  const int16_t padX = 8;
  const int16_t batX = static_cast<int16_t>(w - padX - 28 - 3);  // battery + nub
  const int16_t batY = static_cast<int16_t>(gFooterRect.y + (gFooterRect.h - 12) / 2);
  drawBatteryIcon(batX, batY, batPct, charging);

  uiDrawFooterWeatherOnly(weatherLocal);

  gLastDrawnBatteryPct = static_cast<int8_t>(batPct);
  gLastDrawnCharging = charging;
}

static void uiDrawFull() {
  M5.Lcd.fillScreen(kColorBg);
  drawTopBar();
  switch (gView) {
    case View::Status:
      drawStatusView();
      break;
    case View::WiFi:
      drawWiFiView();
      break;
    case View::About:
      drawAboutView();
      break;
  }
  uiDrawFooterFull(true);

  gLastDrawnView = gView;
  gLastDrawnWifiState = gWifiState;
  gLastDrawnSSID = WiFi.SSID();
  gLastDrawnIP = WiFi.localIP().toString();
  gLastDrawnRSSI = WiFi.RSSI();
  gLastDrawnError = gLastError;
}

static void clearLine(int16_t x, int16_t y, int16_t w) {
  M5.Lcd.fillRect(x, y, w, 18, kColorBg);
}

static void uiUpdateDynamicStatus() {
  const int16_t w = M5.Lcd.width();
  const int16_t pillY = kTopBarH + 14;

  if (gWifiState != gLastDrawnWifiState) {
    drawPill(12, pillY, w - 24, kStatusPillH, wifiStateColor(), wifiStateLabel());
    gLastDrawnWifiState = gWifiState;
  }

  if (WiFi.status() != WL_CONNECTED) return;

  const int16_t y0 = pillY + kStatusPillH + 12;  // first row start
  const int16_t valueX = kInfoValueX;
  const int16_t valueW = w - kInfoValueX - 12;

  const String ssid = WiFi.SSID();
  const String ip = WiFi.localIP().toString();
  const int32_t rssi = WiFi.RSSI();

  if (ssid != gLastDrawnSSID) {
    clearLine(valueX, y0 + kInfoRowH, valueW);
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(ssid, valueX, y0 + kInfoRowH, 2);
    gLastDrawnSSID = ssid;
  }

  if (ip != gLastDrawnIP) {
    clearLine(valueX, y0 + kInfoRowH * 2, valueW);
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(ip, valueX, y0 + kInfoRowH * 2, 2);
    gLastDrawnIP = ip;
  }

  if (rssi != gLastDrawnRSSI) {
    clearLine(valueX, y0 + kInfoRowH * 3, valueW);
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(String(rssi) + " dBm", valueX, y0 + kInfoRowH * 3, 2);
    gLastDrawnRSSI = rssi;
  }
}

static void uiUpdateDynamicWiFi() {
  const int16_t w = M5.Lcd.width();
  const int16_t titleY = kTopBarH + 14;
  const int16_t pillY = titleY + 34;

  if (gWifiState != gLastDrawnWifiState) {
    drawPill(12, pillY, w - 24, kWiFiPillH, wifiStateColor(), wifiStateLabel());
    gLastDrawnWifiState = gWifiState;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (gWifiState == WifiState::Error && gLastError != gLastDrawnError) {
      // Simplest: redraw the whole view when error text changes.
      uiMarkDirty();
    }
    return;
  }

  const int16_t y0 = pillY + kWiFiPillH + 12;
  const int16_t valueX = kInfoValueX;
  const int16_t valueW = w - kInfoValueX - 12;

  const String ssid = WiFi.SSID();
  const String ip = WiFi.localIP().toString();

  if (ssid != gLastDrawnSSID) {
    clearLine(valueX, y0, valueW);
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(ssid, valueX, y0, 2);
    gLastDrawnSSID = ssid;
  }

  if (ip != gLastDrawnIP) {
    clearLine(valueX, y0 + kInfoRowH, valueW);
    M5.Lcd.setTextColor(kColorText, kColorBg);
    M5.Lcd.drawString(ip, valueX, y0 + kInfoRowH, 2);
    gLastDrawnIP = ip;
  }
}

static void uiUpdateDynamic() {
  if (gView != gLastDrawnView) {
    uiMarkDirty();
    return;
  }

  switch (gView) {
    case View::Status:
      uiUpdateDynamicStatus();
      break;
    case View::WiFi:
      uiUpdateDynamicWiFi();
      break;
    case View::About:
      break;
  }
}

static void noteInteraction() { gLastInteractionMs = millis(); }

static void powerTick() {
  const uint32_t now = millis();
  const bool shouldDim = (now - gLastInteractionMs) > kDimAfterMs;
  const uint8_t target = shouldDim ? kBrightnessDim : kBrightnessActive;
  if (target != gCurrentBrightness) {
    M5.Lcd.setBrightness(target);
    gCurrentBrightness = target;
  }
}

static const char* wmoCodeToShortText(int code) {
  // WMO weather interpretation codes (subset).
  if (code == 0) return "Clear";
  if (code <= 2) return "Mostly clear";
  if (code == 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Showers";
  if (code >= 85 && code <= 86) return "Snow showers";
  if (code >= 95) return "Thunder";
  return "Weather";
}

static const char* portalPasswordOrNull() {
  return (strlen(PORTAL_AP_PASS) >= 8) ? PORTAL_AP_PASS : nullptr;
}

static void weatherTaskMain(void* param) {
  (void)param;

  char out[sizeof(gWeatherText)] = {0};
  snprintf(out, sizeof(out), "%s weather: update failed", WEATHER_LABEL);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  char url[256];
  snprintf(url,
           sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current="
           "temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,weather_code&"
           "forecast_days=1&timezone=Europe%%2FCopenhagen",
           static_cast<double>(WEATHER_LATITUDE),
           static_cast<double>(WEATHER_LONGITUDE));

  if (https.begin(client, url)) {
    const int httpCode = https.GET();
    if (httpCode == 200) {
      const String payload = https.getString();
      StaticJsonDocument<4096> doc;
      const DeserializationError err = deserializeJson(doc, payload);
      if (!err) {
        const float temp = doc["current"]["temperature_2m"] | NAN;
        const int code = doc["current"]["weather_code"] | -1;

        const float tmax = doc["daily"]["temperature_2m_max"][0] | NAN;
        const float tmin = doc["daily"]["temperature_2m_min"][0] | NAN;
        const int dcode = doc["daily"]["weather_code"][0] | -1;

        if (!isnan(temp)) {
          snprintf(out,
                   sizeof(out),
                   "%s: %.0f°C %s | Today %.0f–%.0f°C %s",
                   WEATHER_LABEL,
                   static_cast<double>(temp),
                   wmoCodeToShortText(code),
                   static_cast<double>(tmin),
                   static_cast<double>(tmax),
                   wmoCodeToShortText(dcode));
        }
      } else {
        snprintf(out, sizeof(out), "%s weather: parse error", WEATHER_LABEL);
      }
    } else {
      snprintf(out, sizeof(out), "%s weather: HTTP %d", WEATHER_LABEL, httpCode);
    }
    https.end();
  } else {
    snprintf(out, sizeof(out), "%s weather: TLS init failed", WEATHER_LABEL);
  }

  portENTER_CRITICAL(&gWeatherMux);
  strncpy(gWeatherText, out, sizeof(gWeatherText));
  gWeatherText[sizeof(gWeatherText) - 1] = '\0';
  gWeatherHasData = true;
  gWeatherNextFetchMs = millis() + (30UL * 60UL * 1000UL);
  gWeatherScrollPx = 0;
  portEXIT_CRITICAL(&gWeatherMux);

  gWeatherTaskRunning = false;
  gWeatherTask = nullptr;
  vTaskDelete(nullptr);
}

static void weatherTick() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (gWeatherTaskRunning) return;

  const uint32_t now = millis();
  if (gWeatherNextFetchMs != 0 && now < gWeatherNextFetchMs) return;

  gWeatherTaskRunning = true;
  xTaskCreatePinnedToCore(weatherTaskMain, "weather", 8192, nullptr, 1, &gWeatherTask, 0);
}

static void footerTick() {
  const uint32_t now = millis();
  if (now < gFooterNextTickMs) return;
  gFooterNextTickMs = now + 250;

  char weatherLocal[sizeof(gWeatherText)];
  portENTER_CRITICAL(&gWeatherMux);
  strncpy(weatherLocal, gWeatherText, sizeof(weatherLocal));
  weatherLocal[sizeof(weatherLocal) - 1] = '\0';
  portEXIT_CRITICAL(&gWeatherMux);

  const int16_t w = M5.Lcd.width();
  const int16_t padX = 8;
  const int16_t batX = static_cast<int16_t>(w - padX - 28 - 3);
  const int16_t textMaxW = static_cast<int16_t>(batX - padX - 8);
  const int16_t textW = M5.Lcd.textWidth(weatherLocal, 2);
  const bool shouldScroll = textW > textMaxW;
  if (shouldScroll) gWeatherScrollPx += 2;

  batterySampleTick();
  const bool batChanged = (static_cast<int8_t>(gBatteryPctCached) != gLastDrawnBatteryPct) ||
                          (gBatteryChargingCached != gLastDrawnCharging);

  if (batChanged) {
    uiDrawFooterFull(false);
  } else if (shouldScroll) {
    uiDrawFooterWeatherOnly(weatherLocal);
  }
}

static void wifiManagerApCallback(WiFiManager* wifiManager) {
  (void)wifiManager;
  Serial.println("[WiFi] Config portal started");
  uiMarkDirty();
}

static void wifiStartConnecting() {
  gLastError = "";
  if (gPortalActive) {
    gWiFiManager.stopConfigPortal();
    gPortalActive = false;
  }
  gWifiState = WifiState::Connecting;
  gWifiDeadlineMs = millis() + kConnectTimeoutMs;

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);

  if (strlen(WIFI_SSID) > 0) {
    Serial.println("[WiFi] Connecting (secrets)");
    gConnectUsingSecrets = true;
    gConnectTarget = WIFI_SSID;
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  } else {
    Serial.println("[WiFi] Connecting (saved creds)");
    gConnectUsingSecrets = false;
    gConnectTarget = "";
    WiFi.begin();  // uses stored credentials if present
  }
  gLastStaStatus = WiFi.status();

  uiMarkDirty();
}

static void wifiStartPortal(bool resetFirst) {
  Serial.println("[WiFi] Starting config portal");

  if (gPortalActive) {
    gWiFiManager.stopConfigPortal();
    gPortalActive = false;
  }

  if (resetFirst) {
    Serial.println("[WiFi] Resetting saved WiFi config");
    gWiFiManager.resetSettings();
  }

  gWiFiManager.setAPCallback(wifiManagerApCallback);
  gWiFiManager.setConnectTimeout(15);
  gWiFiManager.setConfigPortalBlocking(false);
  WiFi.setSleep(false);

  gPortalActive = true;
  gWifiState = WifiState::Portal;
  gPortalDeadlineMs = millis() + kPortalTimeoutMs;

  // Non-blocking: returns immediately; we keep calling process() from loop().
  gWiFiManager.startConfigPortal(kPortalApName, portalPasswordOrNull());

  uiMarkDirty();
}

static void wifiTick() {
  const wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    if (gWifiState != WifiState::Connected) {
      Serial.println("[WiFi] Connected");
      WiFi.setSleep(true);
      if (gPortalActive) {
        gWiFiManager.stopConfigPortal();
        gPortalActive = false;
      }
      gWifiState = WifiState::Connected;
      uiMarkDirty();
    }
    return;
  }

  if (st != gLastStaStatus) {
    gLastStaStatus = st;
    Serial.printf("[WiFi] STA status: %d (%s)\n", static_cast<int>(st), staStatusToString(st));
    uiMarkDirty();
  }

  if (gWifiState == WifiState::Portal && gPortalActive) {
    gWiFiManager.process();
    if (millis() > gPortalDeadlineMs) {
      Serial.println("[WiFi] Portal timeout");
      gWiFiManager.stopConfigPortal();
      gPortalActive = false;
      gWifiState = WifiState::Error;
      gLastError = "Portal timeout";
      uiMarkDirty();
    }
    return;
  }

  if (gWifiState == WifiState::Connecting) {
    if (st == WL_CONNECT_FAILED) {
      Serial.println("[WiFi] Auth failed; starting portal");
      wifiStartPortal(false);
      return;
    }
    if (millis() > gWifiDeadlineMs) {
      Serial.println("[WiFi] Connect timeout; starting portal");
      wifiStartPortal(false);
    }
    return;
  }

  if (gWifiState == WifiState::Connected) {
    // Lost connection: try reconnect for a while, then portal.
    Serial.println("[WiFi] Disconnected; retrying");
    wifiStartConnecting();
    return;
  }
}

static void inputTick() {
  if (gSwipeLeft.wasDetected()) {
    noteInteraction();
    if (gView == View::Status) gView = View::WiFi;
    else if (gView == View::WiFi) gView = View::About;
    else gView = View::Status;
    uiMarkDirty();
  } else if (gSwipeRight.wasDetected()) {
    noteInteraction();
    if (gView == View::Status) gView = View::About;
    else if (gView == View::About) gView = View::WiFi;
    else gView = View::Status;
    uiMarkDirty();
  }

  if (gHitTabStatus && gHitTabStatus->wasPressed()) {
    noteInteraction();
    gView = View::Status;
    uiMarkDirty();
  } else if (gHitTabWiFi && gHitTabWiFi->wasPressed()) {
    noteInteraction();
    gView = View::WiFi;
    uiMarkDirty();
  } else if (gHitTabAbout && gHitTabAbout->wasPressed()) {
    noteInteraction();
    gView = View::About;
    uiMarkDirty();
  }

  if (gView != View::WiFi) return;

  if (gHitPortal && gHitPortal->wasPressed()) {
    noteInteraction();
    wifiStartPortal(false);
  } else if (gHitRetry && gHitRetry->wasPressed()) {
    noteInteraction();
    wifiStartConnecting();
  } else if (gHitForget && gHitForget->wasPressed()) {
    noteInteraction();
    wifiStartPortal(true);
  }
}

void setup() {
  M5.begin();
  Serial.begin(115200);

  gLastInteractionMs = millis();
  M5.Lcd.setBrightness(kBrightnessActive);
  gCurrentBrightness = kBrightnessActive;
  gBatteryNextSampleMs = 0;
  batterySampleTick();

  uiInit();
  wifiStartConnecting();
  uiDrawFull();
  gUiDirty = false;
  gUiNextRefreshMs = millis() + 1000;
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    noteInteraction();
    wifiStartPortal(false);
  }
  if (M5.BtnB.wasPressed()) {
    noteInteraction();
    if (gView == View::Status) gView = View::WiFi;
    else if (gView == View::WiFi) gView = View::About;
    else gView = View::Status;
    uiMarkDirty();
  }
  if (M5.BtnC.wasPressed()) {
    noteInteraction();
    if (gView == View::Status) gView = View::About;
    else if (gView == View::About) gView = View::WiFi;
    else gView = View::Status;
    uiMarkDirty();
  }

  wifiTick();
  inputTick();

  const uint32_t now = millis();
  if (gUiDirty) {
    uiDrawFull();
    gUiDirty = false;
    gUiNextRefreshMs = now + 1000;
  } else if (now > gUiNextRefreshMs) {
    uiUpdateDynamic();
    if (gUiDirty) {
      uiDrawFull();
      gUiDirty = false;
    }
    gUiNextRefreshMs = now + 1000;
  }

  weatherTick();
  footerTick();
  powerTick();

  delay(10);
}
