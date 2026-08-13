#include "andon_workorders.hpp"
#include "andon_wifi.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "secrets.h" // CONFIG_API_BASE_URL, STATION_ID

// Bounded storage - see andon_config.cpp's ANDON_CFG_* comment for the same
// reasoning (agents.md: "bound queues, strings, payloads"; no heap/String
// backing what the UI reads every frame).
#define ANDON_WO_MAX        8
#define ANDON_WO_ID_LEN     24
#define ANDON_WO_PRODUCT_LEN 40

static char s_id[ANDON_WO_MAX][ANDON_WO_ID_LEN];
static char s_product[ANDON_WO_MAX][ANDON_WO_PRODUCT_LEN];
static int s_target[ANDON_WO_MAX];
static int s_productionCount[ANDON_WO_MAX]; // see productionCount()/setProductionCount()
static int s_count = 0;
static int s_selectedIdx = -1;

static Preferences s_prefs;
// Separate namespace from s_prefs's "andon_wo" (list/selection) - keyed by
// each work order's own id (fits NVS's 15-char key limit - "WO-240811-07"
// is 12 - see productionCount()'s comment for why id, not index).
static Preferences s_countPrefs;

// Single hardcoded fallback so the terminal is still demoable on a first
// boot with no cache and no network - same target/id this firmware used
// before this module existed (main.cpp's old literal "WO-240811-07").
static void applyPlaceholder() {
  strlcpy(s_id[0], "WO-240811-07", ANDON_WO_ID_LEN);
  strlcpy(s_product[0], "Unknown product", ANDON_WO_PRODUCT_LEN);
  s_target[0] = 120;
  s_count = 1;
}

// Parses {"workOrders":[{"workOrderId":"...","product":"...","target":N}]}
// into s_id/s_product/s_target. Returns true if at least one entry was
// applied (empty/malformed array is not treated as a hard failure - see
// andon_config.cpp's applyConfig() for the same convention).
static bool applyWorkOrders(JsonDocument &doc) {
  JsonArray arr = doc["workOrders"].as<JsonArray>();
  if (arr.isNull()) {
    Serial.println("AndonWorkOrders: response has no 'workOrders' array - ignoring");
    return false;
  }

  int n = 0;
  for (JsonObject wo : arr) {
    if (n >= ANDON_WO_MAX) {
      Serial.printf("AndonWorkOrders: more than %d work orders - truncating\r\n", ANDON_WO_MAX);
      break;
    }
    const char *id = wo["workOrderId"] | "";
    if (id[0] == '\0') continue;
    strlcpy(s_id[n], id, ANDON_WO_ID_LEN);
    strlcpy(s_product[n], wo["product"] | "", ANDON_WO_PRODUCT_LEN);
    s_target[n] = wo["target"] | 0;
    n++;
  }

  if (n == 0) return false;
  s_count = n;
  return true;
}

// Same shape as andon_config.cpp's fetchFromNetwork() - bounded WiFi
// connect (delegated to AndonWifi) + one HTTP GET, once at boot.
static bool fetchFromNetwork(String &outRawJson) {
  if (WiFi.status() != WL_CONNECTED) {
    if (!AndonWifi::connectSavedOrFallback(nullptr)) {
      Serial.println("AndonWorkOrders: no WiFi available - staying on cached/placeholder list");
      return false;
    }
  }

  String host = AndonWifi::getServerHost();
  String baseUrl = host.length() > 0 ? ("http://" + host + ":8080") : String(CONFIG_API_BASE_URL);
  String url = baseUrl + "/api/v1/work-orders/stations/" + STATION_ID;
  Serial.printf("AndonWorkOrders: GET %s\r\n", url.c_str());

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("AndonWorkOrders: HTTP GET failed (code %d) - staying on cached/placeholder list\r\n", code);
    http.end();
    return false;
  }
  outRawJson = http.getString();
  http.end();
  return true;
}

