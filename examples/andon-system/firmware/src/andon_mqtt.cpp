#include "andon_mqtt.hpp"
#include "andon_wifi.hpp"
#include "andon_offlinequeue.hpp" // persisted retry queue - see publishEventAndAwaitResult()/flushOfflineQueue()

#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>

#include "secrets.h" // MQTT_BROKER_HOST, MQTT_BROKER_PORT, PLANT_ID, STATION_ID

static WiFiClient s_wifiClient;
static PubSubClient s_mqtt(s_wifiClient);

// PubSubClient::setServer(const char*, ...) stores the raw pointer, not a
// copy - a local/temporary String's c_str() would dangle the moment
// ensureConnected() returns. File-scope keeps it alive for the program's
// lifetime instead.
static String s_brokerHostBuf;

static String s_deviceId;
static volatile bool s_resultReceived = false;
static String s_resultIncidentId;
static String s_resultStatus;
static String s_waitingCorrelationId;

// Incoming state pushes (dashboard Acknowledge/Start Handling/Resolve) -
// see andon_mqtt.hpp's hasStateUpdate()/consumeStateUpdate().
static volatile bool s_stateUpdatePending = false;
static String s_stateIncidentId;
static String s_stateStatus;

static uint32_t s_lastConnectAttemptMs = 0;

// "esp32-<mac-no-colons>" - stable across reboots (derived from the radio's
// MAC), matches architectur.md's "esp32-042"-style deviceId examples
// closely enough for this test harness without needing a separate
// provisioned identity.
static const String &deviceId() {
  if (s_deviceId.length() == 0) {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    s_deviceId = "esp32-" + mac;
  }
  return s_deviceId;
}

// Hand-rolled substring parse instead of pulling ArduinoJson into this
// module for two fields - fragile against a hand-written backend response
// reordering/reformatting its JSON, but fine against backend/'s fixed
// serialization (see that package's src/server.ts). Revisit with real JSON
// parsing if this module ever needs more than correlationId/status/
// incidentId out of the result.
static bool extractField(const String &body, const char *key, String &out) {
  String needle = String("\"") + key + "\":\"";
  int start = body.indexOf(needle);
  if (start < 0) return false;
  start += needle.length();
  int end = body.indexOf('"', start);
  if (end < 0) return false;
  out = body.substring(start, end);
  return true;
}

static void mqttCallback(char *topic, uint8_t *payload, unsigned int length) {
  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];

  String topicStr(topic);
  if (topicStr.endsWith("/state")) {
    // Dashboard-driven push (Acknowledge/Start Handling/Resolve) - see
    // andon_mqtt.hpp's poll()/hasStateUpdate(). Overwrites any not-yet-
    // consumed update rather than queueing (single-incident-at-a-time
    // model, same as the rest of this firmware - see g_andon in main.cpp).
    extractField(body, "incidentId", s_stateIncidentId);
    extractField(body, "status", s_stateStatus);
    s_stateUpdatePending = true;
    return;
  }

  // Otherwise assume the .../result topic - a COMMAND_RESULT reply to
  // whichever publishEventAndAwaitResult() call is currently waiting.
  String correlationId;
  if (!extractField(body, "correlationId", correlationId)) return;
  if (correlationId != s_waitingCorrelationId) return; // not the result we're waiting for

  extractField(body, "status", s_resultStatus);
  extractField(body, "incidentId", s_resultIncidentId);
  s_resultReceived = true;
}

