#include "andon_config.hpp"
#include "andon_types.hpp"
#include "andon_wifi.hpp"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include "secrets.h" // CONFIG_API_BASE_URL, STATION_ID - gitignored, see secrets.example.h (WIFI_SSID/PASSWORD now owned by andon_wifi.cpp)

// Defined in main.cpp - deliberately non-static/non-const there (see the
// comment next to its definition) so this module can rewrite it in place.
extern CategoryInfo CATEGORIES[4];

// Bounded storage for fetched/cached reason labels - CategoryInfo::reasons
// is a raw `const char **`, so whatever it points at has to outlive the
// fetch call. Fixed-size buffers (no heap/String backing the actual
// pointers callers dereference) per agents.md's embedded reliability rules
// ("bound queues, strings, payloads" / "avoid dynamic allocation in
// recurring hot paths" - this isn't a hot path, but the *storage* it
// leaves behind for the UI to read every frame should still be static).
#define ANDON_CFG_MAX_REASONS 8
#define ANDON_CFG_LABEL_LEN   40

static char s_reasonLabel[4][ANDON_CFG_MAX_REASONS][ANDON_CFG_LABEL_LEN];
static const char *s_reasonPtrs[4][ANDON_CFG_MAX_REASONS];
static char s_reasonCode[4][ANDON_CFG_MAX_REASONS][ANDON_CFG_LABEL_LEN];
static const char *s_reasonCodePtrs[4][ANDON_CFG_MAX_REASONS];

static Preferences s_prefs;

// Applies one parsed configuration document onto CATEGORIES[] in place.
// Returns true if at least one category's reasons were updated. Unknown
// category codes and malformed/empty reason lists are skipped (logged),
// not treated as a hard failure - a partially-useful config response still
// gets applied.
static bool applyConfig(JsonDocument &doc) {
  JsonArray categories = doc["categories"].as<JsonArray>();
  if (categories.isNull()) {
    Serial.println("AndonConfig: response has no 'categories' array - ignoring");
    return false;
  }

  bool matchedAny = false;
  for (JsonObject catJson : categories) {
    const char *code = catJson["code"] | "";
    int catIdx = -1;
    for (int i = 0; i < 4; i++) {
      if (strcmp(CATEGORIES[i].label, code) == 0) { catIdx = i; break; }
    }
    if (catIdx < 0) {
      // AND-002: the four categories are fixed - an unrecognized code from
      // the backend is skipped, not added and not an error for the rest
      // of the document.
      Serial.printf("AndonConfig: unknown category code '%s' - skipping\r\n", code);
      continue;
    }

    JsonArray reasons = catJson["reasons"].as<JsonArray>();
    if (reasons.isNull()) continue;

    uint8_t count = 0;
    for (JsonObject reasonJson : reasons) {
      if (count >= ANDON_CFG_MAX_REASONS) {
        Serial.printf("AndonConfig: category '%s' has more than %d reasons - truncating\r\n", code, ANDON_CFG_MAX_REASONS);
        break;
      }
      const char *label = reasonJson["label"] | "";
      if (label[0] == '\0') continue;
      strlcpy(s_reasonLabel[catIdx][count], label, ANDON_CFG_LABEL_LEN);
      s_reasonPtrs[catIdx][count] = s_reasonLabel[catIdx][count];
      // code is optional in the response (contracts doc's Compatibility
      // section) - fall back to the label itself so
      // AndonMqtt::submitAndonRequest() always has *something* non-empty
      // to send as reasonCode, even against a response that only bothered
      // sending labels.
      const char *code = reasonJson["code"] | label;
      strlcpy(s_reasonCode[catIdx][count], code, ANDON_CFG_LABEL_LEN);
      s_reasonCodePtrs[catIdx][count] = s_reasonCode[catIdx][count];
      count++;
    }
    if (count == 0) continue; // don't blank out a category's reasons on a malformed/empty list

    CATEGORIES[catIdx].reasons = s_reasonPtrs[catIdx];
    CATEGORIES[catIdx].reasonCodes = s_reasonCodePtrs[catIdx];
    CATEGORIES[catIdx].reasonCount = count;
    matchedAny = true;
  }
  return matchedAny;
}

// Bounded WiFi connect (delegated to AndonWifi - NVS-saved network first,
// secrets.h as fallback; see andon_wifi.hpp) + one HTTPS/HTTP GET. Returns
// false (with the reason logged) on anything short of a clean 200
// response; never retries or blocks beyond the fixed timeouts involved -
// this runs once at boot (and once more after the WiFi setup screen closes
// - see main.cpp's loop()), not in a recurring loop, and the caller
// already has a perfectly good fallback for "no network right now".
static bool fetchFromNetwork(String &outRawJson) {
  if (WiFi.status() != WL_CONNECTED) {
    if (!AndonWifi::connectSavedOrFallback(nullptr)) {
      Serial.println("AndonConfig: no WiFi available - staying on cached/placeholder reasons");
      return false;
    }
  }

  String url = String(CONFIG_API_BASE_URL) + "/api/v1/configuration/stations/" + STATION_ID;
  Serial.printf("AndonConfig: GET %s\r\n", url.c_str());

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("AndonConfig: HTTP GET failed (code %d) - staying on cached/placeholder reasons\r\n", code);
    http.end();
    return false;
  }
  outRawJson = http.getString();
  http.end();
  return true;
}

void AndonConfig::sync() {
  // 1. Cached config first, so a terminal that boots with no network (or
  // before the first-ever live sync completes) gets the last known-good
  // taxonomy instead of only ever the compiled-in placeholders.
  s_prefs.begin("andon_cfg", true); // read-only
  String cached = s_prefs.getString("reasons_json", "");
  s_prefs.end();
  if (cached.length() > 0) {
    JsonDocument cachedDoc; // ArduinoJson v7 - self-sizing, no fixed capacity needed
    if (deserializeJson(cachedDoc, cached) == DeserializationError::Ok) {
      if (applyConfig(cachedDoc)) {
        Serial.println("AndonConfig: applied cached reasons from NVS");
      }
    } else {
      Serial.println("AndonConfig: cached config in NVS is corrupt - ignoring, keeping placeholders");
    }
  }

  // 2. Best-effort live refresh - overwrites both CATEGORIES[] and the
  // cache on success. Every failure mode below leaves step 1's result (or
  // the original hardcoded placeholders) exactly as it already was.
  String rawJson;
  if (!fetchFromNetwork(rawJson)) return;

  JsonDocument doc; // ArduinoJson v7 - self-sizing, no fixed capacity needed
  DeserializationError err = deserializeJson(doc, rawJson);
  if (err) {
    Serial.printf("AndonConfig: JSON parse failed (%s) - keeping previous reasons\r\n", err.c_str());
    return;
  }

  if (applyConfig(doc)) {
    Serial.println("AndonConfig: applied live config from backend");
    s_prefs.begin("andon_cfg", false);
    s_prefs.putString("reasons_json", rawJson);
    s_prefs.end();
  } else {
    Serial.println("AndonConfig: live response didn't match any known category - keeping previous reasons");
  }
}
