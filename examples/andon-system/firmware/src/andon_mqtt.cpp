#include "andon_mqtt.hpp"
#include "andon_wifi.hpp"

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
  s_mqtt.subscribe(resultTopic.c_str());
  Serial.printf("AndonMqtt: connected, subscribed to %s\r\n", resultTopic.c_str());
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
static bool publishEventAndAwaitResult(const char *eventType, const String &innerPayloadJson) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("AndonMqtt: no WiFi - can't submit");
    return false;
  }
  if (!ensureConnected()) return false;

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
    return false;
  }

  uint32_t start = millis();
  while (!s_resultReceived && millis() - start < 5000) {
    s_mqtt.loop();
    delay(20);
  }

  if (!s_resultReceived) {
    Serial.println("AndonMqtt: timed out waiting for COMMAND_RESULT");
    return false;
  }

  Serial.printf("AndonMqtt: result status=%s incidentId=%s\r\n",
                s_resultStatus.c_str(), s_resultIncidentId.c_str());
  return s_resultStatus == "ACCEPTED";
}

bool AndonMqtt::submitAndonRequest(const char *categoryCode, const char *reasonCode,
                                    const char *workOrderId, String &outIncidentId) {
  String innerPayload = String("\"categoryCode\":\"") + categoryCode + "\"," +
                         "\"reasonCode\":\"" + reasonCode + "\"," +
                         "\"workOrderId\":\"" + workOrderId + "\"";
  if (!publishEventAndAwaitResult("ANDON_REQUESTED", innerPayload)) return false;
  outIncidentId = s_resultIncidentId;
  return true;
}

bool AndonMqtt::submitProductionUpdate(int productionCount, const char *workOrderId) {
  String innerPayload = String("\"productionCount\":") + String(productionCount) + "," +
                         "\"workOrderId\":\"" + workOrderId + "\"";
  return publishEventAndAwaitResult("PRODUCTION_COUNT_UPDATED", innerPayload);
}