static bool ensureConnected() {
  if (s_mqtt.connected()) return true;
  // PubSubClient's default MQTT_MAX_PACKET_SIZE is only 128 bytes - way
  // under the ~250-300 byte event envelope this module publishes (full
  // JSON payload + a ~75-byte topic string) - publish() silently returns
  // false once over that limit. Must be called before connect(), not just
  // before publish() (it sizes an internal buffer the socket read/write
  // path uses from connection time).
  s_mqtt.setBufferSize(512);

  // Same "Server" tab / NVS host AndonConfig::sync() uses - see
  // andon_wifi.hpp's getServerHost(). Falls back to secrets.h's
  // MQTT_BROKER_HOST if nothing's been entered on-device yet.
  s_brokerHostBuf = AndonWifi::getServerHost();
  const char *brokerHost = s_brokerHostBuf.length() > 0 ? s_brokerHostBuf.c_str() : MQTT_BROKER_HOST;
  s_mqtt.setServer(brokerHost, MQTT_BROKER_PORT);
  s_mqtt.setCallback(mqttCallback);
  Serial.printf("AndonMqtt: connecting to %s:%d...\r\n", brokerHost, MQTT_BROKER_PORT);
  if (!s_mqtt.connect(deviceId().c_str())) {
    Serial.printf("AndonMqtt: connect failed (state=%d)\r\n", s_mqtt.state());
    return false;
  }
  String resultTopic = "andon/v1/device/" + deviceId() + "/result";
  String stateTopic = "andon/v1/device/" + deviceId() + "/state";
  s_mqtt.subscribe(resultTopic.c_str());
  s_mqtt.subscribe(stateTopic.c_str());
  Serial.printf("AndonMqtt: connected, subscribed to %s and %s\r\n", resultTopic.c_str(), stateTopic.c_str());
  return true;
}

// Shared by submitAndonRequest() and submitProductionUpdate(): wraps
// innerPayloadJson (just the event-specific "payload" object body, e.g.
// `"categoryCode":"...","reasonCode":"..."`) in the standard envelope
// (architectur.md SS8.2), publishes it to the device event topic, and
// blocks (bounded ~5s) for the matching COMMAND_RESULT. Returns true only
// once an ACCEPTED result actually comes back - see andon_mqtt.hpp's
// header comment for why that distinction matters (agents.md: never claim
// backend-accepted without proof).
//
// queueOnFailure (2026-08-14): when true, every "never got a definitive
// answer" exit point below (no WiFi, broker unreachable, publish()
// failing, or a COMMAND_RESULT timeout) hands the exact envelope/topic
// just built to AndonOfflineQueue::enqueue() for later automatic retry
// instead of just dropping it - see that module's header comment.
// Deliberately NOT triggered by an explicit REJECTED result (the very
// last line below) - that's a definitive backend answer, not a delivery
// failure, and retrying it would be pointless. Callers that shouldn't
// queue (submitStatusUpdate() - device-only Start Handling/Resolve,
// out of scope for this pass, see AndonOfflineQueue's header) pass false.
static bool publishEventAndAwaitResult(const char *eventType, const String &innerPayloadJson,
                                        bool queueOnFailure) {
  // Built up front so every early-return below can still enqueue it -
  // only WiFi/AndonWifi state is needed before this point, no network
  // round trip has to have happened yet.
  static uint32_t sequence = 0;
  sequence++;
  String eventId = deviceId() + "-evt-" + String(millis()) + "-" + String(sequence);
  String correlationId = eventId; // 1:1 for this simple request/result flow
  // NOTE: not persisted across reboots (agents.md §9 wants that for
  // production) - see andon_mqtt.hpp's SCOPE note.
  String idempotencyKey = deviceId() + ":" + String(millis()) + ":" + String(sequence);

  char deviceTimestamp[32];
  time_t now = time(nullptr);
  struct tm tmInfo;
  gmtime_r(&now, &tmInfo);
  strftime(deviceTimestamp, sizeof(deviceTimestamp), "%Y-%m-%dT%H:%M:%SZ", &tmInfo);

  String payload = String("{") +
    "\"schemaVersion\":1," +
    "\"eventId\":\"" + eventId + "\"," +
    "\"eventType\":\"" + eventType + "\"," +
    "\"idempotencyKey\":\"" + idempotencyKey + "\"," +
    "\"deviceId\":\"" + deviceId() + "\"," +
    "\"stationId\":\"" + STATION_ID + "\"," +
    "\"deviceTimestamp\":\"" + deviceTimestamp + "\"," +
    "\"sequence\":" + String(sequence) + "," +
    "\"correlationId\":\"" + correlationId + "\"," +
    "\"payload\":{" + innerPayloadJson + "}" +
  "}";

  String eventTopic = "andon/v1/plant/" + String(PLANT_ID) + "/station/" + String(STATION_ID) +
                       "/device/" + deviceId() + "/event";

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("AndonMqtt: no WiFi - can't submit");
    if (queueOnFailure) AndonOfflineQueue::enqueue(eventTopic, payload);
    return false;
  }
  if (!ensureConnected()) {
    if (queueOnFailure) AndonOfflineQueue::enqueue(eventTopic, payload);
    return false;
  }

  // architectur.md SS8.3 asks for QoS 1 - PubSubClient (this library) has
  // no PUBACK handling and always sends QoS 0 on the wire regardless of
  // what's requested. Acceptable for this local test harness (see
  // andon_mqtt.hpp's SCOPE note); a production firmware needs a
  // QoS-1-capable client (e.g. a library built on AsyncMqttClient).
  s_resultReceived = false;
  s_waitingCorrelationId = correlationId;
  Serial.printf("AndonMqtt: publishing %s to %s\r\n", eventType, eventTopic.c_str());
  if (!s_mqtt.publish(eventTopic.c_str(), payload.c_str())) {
    Serial.println("AndonMqtt: publish failed");
    if (queueOnFailure) AndonOfflineQueue::enqueue(eventTopic, payload);
    return false;
  }

  uint32_t start = millis();
  while (!s_resultReceived && millis() - start < 5000) {
    s_mqtt.loop();
    delay(20);
  }

  if (!s_resultReceived) {
    Serial.println("AndonMqtt: timed out waiting for COMMAND_RESULT");
    if (queueOnFailure) AndonOfflineQueue::enqueue(eventTopic, payload);
    return false;
  }

  Serial.printf("AndonMqtt: result status=%s incidentId=%s\r\n",
                s_resultStatus.c_str(), s_resultIncidentId.c_str());
  return s_resultStatus == "ACCEPTED"; // REJECTED falls through here too - a definitive answer, never queued
}

