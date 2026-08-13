#include "andon_wifi.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <lvgl.h>
#include <time.h>

#include "secrets.h" // WIFI_SSID, WIFI_PASSWORD - gitignored, see secrets.example.h

// NTP, so the header clock (main.cpp's g_headerTimeLabel, refreshed in
// tickTimerCb() via getLocalTime()) shows real time instead of the fixed
// "08:00" placeholder it used to. Same default as gemini-chatbot
// (GMT+7/Indonesia, no DST) - change if this terminal deploys elsewhere.
static const char *NTP_SERVER = "pool.ntp.org";
static const long GMT_OFFSET_SEC = 7 * 3600;
static const int DAYLIGHT_OFFSET_SEC = 0;

// ---------------------------------------------------------------------------
// Ported from examples/gemini-chatbot/src/main.cpp almost verbatim - same
// T9 keypad, same screen layout/coordinates (identical hardware/rotation,
// so the same touch-dead-zone-safe positions apply directly), same NVS
// storage shape. Renamed out of the "chat_"/"wifi_" prefixes that made
// sense there (this file has no chat screen to disambiguate from) and
// dropped gemini-chatbot's later SD-card multi-network layer - see
// andon_wifi.hpp's header comment for why.
// ---------------------------------------------------------------------------

// --- NVS storage (single saved network) -------------------------------------

static Preferences s_wifiPrefs;

static bool loadSavedWifi(String &ssid, String &pass) {
  s_wifiPrefs.begin("wifi", true); // read-only
  ssid = s_wifiPrefs.getString("ssid", "");
  pass = s_wifiPrefs.getString("pass", "");
  s_wifiPrefs.end();
  return ssid.length() > 0;
}

static void saveWifi(const String &ssid, const String &pass) {
  s_wifiPrefs.begin("wifi", false);
  s_wifiPrefs.putString("ssid", ssid);
  s_wifiPrefs.putString("pass", pass);
  s_wifiPrefs.end();
}

// Blocking connect attempt (up to ~10s), updating statusLabel (if given) as
// it goes. Returns true on success.
static bool tryConnectWifi(const String &ssid, const String &pass, lv_obj_t *statusLabel) {
  Serial.printf("AndonWifi: connecting to '%s'...\r\n", ssid.c_str());
  WiFi.disconnect(true);
  delay(1000); // full radio power-cycle from disconnect(true) needs this long to settle
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    if (statusLabel) {
      String dots = "";
      for (int i = 0; i < (attempts % 4); i++) dots += ".";
      lv_label_set_text_fmt(statusLabel, "Connecting%s", dots.c_str());
      lv_refr_now(NULL);
    }
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  Serial.println(ok ? "AndonWifi: connected!" : "AndonWifi: connect failed.");
  if (ok) {
    // Single choke point every successful connect (boot fallback, saved-
    // network reconnect, and the interactive setup screen's Select/Send)
    // flows through, so this only needs to live here. configTime() itself
    // is non-blocking - it kicks off the SNTP client and returns
    // immediately; getLocalTime() (see main.cpp's tickTimerCb) is what
    // actually waits for/reports sync completion, bounded by its own
    // short timeout so it never stalls the 1Hz UI timer.
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  }
  return ok;
}

// --- NVS storage (backend host, separate namespace from WiFi creds) --------

static Preferences s_serverPrefs;

String AndonWifi::getServerHost() {
  s_serverPrefs.begin("server", true); // read-only
  String host = s_serverPrefs.getString("host", "");
  s_serverPrefs.end();
  return host;
}

static void saveServerHost(const String &host) {
  s_serverPrefs.begin("server", false);
  s_serverPrefs.putString("host", host);
  s_serverPrefs.end();
}

bool AndonWifi::connectSavedOrFallback(lv_obj_t *statusLabel) {
  String ssid, pass;
  if (loadSavedWifi(ssid, pass)) {
    if (tryConnectWifi(ssid, pass, statusLabel)) return true;
  }
  if (strlen(WIFI_SSID) > 0) {
    if (tryConnectWifi(WIFI_SSID, WIFI_PASSWORD, statusLabel)) {
      saveWifi(WIFI_SSID, WIFI_PASSWORD); // adopt it into NVS going forward
      return true;
    }
  }
  return false;
}

