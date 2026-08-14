#pragma once

#include <Arduino.h>

// Persisted retry queue for MQTT events that couldn't be delivered right
// away (no WiFi, broker unreachable, or a COMMAND_RESULT timeout) -
// closes the gap andon_mqtt.hpp's SCOPE note has flagged since this
// firmware's MQTT integration first landed: "no persisted offline queue,
// no retry-with-backoff, no idempotency key surviving a reboot" (agents.md
// §9 requires all three for production).
//
// This module only owns PERSISTENCE (an SD-backed FIFO of already-fully-
// serialized (topic, payload) pairs) - it doesn't know how to publish or
// what a COMMAND_RESULT looks like. AndonMqtt (andon_mqtt.cpp) owns the
// actual resend attempt, using its own existing MQTT client/publish-and-
// wait mechanics, and calls enqueue()/peekFront()/removeFront() here to
// drive it. Because the exact envelope a failed submitAndonRequest()/
// submitProductionUpdate() call already built (idempotencyKey, eventId,
// correlationId and all) is what gets queued unchanged, a later retry -
// even after a reboot - reuses that same idempotency key automatically,
// satisfying agents.md §9's "retain the same idempotency key across
// retries and reboot" without any extra bookkeeping.
//
// Backed by the microSD card (see main.cpp's testSdCard() for the
// SPIClass(HSPI)-on-TF-pins recipe this reuses, and the pin/init lessons
// learned getting it working). If no card is present/working, every
// function here safely no-ops - queueing is best-effort, not required for
// the terminal to function, matching AndonConfig/AndonWorkOrders' "never
// block the operator flow" philosophy elsewhere in this firmware.
namespace AndonOfflineQueue {

// Call once from setup(), right after the SD card's own init (see
// main.cpp's testSdCard()) - just remembers whether SD is usable. Doesn't
// touch the card/filesystem itself.
void begin(bool sdReady);

// Appends one (topic, payload) pair - AndonMqtt passes the *exact* string
// its own publishEventAndAwaitResult() already built for the attempt that
// just failed to deliver, unmodified (see this file's own header comment
// on why that matters for idempotency). Bounded (ANDON_QUEUE_MAX entries,
// agents.md: "bound queues, strings, payloads") - drops the OLDEST queued
// entry to make room for a new one rather than refusing it, logged
// either way. A no-op (silently) if SD isn't usable.
void enqueue(const String &topic, const String &payload);

// Number of entries currently queued (0 if SD isn't usable or the queue
// file doesn't exist yet).
int count();

// FIFO peek - reads the oldest queued entry without removing it, so the
// caller can attempt to resend it first and only call removeFront() once
// that attempt actually got a definitive answer (matches every other
// "never claim accepted without proof" contract in this firmware).
// Returns false (out-params left untouched) if the queue is empty or SD
// isn't usable.
bool peekFront(String &outTopic, String &outPayload);

// Removes the oldest entry - call after peekFront()'s entry got ANY
// COMMAND_RESULT back (ACCEPTED or REJECTED are both definitive answers;
// only "never got a result at all" should leave an entry queued).
void removeFront();

} // namespace AndonOfflineQueue
