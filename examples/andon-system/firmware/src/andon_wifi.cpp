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

// --- Custom big-key QWERTY-ish keyboard (EXPERIMENTAL - branch
// experiment/qwerty-wifi-keyboard, 2026-08-13) -------------------------------
//
// v2 of this experiment. v1 (see git log on this branch) tried LVGL's stock
// lv_keyboard widget - full 26-key QWERTY, uniform key size. Follow-up
// request: "kalau buat custom yang penting untuk key2 penting dibuat agak
// gede, untuk yg jarang2 kepake dibuat multiple" - i.e. don't size every
// key the same; give commonly-used keys more room and fold rarely-used
// letters onto shared multi-tap keys (T9-style cycling, but only for the
// few letters that are actually rare - not the whole alphabet like the
// original phone-number-pad T9 did).
//
// Letters grouped by rough English-frequency (j+k and z+x+v are each
// English's least-common letters - merging them costs little in typing
// speed since they're rarely needed, and buys real width back for every
// other key in their row):
//   Row 1 (10 keys): q w e r t y u i o p - unmodified, no good low-
//     frequency candidate to merge here without also giving up a common one
//   Row 2 (8 keys):  a s d f g h [j/k] l
//   Row 3 (7 keys):  Shift [z/x/v] c b n m Backspace
//   Row 4 (3 keys):  123 (switch to number/symbol mode) | Space | Send
// vs. the stock keyboard's ~40 cells across 5 rows - fewer, bigger cells,
// especially in rows 2-3 (~8-22% wider) and row 4 (Space/Send are each
// nearly 1/3 of the full 460px width).
//
// Multi-tap cycling on the [j/k] and [z/x/v] keys reuses the exact
// mechanism gemini-chatbot's/this branch's original T9 grid used (repeated
// taps within KEY_CYCLE_TIMEOUT_MS delete-and-advance instead of insert) -
// see s_cycleLastIdx/s_cyclePos/s_cycleLastMs and onPwKeypad() below.
// Every other letter, digit, and symbol is a single, immediate tap - only
// those two keys actually cycle.
#define KEY_CYCLE_TIMEOUT_MS 600

// --- LOWER/UPPER letter mode -------------------------------------------------
// Row breaks are literal "\n" array entries (lv_btnmatrix convention); a
// "\n" INSIDE a cell's own string (e.g. "j\nk") instead makes a 2-line
// label on that one button - LVGL draws it centered, stacked.
static const char *wifiKbMapLower[] = {
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
  "a", "s", "d", "f", "g", "h", "j\nk", "l", "\n",
  LV_SYMBOL_UP, "z\nx\nv", "c", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
  "123", "SPACE", LV_SYMBOL_OK, "",
};
static const char *wifiKbMapUpper[] = {
  "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
  "A", "S", "D", "F", "G", "H", "J\nK", "L", "\n",
  LV_SYMBOL_UP, "Z\nX\nV", "C", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
  "123", "SPACE", LV_SYMBOL_OK, "",
};
// Index -> the actual character(s) that cell cycles through (case-folded;
// shift uppercases at insert time, same as the old T9 code did - see
// onPwKeypad()). NULL entries are control cells (Shift/Backspace/123/
// Space/Send), handled by index instead of a cycle lookup.
static const char *wifiKbCycleLower[] = {
  "q", "w", "e", "r", "t", "y", "u", "i", "o", "p",              // 0-9
  "a", "s", "d", "f", "g", "h", "jk", "l",                       // 10-17
  NULL, "zxv", "c", "b", "n", "m", NULL,                         // 18-24 (18=Shift, 24=Backspace)
  NULL, NULL, NULL,                                              // 25-27 (123, Space, Send)
};
static const int KB_SHIFT_IDX = 18;
static const int KB_LETTER_BACKSPACE_IDX = 24;
static const int KB_MODE_IDX = 25;
static const int KB_SPACE_IDX = 26;
static const int KB_SEND_IDX = 27;