// --- QWERTY keyboard (EXPERIMENTAL - branch experiment/qwerty-wifi-keyboard,
// 2026-08-13) --------------------------------------------------------------
//
// Replaces the T9 multi-tap grid (gemini-chatbot's original design, ported
// here as-is until now) with LVGL's built-in lv_keyboard widget, sized to
// use nearly the full screen width for bigger keys ("agak besar supaya
// ngga miss klik" - explicit request). Trades away T9's "no real keyboard,
// stays true to the feature-phone bit" identity for standard-QWERTY speed
// and (hopefully) fewer mis-taps - that trade is exactly what this branch
// exists to evaluate before deciding whether to bring it back to main.
//
// lv_keyboard handles character insertion/backspace/cursor-move/shift/
// mode-switching internally once bound to a textarea (lv_keyboard_set_
// textarea()) - none of the T9 multi-tap-cycle bookkeeping below is
// needed anymore. Its default map's bottom-left key (a "keyboard" glyph)
// fires LV_EVENT_CANCEL - repurposed as this screen's Back button (see
// onPwKeyboardEvent()) instead of the old dedicated s_pwBackBtn widget,
// and its OK key fires LV_EVENT_READY - repurposed as Send/Save (was
// T9_SEND_IDX). See lv_keyboard_def_event_cb() in
// lib/lvgl/src/extra/widgets/keyboard/lv_keyboard.c for exactly what
// triggers what.

// --- Screen state ------------------------------------------------------------

#define WIFI_LIST_VISIBLE_ROWS 7
static lv_obj_t *s_listTitle = nullptr;
static lv_obj_t *s_listBox = nullptr;
static lv_obj_t *s_listRows[WIFI_LIST_VISIBLE_ROWS] = {nullptr};
static lv_obj_t *s_listRowLabels[WIFI_LIST_VISIBLE_ROWS] = {nullptr};
static lv_obj_t *s_upBtn = nullptr;
static lv_obj_t *s_selectBtn = nullptr;
static lv_obj_t *s_downBtn = nullptr;
static lv_obj_t *s_cancelBtn = nullptr;
static lv_obj_t *s_skipBtn = nullptr;
static int s_listIdx = 0;
static int s_listCount = 0;

static lv_obj_t *s_pwTitle = nullptr;
static lv_obj_t *s_pwTa = nullptr;
static lv_obj_t *s_pwStatusLabel = nullptr;
static lv_obj_t *s_pwKb = nullptr; // lv_keyboard now, not lv_btnmatrix - see the QWERTY comment above
static String s_setupSsid = "";

// "Server" tab - same screen, same keyboard, different target textarea.
// s_activeTa is whichever of s_pwTa/s_serverTa is currently bound to the
// keyboard; kept as its own pointer (updated by onTabToggle() and
// showPasswordView(), which also call lv_keyboard_set_textarea(s_pwKb, ...)
// to keep the widget's own binding in sync - see applyTabVisibility()) so
// onPwKeyboardEvent() doesn't need to branch on s_onServerTab for anything
// but which save/connect action READY should trigger.
static bool s_onServerTab = false;
static lv_obj_t *s_serverTa = nullptr;
static lv_obj_t *s_tabBtn = nullptr;
static lv_obj_t *s_activeTa = nullptr;

static bool s_setupRequested = false;
static bool s_setupDone = false;
static bool s_setupConnected = false;

void AndonWifi::requestSetup() { s_setupRequested = true; }
bool AndonWifi::isSetupRequested() { return s_setupRequested; }
void AndonWifi::clearSetupRequest() { s_setupRequested = false; }

static void showListView();
static void showPasswordView(const String &ssid);

