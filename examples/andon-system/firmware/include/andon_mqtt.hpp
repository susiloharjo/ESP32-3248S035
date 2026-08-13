#pragma once

#include <Arduino.h>

// MQTT client for the backend - publishes Send Request/production updates
// (the wire-up for main.cpp's submitRequest(), which used to be pure local
// mock state) and, as of poll()/hasStateUpdate() below, subscribes to
// receive incident state pushed back from the dashboard (Acknowledge/Start
// Handling/Resolve), replacing what used to be a local "(DEMO) SIMULATE
// TECHNICIAN ACK" button. Topic/envelope shape from architectur.md §8
// (topic convention, event envelope, delivery semantics) for the publish
// side; the device-state topic/shape is provisional (see
// dashboard/PLAN.md §8) since it isn't in architectur.md at all yet - see
// contracts/ for the config-fetch HTTP contract's equivalent, this doesn't
// have its own contracts/ doc yet.
//
// SCOPE - this is the minimal slice needed to test Send Request +
// dashboard acknowledge/resolve end-to-end against backend/'s test server
// (see that package's own scope note), NOT the full agents.md §9 firmware
// reliability story:
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
//
// rejectCount added 2026-08-13 (OEE Quality tracking - see
// andon_workorders.hpp's rejectCount()) - reuses this same event/payload
// rather than a separate one, same "don't invent a new topic for every
// new field" convention PRODUCTION_COUNT_UPDATED itself already set.
bool submitProductionUpdate(int productionCount, int rejectCount, const char *workOrderId);

// Device-initiated Start Handling / Resolve - the counterpart to
// hasStateUpdate()'s incoming Acknowledge push. Deliberately NOT symmetric
// with that push: Acknowledge is the one transition the dashboard can
// still drive remotely (see backend/src/server.ts), but Start Handling and
// Resolve are restricted to the device on purpose - a technician has to be
// physically at the terminal to advance past Acknowledged (product
// decision, 2026-08-13). status must be "HANDLING" or "RESOLVED". Same
// blocking/wait-for-COMMAND_RESULT contract as submitAndonRequest() -
// returns true only once the backend actually applied the transition.
bool submitStatusUpdate(const char *incidentId, const char *status);

// Services the MQTT connection - keeps it alive (reconnecting as needed)
// and processes incoming messages via PubSubClient::loop(). Call every
// loop() cycle, top-level only, same as AndonWifi's flag-consuming
// pattern - this module doesn't call lv_timer_handler() itself so it
// isn't actually reentrancy-hazardous the way AndonWifi::runSetupFlow()
// is, but keeping every MQTT/LVGL touch point at loop()'s top level keeps
// that invariant simple to reason about instead of case-by-case.
void poll();

// True once a real INCIDENT_STATE_CHANGED push has arrived and not yet
// been consumed (see backend/src/server.ts's publishDeviceState(), fired
// by the dashboard's Acknowledge/Start Handling/Resolve actions - see
// examples/andon-system/dashboard/). This is what replaced main.cpp's old
// "(DEMO) SIMULATE TECHNICIAN ACK" button.
bool hasStateUpdate();

// Reads the pending update (status is "ACKNOWLEDGED"/"HANDLING"/
// "RESOLVED" - matches backend's IncidentStatus) and clears the flag.
// This module doesn't know about AndonState/screens - matching the
// incidentId against whatever's currently open is the caller's job.
void consumeStateUpdate(String &incidentId, String &status);

} // namespace AndonMqtt