// Restores the operator's last selection (NVS) if it's still present in
// the current list; otherwise defaults to index 0.
static void restoreSelection() {
  if (s_count == 0) {
    s_selectedIdx = -1;
    return;
  }
  s_prefs.begin("andon_wo", true);
  String lastId = s_prefs.getString("selected_id", "");
  s_prefs.end();

  if (lastId.length() > 0) {
    for (int i = 0; i < s_count; i++) {
      if (lastId == s_id[i]) {
        s_selectedIdx = i;
        return;
      }
    }
  }
  s_selectedIdx = 0;
}

// Loads each currently-known work order's own persisted count (0 if never
// set - a work order this device has never actually updated). Must run
// after s_id[]/s_count are finalized for this sync() pass.
static void loadProductionCounts() {
  s_countPrefs.begin("andon_woc", true);
  for (int i = 0; i < s_count; i++) {
    s_productionCount[i] = s_countPrefs.getInt(s_id[i], 0);
  }
  s_countPrefs.end();
}

void AndonWorkOrders::sync() {
  // 1. Cached list first, so the picker works offline after the first
  // successful sync, across reboots - same rationale as AndonConfig::sync().
  s_prefs.begin("andon_wo", true);
  String cached = s_prefs.getString("list_json", "");
  s_prefs.end();

  bool haveAny = false;
  if (cached.length() > 0) {
    JsonDocument cachedDoc; // ArduinoJson v7 - self-sizing
    if (deserializeJson(cachedDoc, cached) == DeserializationError::Ok) {
      if (applyWorkOrders(cachedDoc)) {
        Serial.println("AndonWorkOrders: applied cached list from NVS");
        haveAny = true;
      }
    } else {
      Serial.println("AndonWorkOrders: cached list in NVS is corrupt - ignoring");
    }
  }

  if (!haveAny) applyPlaceholder(); // keeps the terminal demoable either way

  // 2. Best-effort live refresh - overwrites both the in-memory list and
  // the cache on success. Every failure mode below leaves step 1's result
  // exactly as it already was.
  String rawJson;
  if (fetchFromNetwork(rawJson)) {
    JsonDocument doc; // ArduinoJson v7 - self-sizing
    DeserializationError err = deserializeJson(doc, rawJson);
    if (err) {
      Serial.printf("AndonWorkOrders: JSON parse failed (%s) - keeping previous list\r\n", err.c_str());
    } else if (applyWorkOrders(doc)) {
      Serial.println("AndonWorkOrders: applied live list from backend");
      s_prefs.begin("andon_wo", false);
      s_prefs.putString("list_json", rawJson);
      s_prefs.end();
    } else {
      Serial.println("AndonWorkOrders: live response had no usable work orders - keeping previous list");
    }
  }

  restoreSelection();
  loadProductionCounts();
}

int AndonWorkOrders::count() { return s_count; }

const char *AndonWorkOrders::workOrderId(int idx) {
  if (idx < 0 || idx >= s_count) return "";
  return s_id[idx];
}

const char *AndonWorkOrders::product(int idx) {
  if (idx < 0 || idx >= s_count) return "";
  return s_product[idx];
}

int AndonWorkOrders::target(int idx) {
  if (idx < 0 || idx >= s_count) return 0;
  return s_target[idx];
}

int AndonWorkOrders::selectedIndex() { return s_selectedIdx; }

void AndonWorkOrders::select(int idx) {
  if (idx < 0 || idx >= s_count) return;
  s_selectedIdx = idx;
  s_prefs.begin("andon_wo", false);
  s_prefs.putString("selected_id", s_id[idx]);
  s_prefs.end();
}

int AndonWorkOrders::productionCount(int idx) {
  if (idx < 0 || idx >= s_count) return 0;
  return s_productionCount[idx];
}

void AndonWorkOrders::setProductionCount(int idx, int count) {
  if (idx < 0 || idx >= s_count) return;
  s_productionCount[idx] = count;
  s_countPrefs.begin("andon_woc", false);
  s_countPrefs.putInt(s_id[idx], count);
  s_countPrefs.end();
}
