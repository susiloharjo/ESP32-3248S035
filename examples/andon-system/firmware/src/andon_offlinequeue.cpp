#include "andon_offlinequeue.hpp"

#include <Arduino.h>
#include <SD.h>

// Plain-text format, two lines per entry (topic, then payload), appended
// in order - avoids pulling ArduinoJson into a module that's just moving
// opaque strings around (same "hand-rolled substring parse instead" call
// andon_mqtt.cpp's own header comment already made for a similar reason).
// Safe because the payload is always the compact single-line JSON
// envelope andon_mqtt.cpp builds via string concatenation - it never
// contains an embedded newline.
#define ANDON_QUEUE_PATH "/andon_queue.txt"
#define ANDON_QUEUE_MAX  20

static bool s_sdReady = false;

void AndonOfflineQueue::begin(bool sdReady) {
  s_sdReady = sdReady;
  if (!sdReady) {
    Serial.println("AndonOfflineQueue: SD not ready - offline queueing disabled for this session");
  }
}

// Reads every line of the queue file into memory (bounded by
// ANDON_QUEUE_MAX*2 lines, so this is never unbounded) - shared by
// count()/peekFront()/removeFront() rather than each re-implementing
// their own partial read, since the ESP32 SD library has no
// "just give me line N" primitive and the file is small enough that
// reading it whole is simpler and cheap enough to redo per call.
static int readAllLines(String outLines[], int maxLines) {
  if (!s_sdReady) return 0;
  File f = SD.open(ANDON_QUEUE_PATH, FILE_READ);
  if (!f) return 0; // doesn't exist yet - an empty queue, not an error
  int n = 0;
  while (f.available() && n < maxLines) {
    outLines[n] = f.readStringUntil('\n');
    outLines[n].trim(); // drop the \r a Windows-authored file (or just \r\n line endings) would leave
    n++;
  }
  f.close();
  return n;
}

// Rewrites the queue file from scratch with exactly these lines - SD.open
// with FILE_WRITE APPENDS on this platform (see gemini-chatbot/src/
// main.cpp's sdSaveWifiNetwork() comment for the same gotcha/workaround),
// so a real "replace the contents" needs SD.remove() first, same as that
// file's own rewrite pattern.
static void writeAllLines(const String lines[], int n) {
  if (!s_sdReady) return;
  SD.remove(ANDON_QUEUE_PATH);
  if (n == 0) return; // leave no file at all for an empty queue
  File f = SD.open(ANDON_QUEUE_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("AndonOfflineQueue: couldn't reopen queue file for rewrite - queue entries may be lost");
    return;
  }
  for (int i = 0; i < n; i++) {
    f.println(lines[i]);
  }
  f.close();
}

void AndonOfflineQueue::enqueue(const String &topic, const String &payload) {
  if (!s_sdReady) return;

  String lines[ANDON_QUEUE_MAX * 2];
  int n = readAllLines(lines, ANDON_QUEUE_MAX * 2);
  int entries = n / 2;

  if (entries >= ANDON_QUEUE_MAX) {
    // Drop the oldest entry (first two lines) to make room - agents.md's
    // "bound queues" over "never lose the newest operator action".
    Serial.println("AndonOfflineQueue: queue full - dropping oldest entry to make room");
    for (int i = 0; i < n - 2; i++) lines[i] = lines[i + 2];
    n -= 2;
  }

  lines[n] = topic;
  lines[n + 1] = payload;
  writeAllLines(lines, n + 2);
  Serial.printf("AndonOfflineQueue: queued for retry (now %d entries) - topic=%s\r\n", (n + 2) / 2, topic.c_str());
}

int AndonOfflineQueue::count() {
  String lines[ANDON_QUEUE_MAX * 2];
  int n = readAllLines(lines, ANDON_QUEUE_MAX * 2);
  return n / 2;
}

bool AndonOfflineQueue::peekFront(String &outTopic, String &outPayload) {
  String lines[ANDON_QUEUE_MAX * 2];
  int n = readAllLines(lines, ANDON_QUEUE_MAX * 2);
  if (n < 2) return false;
  outTopic = lines[0];
  outPayload = lines[1];
  return true;
}

void AndonOfflineQueue::removeFront() {
  String lines[ANDON_QUEUE_MAX * 2];
  int n = readAllLines(lines, ANDON_QUEUE_MAX * 2);
  if (n < 2) return;
  for (int i = 0; i < n - 2; i++) lines[i] = lines[i + 2];
  writeAllLines(lines, n - 2);
}