bool AndonMqtt::submitAndonRequest(const char *categoryCode, const char *reasonCode,
                                    const char *workOrderId, String &outIncidentId) {
  String innerPayload = String("\"categoryCode\":\"") + categoryCode + "\"," +
                         "\"reasonCode\":\"" + reasonCode + "\"," +
                         "\"workOrderId\":\"" + workOrderId + "\"";
  // queueOnFailure=true - see publishEventAndAwaitResult()'s comment.
  // outIncidentId stays empty on a queued (not-yet-delivered) request;
  // the caller (main.cpp's submitRequest()) already treats "not accepted"
  // as SCR-04B QueuedOffline regardless of the reason, so this doesn't
  // need its own signal for "queued vs. genuinely failed" yet.
  if (!publishEventAndAwaitResult("ANDON_REQUESTED", innerPayload, true)) return false;
  outIncidentId = s_resultIncidentId;
  return true;
}

bool AndonMqtt::submitProductionUpdate(int productionCount, int rejectCount, const char *workOrderId) {
  String innerPayload = String("\"productionCount\":") + String(productionCount) + "," +
                         "\"rejectCount\":" + String(rejectCount) + "," +
                         "\"workOrderId\":\"" + workOrderId + "\"";
  return publishEventAndAwaitResult("PRODUCTION_COUNT_UPDATED", innerPayload, true);
}

bool AndonMqtt::submitStatusUpdate(const char *incidentId, const char *status) {
  // queueOnFailure=false, deliberately - see AndonOfflineQueue's header
  // comment and publishEventAndAwaitResult()'s: Start Handling/Resolve
  // are device-only by product decision (a technician must be physically
  // at the terminal), out of scope for automatic offline retry in this
  // pass.
  String innerPayload = String("\"incidentId\":\"") + incidentId + "\"," +
                         "\"status\":\"" + status + "\"";
  return publishEventAndAwaitResult("INCIDENT_STATUS_UPDATE", innerPayload, false);
}