// --- Number/symbol mode ------------------------------------------------------
// No grouping here - digits and the symbols routers actually put in
// passwords are all "important", nothing to fold together.
static const char *wifiKbMapNumber[] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
  "@", "#", "$", "%", "^", "&", "*", "-", "_", ".", LV_SYMBOL_BACKSPACE, "\n",
  "ABC", "SPACE", LV_SYMBOL_OK, "",
};
static const int KB_NUMBER_BACKSPACE_IDX = 20;
static const int KB_NUMBER_ABC_IDX = 21;
static const int KB_NUMBER_SPACE_IDX = 22;
static const int KB_NUMBER_SEND_IDX = 23;

enum KbMode { KB_MODE_LOWER, KB_MODE_UPPER, KB_MODE_NUMBER };
static KbMode s_kbMode = KB_MODE_LOWER;

static int s_cycleLastIdx = -1;
static int s_cyclePos = 0;
static uint32_t s_cycleLastMs = 0;

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
static lv_obj_t *s_pwKb = nullptr; // custom lv_btnmatrix - see the keyboard comment block above
static String s_setupSsid = "";

// "Server" tab - same screen, same keyboard, different target textarea.
// s_activeTa is whichever of s_pwTa/s_serverTa the keyboard is currently
// writing into; kept as its own pointer (updated by onTabToggle() and
// showPasswordView()) so onPwKeypad() doesn't need to know about tabs at
// all beyond which save/connect action Send should trigger.
static bool s_onServerTab = false;
static lv_obj_t *s_serverTa = nullptr;
static lv_obj_t *s_tabBtn = nullptr;
static lv_obj_t *s_pwBackBtn = nullptr;
static lv_obj_t *s_activeTa = nullptr;

static bool s_setupRequested = false;
static bool s_setupDone = false;
static bool s_setupConnected = false;

void AndonWifi::requestSetup() { s_setupRequested = true; }
bool AndonWifi::isSetupRequested() { return s_setupRequested; }
void AndonWifi::clearSetupRequest() { s_setupRequested = false; }

static void showListView();
static void showPasswordView(const String &ssid);
static void applyKbMap();

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
  lv_obj_add_flag(s_pwBackBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pwKb, LV_OBJ_FLAG_HIDDEN);
}

// Shows whichever of s_pwTa/s_serverTa matches s_onServerTab, updates the
// title and the tab button's own label to name the OTHER tab (what tapping
// it switches to), and points s_activeTa at the now-visible field so
// onPwKeypad() doesn't need to know about tabs at all.
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
  s_kbMode = KB_MODE_LOWER; // reset shift/number-mode state from any previous visit
  applyKbMap();
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
  lv_obj_clear_flag(s_pwBackBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_pwKb, LV_OBJ_FLAG_HIDDEN);
}

static void onBackToList(lv_event_t *e) { showListView(); }