static void updateListRows() {
  if (s_listCount <= 0) {
    for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) lv_obj_add_flag(s_listRows[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_listRows[0], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_listRowLabels[0], "No networks found");
    return;
  }

  int start = s_listIdx - WIFI_LIST_VISIBLE_ROWS / 2;
  if (start > s_listCount - WIFI_LIST_VISIBLE_ROWS) start = s_listCount - WIFI_LIST_VISIBLE_ROWS;
  if (start < 0) start = 0;

  for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
    int netIdx = start + i;
    if (netIdx >= s_listCount) {
      lv_obj_add_flag(s_listRows[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(s_listRows[i], LV_OBJ_FLAG_HIDDEN);
    bool isOpen = (WiFi.encryptionType(netIdx) == WIFI_AUTH_OPEN);
    lv_label_set_text_fmt(s_listRowLabels[i], "%s%s",
                           isOpen ? "" : LV_SYMBOL_CLOSE " ",
                           WiFi.SSID(netIdx).c_str());
    bool highlighted = (netIdx == s_listIdx);
    lv_obj_set_style_bg_color(s_listRows[i], highlighted ? lv_color_hex(0x22c55e) : lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_text_color(s_listRowLabels[i], highlighted ? lv_color_hex(0x0f172a) : lv_color_hex(0xffffff), 0);
  }
}

static void showListView() {
  lv_obj_clear_flag(s_listTitle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_listBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_upBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_selectBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_downBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_cancelBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_skipBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pwTitle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pwTa, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_serverTa, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pwStatusLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_tabBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pwKb, LV_OBJ_FLAG_HIDDEN);
}

// Shows whichever of s_pwTa/s_serverTa matches s_onServerTab, updates the
// title and the tab button's own label to name the OTHER tab (what tapping
// it switches to), and points both s_activeTa and the keyboard's own
// binding (lv_keyboard_set_textarea()) at the now-visible field - the
// keyboard widget routes keystrokes via its own internal ->ta pointer, not
// s_activeTa, so both must stay in sync (onPwKeyboardEvent() only needs
// s_activeTa for reading the final text out on READY).
static void applyTabVisibility() {
  if (s_onServerTab) {
    lv_label_set_text(s_pwTitle, "Backend server IP/domain");
    lv_obj_add_flag(s_pwTa, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_serverTa, LV_OBJ_FLAG_HIDDEN);
    s_activeTa = s_serverTa;
  } else {
    lv_label_set_text_fmt(s_pwTitle, "Password for: %s", s_setupSsid.c_str());
    lv_obj_clear_flag(s_pwTa, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_serverTa, LV_OBJ_FLAG_HIDDEN);
    s_activeTa = s_pwTa;
  }
  lv_keyboard_set_textarea(s_pwKb, s_activeTa);
}

static void onTabToggle(lv_event_t *e) {
  s_onServerTab = !s_onServerTab;
  lv_label_set_text(s_pwStatusLabel, "");
  if (s_onServerTab) {
    lv_textarea_set_text(s_serverTa, AndonWifi::getServerHost().c_str());
    lv_obj_t *tabLabel = lv_obj_get_child(s_tabBtn, 0);
    lv_label_set_text(tabLabel, LV_SYMBOL_LEFT "\nWiFi");
  } else {
    lv_obj_t *tabLabel = lv_obj_get_child(s_tabBtn, 0);
    lv_label_set_text(tabLabel, "Server\n" LV_SYMBOL_RIGHT);
  }
  applyTabVisibility();
}

static void showPasswordView(const String &ssid) {
  s_setupSsid = ssid;
  s_onServerTab = false; // always reopen on the WiFi tab, not wherever it was left
  lv_keyboard_set_mode(s_pwKb, LV_KEYBOARD_MODE_TEXT_LOWER); // reset shift/special-chars state from any previous visit
  lv_textarea_set_text(s_pwTa, "");
  lv_label_set_text(s_pwStatusLabel, "");
  lv_obj_t *tabLabel = lv_obj_get_child(s_tabBtn, 0);
  lv_label_set_text(tabLabel, "Server\n" LV_SYMBOL_RIGHT);
  applyTabVisibility(); // sets s_pwTitle text too, so no separate title line needed here

  // s_listTitle and s_pwTitle both sit at the same TOP_MID/y=8 spot (see
  // gemini-chatbot's history - they stacked/overlapped before that file
  // learned to hide one when showing the other); hide it here for the
  // same reason.
  lv_obj_add_flag(s_listTitle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_listBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_upBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_selectBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_downBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_cancelBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_skipBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_pwTitle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_pwStatusLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_tabBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_pwKb, LV_OBJ_FLAG_HIDDEN);
}

// lv_keyboard handles every ordinary keystroke (letters, backspace,
// cursor-move, shift, special-chars mode) internally once bound via
// lv_keyboard_set_textarea() - the only two things this firmware still
// needs to react to are its READY (OK key - was T9_SEND_IDX) and CANCEL
// (the keyboard-glyph key - repurposed as Back, was s_pwBackBtn) events.
// See the QWERTY comment block above for exactly which key fires which.
static void onPwKeyboardEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_CANCEL) {
    showListView();
    return;
  }
  if (code != LV_EVENT_READY) return;

  if (s_onServerTab) {
    // Just persists the field and stays on this screen - unlike the WiFi
    // tab's Send, there's no "did it work" to test synchronously (that
    // only happens once AndonConfig::sync()/AndonMqtt actually reach the
    // host), so this can't set s_setupDone the way a successful WiFi
    // connect does.
    String host = lv_textarea_get_text(s_activeTa);
    saveServerHost(host);
    lv_label_set_text(s_pwStatusLabel, "Saved!");
    return;
  }
  String pass = lv_textarea_get_text(s_activeTa);
  lv_label_set_text(s_pwStatusLabel, "Connecting...");
  lv_refr_now(NULL);
  if (tryConnectWifi(s_setupSsid, pass, s_pwStatusLabel)) {
    saveWifi(s_setupSsid, pass);
    s_setupConnected = true;
    s_setupDone = true;
  } else {
    lv_label_set_text(s_pwStatusLabel, "Failed - check password, try again");
  }
}

static void onSkip(lv_event_t *e) {
  s_setupConnected = false;
  s_setupDone = true;
}

// Distinct from Skip: reconnects to whatever was already working (saved
// network / secrets.h) rather than forcing offline - matches
// gemini-chatbot's wifiListCancelBtnEventCb().
static void onCancel(lv_event_t *e) {
  AndonWifi::connectSavedOrFallback(nullptr);
  s_setupConnected = (WiFi.status() == WL_CONNECTED);
  s_setupDone = true;
}

static void onUp(lv_event_t *e) {
  if (s_listCount <= 0) return;
  s_listIdx = (s_listIdx - 1 + s_listCount) % s_listCount;
  updateListRows();
}

static void onDown(lv_event_t *e) {
  if (s_listCount <= 0) return;
  s_listIdx = (s_listIdx + 1) % s_listCount;
  updateListRows();
}

static void onSelect(lv_event_t *e) {
  if (s_listCount <= 0) return;
  String ssid = WiFi.SSID(s_listIdx);
  bool isOpen = (WiFi.encryptionType(s_listIdx) == WIFI_AUTH_OPEN);
  if (isOpen) {
    if (tryConnectWifi(ssid, "", nullptr)) {
      saveWifi(ssid, "");
      s_setupConnected = true;
      s_setupDone = true;
      return;
    }
    // fall through to password view if an "open" network unexpectedly
    // still needs one (captive portals etc. aren't handled)
  }
  showPasswordView(ssid);
}

// Builds the whole setup screen (list + password views, list shown first)
// on a freshly wiped lv_scr_act() - see andon_wifi.hpp's runSetupFlow()
// comment for why this owns the full screen instead of just g_content.
static void createSetupScreen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  s_listTitle = lv_label_create(lv_scr_act());
  lv_label_set_text(s_listTitle, "Select a WiFi network");
  lv_obj_set_style_text_color(s_listTitle, lv_color_hex(0x22c55e), 0);
  lv_obj_align(s_listTitle, LV_ALIGN_TOP_MID, 0, 8);
  lv_refr_now(NULL); // paint the title before the blocking scan below

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    delay(1000); // full radio power-cycle from disconnect(true) needs this long to settle
    WiFi.mode(WIFI_STA);
    delay(100);
  }

  Serial.println("AndonWifi: scanning...");
  int n = WiFi.scanNetworks();
  Serial.printf("AndonWifi: scanNetworks() returned %d\r\n", n);
  // ESP32 quirk: the very first scanNetworks() right after a mode/radio
  // change frequently comes back 0 or WIFI_SCAN_FAILED (-2) - retry once.
  for (int attempt = 0; n <= 0 && attempt < 2; attempt++) {
    Serial.println("AndonWifi: scan came back empty/failed - retrying...");
    delay(300);
    n = WiFi.scanNetworks();
    Serial.printf("AndonWifi: scanNetworks() retry returned %d\r\n", n);
  }
  s_listCount = n > 0 ? n : 0;
  s_listIdx = 0;

  s_listBox = lv_obj_create(lv_scr_act());
  lv_obj_set_size(s_listBox, 290, 280);
  lv_obj_align(s_listBox, LV_ALIGN_TOP_LEFT, 10, 35);
  lv_obj_set_style_bg_color(s_listBox, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_listBox, 2, 0);
  lv_obj_set_style_border_color(s_listBox, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_radius(s_listBox, 8, 0);
  lv_obj_set_style_pad_all(s_listBox, 4, 0);
  lv_obj_clear_flag(s_listBox, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
    s_listRows[i] = lv_obj_create(s_listBox);
    lv_obj_set_size(s_listRows[i], 274, 36);
    lv_obj_set_pos(s_listRows[i], 0, i * 39);
    lv_obj_set_style_bg_color(s_listRows[i], lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(s_listRows[i], 1, 0);
    lv_obj_set_style_border_color(s_listRows[i], lv_color_hex(0x22c55e), 0);
    lv_obj_set_style_radius(s_listRows[i], 6, 0);
    lv_obj_set_style_pad_all(s_listRows[i], 0, 0);
    lv_obj_clear_flag(s_listRows[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_listRows[i], LV_OBJ_FLAG_CLICKABLE);

    s_listRowLabels[i] = lv_label_create(s_listRows[i]);
    lv_obj_set_style_text_color(s_listRowLabels[i], lv_color_hex(0xffffff), 0);
    lv_label_set_long_mode(s_listRowLabels[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_listRowLabels[i], 258);
    lv_obj_align(s_listRowLabels[i], LV_ALIGN_LEFT_MID, 8, 0);
  }

  if (n < 0) {
    for (int i = 1; i < WIFI_LIST_VISIBLE_ROWS; i++) lv_obj_add_flag(s_listRows[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(s_listRowLabels[0], "Scan failed (code %d)", n);
  } else {
    updateListRows();
  }

  const int32_t COL2_X = 310, COL2_W = 160;
  const int32_t ROW_H = 37, ROW_GAP = 4;
  int32_t col2Y = 113;

  s_upBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_upBtn, COL2_W, ROW_H);
  lv_obj_align(s_upBtn, LV_ALIGN_TOP_LEFT, COL2_X, col2Y);
  lv_obj_set_style_radius(s_upBtn, 10, 0);
  lv_obj_set_style_bg_color(s_upBtn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_upBtn, 1, 0);
  lv_obj_set_style_border_color(s_upBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_add_event_cb(s_upBtn, onUp, LV_EVENT_CLICKED, NULL);
  lv_obj_t *upLabel = lv_label_create(s_upBtn);
  lv_label_set_text(upLabel, LV_SYMBOL_UP);
  lv_obj_center(upLabel);
  col2Y += ROW_H + ROW_GAP;

  s_selectBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_selectBtn, COL2_W, ROW_H);
  lv_obj_align(s_selectBtn, LV_ALIGN_TOP_LEFT, COL2_X, col2Y);
  lv_obj_set_style_radius(s_selectBtn, 10, 0);
  lv_obj_set_style_bg_color(s_selectBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_add_event_cb(s_selectBtn, onSelect, LV_EVENT_CLICKED, NULL);
  lv_obj_t *selectLabel = lv_label_create(s_selectBtn);
  lv_label_set_text(selectLabel, "Select");
  lv_obj_center(selectLabel);
  col2Y += ROW_H + ROW_GAP;

  s_downBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_downBtn, COL2_W, ROW_H);
  lv_obj_align(s_downBtn, LV_ALIGN_TOP_LEFT, COL2_X, col2Y);
  lv_obj_set_style_radius(s_downBtn, 10, 0);
  lv_obj_set_style_bg_color(s_downBtn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_downBtn, 1, 0);
  lv_obj_set_style_border_color(s_downBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_add_event_cb(s_downBtn, onDown, LV_EVENT_CLICKED, NULL);
  lv_obj_t *downLabel = lv_label_create(s_downBtn);
  lv_label_set_text(downLabel, LV_SYMBOL_DOWN);
  lv_obj_center(downLabel);
  col2Y += ROW_H + ROW_GAP;

  s_cancelBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_cancelBtn, COL2_W, ROW_H);
  lv_obj_align(s_cancelBtn, LV_ALIGN_TOP_LEFT, COL2_X, col2Y);
  lv_obj_set_style_radius(s_cancelBtn, 10, 0);
  lv_obj_set_style_bg_color(s_cancelBtn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_cancelBtn, 1, 0);
  lv_obj_set_style_border_color(s_cancelBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_add_event_cb(s_cancelBtn, onCancel, LV_EVENT_CLICKED, NULL);
  lv_obj_t *cancelLabel = lv_label_create(s_cancelBtn);
  lv_label_set_text(cancelLabel, LV_SYMBOL_LEFT " Back");
  lv_obj_center(cancelLabel);
  col2Y += ROW_H + ROW_GAP;

  s_skipBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_skipBtn, COL2_W, ROW_H);
  lv_obj_align(s_skipBtn, LV_ALIGN_TOP_LEFT, COL2_X, col2Y);
  lv_obj_set_style_radius(s_skipBtn, 10, 0);
  lv_obj_set_style_bg_color(s_skipBtn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_skipBtn, 1, 0);
  lv_obj_set_style_border_color(s_skipBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_add_event_cb(s_skipBtn, onSkip, LV_EVENT_CLICKED, NULL);
  lv_obj_t *skipLabel = lv_label_create(s_skipBtn);
  lv_label_set_text(skipLabel, "Skip");
  lv_obj_center(skipLabel);

  s_pwTitle = lv_label_create(lv_scr_act());
  lv_label_set_text(s_pwTitle, "Password for:");
  lv_obj_set_style_text_color(s_pwTitle, lv_color_hex(0x22c55e), 0);
  lv_obj_align(s_pwTitle, LV_ALIGN_TOP_MID, 0, 8);

  s_pwTa = lv_textarea_create(lv_scr_act());
  lv_obj_set_size(s_pwTa, 460, 40);
  lv_obj_align(s_pwTa, LV_ALIGN_TOP_MID, 0, 35);
  lv_textarea_set_one_line(s_pwTa, true);
  lv_textarea_set_password_mode(s_pwTa, false); // shown in plain text, not masked (matches gemini-chatbot)
  lv_textarea_set_placeholder_text(s_pwTa, "Password");
  lv_obj_set_style_radius(s_pwTa, 16, 0);
  lv_obj_set_style_border_width(s_pwTa, 2, 0);
  lv_obj_set_style_border_color(s_pwTa, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_color(s_pwTa, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_text_color(s_pwTa, lv_color_hex(0xffffff), 0);

  // Same style/position as s_pwTa (only one of the two is ever visible -
  // see applyTabVisibility()), for the Server tab. Bare host only - no
  // "http://" scheme and no ":port" (andon_config.cpp/andon_mqtt.cpp each
  // append their own fixed port, 8080/MQTT_BROKER_PORT, to whatever's
  // saved here - see AndonWifi::getServerHost()'s comment). A domain name
  // works exactly the same as an IP as long as it resolves to the same
  // host serving both those ports (e.g. the docker-compose backend) -
  // there's no reverse-proxy/non-default-port support here, out of scope
  // for this local test harness (see backend/src/server.ts's own note).
  s_serverTa = lv_textarea_create(lv_scr_act());
  lv_obj_set_size(s_serverTa, 460, 40);
  lv_obj_align(s_serverTa, LV_ALIGN_TOP_MID, 0, 35);
  lv_textarea_set_one_line(s_serverTa, true);
  lv_textarea_set_password_mode(s_serverTa, false);
  lv_textarea_set_placeholder_text(s_serverTa, "IP or domain, e.g. 192.168.1.50");
  lv_obj_set_style_radius(s_serverTa, 16, 0);
  lv_obj_set_style_border_width(s_serverTa, 2, 0);
  lv_obj_set_style_border_color(s_serverTa, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_color(s_serverTa, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_text_color(s_serverTa, lv_color_hex(0xffffff), 0);
  lv_obj_add_flag(s_serverTa, LV_OBJ_FLAG_HIDDEN);

  s_pwStatusLabel = lv_label_create(lv_scr_act());
  lv_label_set_text(s_pwStatusLabel, "");
  lv_obj_set_style_text_color(s_pwStatusLabel, lv_color_hex(0xffffff), 0);
  lv_obj_align(s_pwStatusLabel, LV_ALIGN_TOP_MID, 0, 80);

  // Single small button now (was a 3-stacked column: Symbols/Back/Tab) -
  // lv_keyboard's own OK/keyboard-glyph keys cover Send and Back (see
  // onPwKeyboardEvent()), and its own "1#"/"ABC" keys cover symbols/shift,
  // so only the WiFi<->Server tab switch still needs a dedicated widget.
  // Freeing that column's width is the actual point of this experiment -
  // bigger QWERTY keys, not just fitting one more button.
  const int32_t PW_BTN_W = 70, PW_BTN_H = 36;
  s_tabBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_tabBtn, PW_BTN_W, PW_BTN_H);
  lv_obj_align(s_tabBtn, LV_ALIGN_TOP_RIGHT, -10, 113);
  lv_obj_add_event_cb(s_tabBtn, onTabToggle, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(s_tabBtn, 12, 0);
  lv_obj_set_style_bg_color(s_tabBtn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_tabBtn, 1, 0);
  lv_obj_set_style_border_color(s_tabBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_t *tabLabel = lv_label_create(s_tabBtn);
  lv_label_set_text(tabLabel, "Server\n" LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(tabLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_align(tabLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(tabLabel);

  // QWERTY keyboard - nearly full content width (460 of 480) for bigger
  // keys than the old T9 grid's 380px had room for, starting right below
  // the tab button's row (both start at the same y=113 dead-zone-safe
  // line - see the CONTENT_W/MARGIN/GAP comment convention this firmware
  // otherwise uses in main.cpp). lv_keyboard_set_textarea() (called from
  // applyTabVisibility()) is what actually routes keystrokes - this
  // create call just builds the widget and styles it to match the rest of
  // the dark/green theme.
  s_pwKb = lv_keyboard_create(lv_scr_act());
  lv_obj_add_event_cb(s_pwKb, onPwKeyboardEvent, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(s_pwKb, onPwKeyboardEvent, LV_EVENT_CANCEL, NULL);
  lv_obj_set_style_bg_opa(s_pwKb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s_pwKb, 0, 0);
  lv_obj_set_style_pad_all(s_pwKb, 4, 0);
  lv_obj_set_style_pad_row(s_pwKb, 6, 0);
  lv_obj_set_style_pad_column(s_pwKb, 6, 0);
  lv_obj_set_style_radius(s_pwKb, 8, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s_pwKb, lv_color_hex(0x1e293b), LV_PART_ITEMS);
  lv_obj_set_style_border_width(s_pwKb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(s_pwKb, lv_color_hex(0x22c55e), LV_PART_ITEMS);
  lv_obj_set_style_text_color(s_pwKb, lv_color_hex(0xffffff), LV_PART_ITEMS);
  lv_obj_set_style_bg_color(s_pwKb, lv_color_hex(0x22c55e), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(s_pwKb, lv_color_hex(0x0f172a), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_size(s_pwKb, 460, 160);
  lv_obj_align(s_pwKb, LV_ALIGN_TOP_LEFT, 10, 155);

  showListView(); // start on the list view

  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(NULL);
}

bool AndonWifi::runSetupFlow() {
  // Nothing here to pause before the lv_obj_clean(lv_scr_act()) below (no
  // header timer keeps writing into freed header/content widgets in this
  // firmware, unlike gemini-chatbot's chat_time_label - the gear button
  // that reaches this is only ever shown on SCR-01, where g_elapsedLabel/
  // g_waitLabel are already null; see tickTimerCb()'s null guards).
  s_setupDone = false;
  s_setupConnected = false;
  createSetupScreen();
  while (!s_setupDone) {
    lv_timer_handler();
    delay(15);
  }
  return s_setupConnected;
}