// Resends the oldest queued entry (AndonOfflineQueue) over the already-
// connected s_mqtt - caller (poll(), below) guarantees connectivity
// before calling this. Only ever handles ONE entry per call (not a
// while-loop draining the whole queue) so a long queue can't block
// poll()/loop() for multiple round trips in a row; the next poll() tick
// picks up wherever this left off. Stops (leaves the entry queued) on
// anything short of a definitive COMMAND_RESULT, same "never claim
// accepted without proof, preserve delivery order" logic
// publishEventAndAwaitResult() itself uses - an ACCEPTED *or* REJECTED
// result both count as definitive here too (see that function's own
// comment on why REJECTED shouldn't cause requeueing).
static void flushOfflineQueueOnce() {
  String topic, payload;
  if (!AndonOfflineQueue::peekFront(topic, payload)) return;

  String correlationId;
  if (!extractField(payload, "correlationId", correlationId)) {
    Serial.println("AndonOfflineQueue: queued entry has no correlationId - dropping corrupt entry");
    AndonOfflineQueue::removeFront();
    return;
  }

  Serial.printf("AndonOfflineQueue: retrying queued publish to %s\r\n", topic.c_str());
  s_resultReceived = false;
  s_waitingCorrelationId = correlationId;
  if (!s_mqtt.publish(topic.c_str(), payload.c_str())) {
    Serial.println("AndonOfflineQueue: publish() failed again - will retry next poll()");
    return; // leave it queued, try again later
  }

  uint32_t start = millis();
  while (!s_resultReceived && millis() - start < 5000) {
    s_mqtt.loop();
    delay(20);
  }
  if (!s_resultReceived) {
    Serial.println("AndonOfflineQueue: timed out again - will retry next poll()");
    return; // leave it queued
  }

  Serial.printf("AndonOfflineQueue: queued entry resolved, result=%s - removing from queue\r\n",
                s_resultStatus.c_str());
  AndonOfflineQueue::removeFront();
}

void AndonMqtt::poll() {
  if (WiFi.status() != WL_CONNECTED) return; // nothing to service without a link

  if (!s_mqtt.connected()) {
    // Throttled - without this, a broker that's down would get a fresh
    // TCP connect attempt every single loop() cycle (every ~10ms), which
    // both wastes cycles and could look like a connection-flood to
    // anything monitoring the broker.
    uint32_t now = millis();
    if (now - s_lastConnectAttemptMs < 3000) return;
    s_lastConnectAttemptMs = now;
    if (!ensureConnected()) return;
  }

  s_mqtt.loop();

  // Own throttle, separate from the reconnect one above - only bother
  // once actually connected, and no more than once every few seconds
  // even then (each attempt can block up to ~5s waiting for a result, so
  // this also caps how often loop() can stall for that long).
  //
  // The time check MUST short-circuit before AndonOfflineQueue::count()
  // runs, not after - count() does real SD I/O (opens the queue file),
  // and with an empty/nonexistent queue that's a failed open() every
  // single time. Originally written the other way around (count() first)
  // - on real hardware that meant an SD.open() attempt on every loop()
  // tick (~every 50ms) whenever MQTT was connected, logging a
  // "does not exist" error from the ESP-IDF VFS layer each time -
  // harmless to correctness but needless SD thrashing and log spam.
  static uint32_t lastFlushAttemptMs = 0;
  uint32_t now = millis();
  if (s_mqtt.connected() && now - lastFlushAttemptMs > 8000) {
    lastFlushAttemptMs = now;
    if (AndonOfflineQueue::count() > 0) flushOfflineQueueOnce();
  }
}

bool AndonMqtt::hasStateUpdate() {
  return s_stateUpdatePending;
}

void AndonMqtt::consumeStateUpdate(String &incidentId, String &status) {
  incidentId = s_stateIncidentId;
  status = s_stateStatus;
  s_stateUpdatePending = false;
}