// Send (WiFi tab) / Save (Server tab) - shared by both letter mode's
// KB_SEND_IDX and number mode's KB_NUMBER_SEND_IDX (see onPwKeypad()).
static void triggerSendOrSave() {
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

// Swaps s_pwKb's map to match s_kbMode and invalidates any in-progress
// multi-tap cycle (switching maps mid-cycle would otherwise delete/replace
// a character on a now-different key - see onPwKeypad()'s cycle handling).
static void applyKbMap() {
  const char **map = (s_kbMode == KB_MODE_NUMBER) ? wifiKbMapNumber
                    : (s_kbMode == KB_MODE_UPPER) ? wifiKbMapUpper
                    : wifiKbMapLower;
  lv_btnmatrix_set_map(s_pwKb, map);
  s_cycleLastIdx = -1;
}

static void onPwKeypad(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *btnm = lv_event_get_target(e);
  uint16_t idx = lv_btnmatrix_get_selected_btn(btnm);
  if (idx == LV_BTNMATRIX_BTN_NONE) return;

  if (s_kbMode == KB_MODE_NUMBER) {
    if (idx == KB_NUMBER_BACKSPACE_IDX) { lv_textarea_del_char(s_activeTa); return; }
    if (idx == KB_NUMBER_ABC_IDX) { s_kbMode = KB_MODE_LOWER; applyKbMap(); return; }
    if (idx == KB_NUMBER_SPACE_IDX) { lv_textarea_add_char(s_activeTa, ' '); return; }
    if (idx == KB_NUMBER_SEND_IDX) { triggerSendOrSave(); return; }
    // Every other cell in number mode is a direct single-char insert - no
    // cycling, see the "no grouping here" comment on wifiKbMapNumber.
    const char *txt = lv_btnmatrix_get_btn_text(btnm, idx);
    if (txt && txt[0]) lv_textarea_add_char(s_activeTa, txt[0]);
    return;
  }

  // Letter modes (LOWER/UPPER) share the same index layout.
  if (idx == KB_SHIFT_IDX) {
    s_kbMode = (s_kbMode == KB_MODE_UPPER) ? KB_MODE_LOWER : KB_MODE_UPPER;
    applyKbMap();
    return;
  }
  if (idx == KB_LETTER_BACKSPACE_IDX) {
    lv_textarea_del_char(s_activeTa);
    s_cycleLastIdx = -1;
    return;
  }
  if (idx == KB_MODE_IDX) {
    s_kbMode = KB_MODE_NUMBER;
    applyKbMap();
    return;
  }
  if (idx == KB_SPACE_IDX) {
    lv_textarea_add_char(s_activeTa, ' ');
    s_cycleLastIdx = -1;
    return;
  }
  if (idx == KB_SEND_IDX) {
    triggerSendOrSave();
    return;
  }

  // Ordinary letter or one of the two grouped keys ([j/k], [z/x/v]) -
  // same repeated-tap-cycles-through-the-cell's-characters mechanism the
  // original T9 grid used, just with per-key cycle length 1-3 instead of
  // always the full phone-pad group.
  const char *cycle = wifiKbCycleLower[idx];
  if (!cycle) return;
  int cycleLen = (int)strlen(cycle);

  uint32_t now = millis();
  bool cyclingSameKey = (idx == s_cycleLastIdx) && (now - s_cycleLastMs < KEY_CYCLE_TIMEOUT_MS);
  if (cyclingSameKey) {
    lv_textarea_del_char(s_activeTa);
    s_cyclePos = (s_cyclePos + 1) % cycleLen;
  } else {
    s_cyclePos = 0;
  }

  char ch = cycle[s_cyclePos];
  if (s_kbMode == KB_MODE_UPPER && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
  lv_textarea_add_char(s_activeTa, ch);
  s_cycleLastIdx = idx;
  s_cycleLastMs = now;
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

  // Two small buttons side by side (was a 3-stacked column: Symbols/Back/
  // Tab) - the custom keyboard's own 123/Send cells (and Shift for case)
  // cover what Symbols used to, so only Back and the WiFi<->Server tab
  // switch still need dedicated widgets. Both sit in the same y=113
  // dead-zone-safe row as the keyboard's row 1 starts just below them -
  // freeing the rest of that column's width is the actual point of this
  // experiment, not just fitting one more button.
  const int32_t PW_BTN_W = 70, PW_BTN_H = 36;
  s_pwBackBtn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(s_pwBackBtn, PW_BTN_W, PW_BTN_H);
  lv_obj_align(s_pwBackBtn, LV_ALIGN_TOP_RIGHT, -86, 113);
  lv_obj_add_event_cb(s_pwBackBtn, onBackToList, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(s_pwBackBtn, 12, 0);
  lv_obj_set_style_bg_color(s_pwBackBtn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(s_pwBackBtn, 1, 0);
  lv_obj_set_style_border_color(s_pwBackBtn, lv_color_hex(0x22c55e), 0);
  lv_obj_t *backLabel = lv_label_create(s_pwBackBtn);
  lv_label_set_text(backLabel, LV_SYMBOL_LEFT " Back");
  lv_obj_set_style_text_font(backLabel, &lv_font_montserrat_12, 0);
  lv_obj_center(backLabel);

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

  // Custom big-key keyboard - nearly full content width (460 of 480),
  // starting right below the Back/Tab row (both start at the same y=113
  // dead-zone-safe line - see the CONTENT_W/MARGIN/GAP comment convention
  // this firmware otherwise uses in main.cpp). See the keyboard comment
  // block near the top of this file for the row/grouping design and
  // applyKbMap() for how s_kbMode picks which of the three maps is active.
  s_pwKb = lv_btnmatrix_create(lv_scr_act());
  applyKbMap();
  lv_obj_add_event_cb(s_pwKb, onPwKeypad, LV_EVENT_VALUE_CHANGED, NULL);
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
