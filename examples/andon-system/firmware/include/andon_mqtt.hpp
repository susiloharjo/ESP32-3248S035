#pragma once

#include <Arduino.h>

// MQTT client for submitting an Andon request to the backend - the actual
// wire-up for main.cpp's submitRequest(), which used to be pure local mock
// state (see its old TODO(backend) comment). Topic/envelope shape from
// architectur.md §8 (topic convention, event envelope, delivery semantics)
// - see contracts/ for the config-fetch HTTP contract's equivalent; this
// doesn't have its own contracts/ doc yet (see the SCOPE note below).
//
// SCOPE - this is the minimal slice needed to test Send Request end-to-end
// against backend/'s test server (see that package's own scope note), NOT
// the full agents.md §9 firmware reliability story:
//  - One synchronous attempt per tap: connect, publish QoS 1, wait
//    (bounded) for the COMMAND_RESULT. No persisted offline queue, no
//    retry-with-backoff, no idempotency key surviving a reboot (agents.md
//    §9 requires all of these for production - "Retain the same
//    idempotency key across retries and reboot", "bounded exponential
//    backoff with jitter"). A failed/timed-out attempt currently just
//    reports failure - main.cpp's SCR-04B QueuedOffline path isn't wired
//    to this yet.
//  - No TLS, no device credentials/ACLs (agents.md §12) - matches
//    backend/'s own local-test-only scope for now.
namespace AndonMqtt {

// Connects (if not already connected) to the broker configured in
// secrets.h, publishes one ANDON_REQUESTED event, and blocks (bounded,
// ~5s) waiting for the backend's COMMAND_RESULT. Returns true only if an
// ACCEPTED result was actually received back - never reports success just
// because the publish call itself didn't error (agents.md: "Never report
// a locally queued request as backend accepted or responder notified").
// On success, outIncidentId receives the backend-assigned incident id.
// Safe to call directly from an LVGL event callback (blocks via delay(),
// same as andon_wifi.cpp's tryConnectWifi() - does not call
// lv_timer_handler() itself, so it isn't the reentrancy hazard
// AndonWifi::runSetupFlow() is - see that header's comment).
bool submitAndonRequest(const char *categoryCode, const char *reasonCode,
                         const char *workOrderId, String &outIncidentId);

// Same connect/publish/wait-for-result mechanics as submitAndonRequest(),
// but for main.cpp's onProductionConfirm() instead. eventType
// PRODUCTION_COUNT_UPDATED is NOT in architectur.md SS8 or any other
// governing doc - this screen was already flagged as new scope beyond
// design.md's numbered screens (see main.cpp's showScreenUpdateProduction()
// comment), so this reuses the existing device event topic/envelope/
// idempotency convention rather than inventing a new topic, but is
// otherwise an ad-hoc extension both here and in backend/'s handler.
// Returns true only once the backend actually acks it (same "never claim
// accepted without proof" rule as submitAndonRequest()).
bool submitProductionUpdate(int productionCount, const char *workOrderId);

} // namespace AndonMqtt
