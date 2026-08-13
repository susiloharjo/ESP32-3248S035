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
// The server's own count, not a device-owned value - see
// productionCount()/setProductionCount()'s comments for why this changed
// (2026-08-13) from its own separately-NVS-persisted-per-id storage to
// just another field parsed off the same cached/live-fetched JSON as
// product/target.
static int s_productionCount[ANDON_WO_MAX];
static int s_count = 0;
static int s_selectedIdx = -1;

static Preferences s_prefs;

// Single hardcoded fallback so the terminal is still demoable on a first
// boot with no cache and no network - same target/id this firmware used
// before this module existed (main.cpp's old literal "WO-240811-07").
static void applyPlaceholder() {
  strlcpy(s_id[0], "WO-240811-07", ANDON_WO_ID_LEN);
  strlcpy(s_product[0], "Unknown product", ANDON_WO_PRODUCT_LEN);
  s_target[0] = 120;
  s_productionCount[0] = 0;
  s_count = 1;
}

// Parses {"workOrders":[{"workOrderId":"...","product":"...","target":N,
// "productionCount":N}]} into s_id/s_product/s_target/s_productionCount.
// Returns true if at least one entry was applied (empty/malformed array
// is not treated as a hard failure - see andon_config.cpp's applyConfig()
// for the same convention).
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
    s_productionCount[n] = wo["productionCount"] | 0; // absent (cached pre-2026-08-13 JSON) -> 0
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

// Called once at boot (setup()) AND every time the picker screen opens
// (main.cpp's onOpenUpdateProduction(), 2026-08-13) - not just at boot
// like AndonConfig::sync() - specifically so productionCount (see that
// field's comment above) reflects the server's own number at the moment
// the operator is about to act on it, not whatever this device last
// happened to fetch. Safe to call from an LVGL event callback the same
// way AndonMqtt's blocking calls already are (bounded HTTP timeout, no
// internal lv_timer_handler() call - see andon_wifi.hpp's reentrancy
// note for the one pattern that actually isn't safe here).
void AndonWorkOrders::sync() {
  // 1. Cached list first (from the last successful live fetch below, not
  // necessarily THIS station's absolute latest - see this function's own
  // comment), so the picker still works offline after at least one prior
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

// In-memory only (no NVS write, unlike select()) - the server is the
// durable source of truth for this value now (see the type's comment
// above and sync()'s doc comment), not this device. This just keeps the
// picker showing the operator's own just-confirmed number immediately,
// without waiting for the next sync() round trip to reflect back what
// AndonMqtt::submitProductionUpdate() (called right alongside this, see
// main.cpp's onProductionConfirm()) presumably just told the backend.
void AndonWorkOrders::setProductionCount(int idx, int count) {
  if (idx < 0 || idx >= s_count) return;
  s_productionCount[idx] = count;
}
