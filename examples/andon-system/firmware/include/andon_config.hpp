#pragma once

// Fetches per-category assistance REASONS from the backend's station
// configuration endpoint - architectur.md's
//   GET /api/v1/configuration/stations/{stationId}
// (see contracts/http/v1/get-configuration-stations.md for the exact
// request/response shape) - and applies them onto the CATEGORIES[] table
// declared in main.cpp.
//
// PRD.md AND-002 fixes the four categories themselves (identity, icon,
// color) - only AND-003 says their REASON lists are plant/line
// configurable. So this module only ever rewrites CategoryInfo::reasons /
// reasonCount for a matched category; it never touches category count,
// label, icon, or color.
//
// Deliberately isolated from main.cpp's screen/state-machine code (see
// PLAN.md's "Deliberate deviations from agents.md" - this firmware was
// built UI-only with no networking at all; this is the first piece that
// changes that, so it's kept in its own module rather than woven into the
// 1500+ line UI file). Every failure path is non-fatal and leaves
// CATEGORIES[] exactly as it already was - either the original hardcoded
// placeholders (PRD.md AND-003's "invent a plausible placeholder set"
// note) on a first boot with no cache and no network, or whatever was last
// successfully synced/cached otherwise. The operator flow never blocks on
// or breaks because of this.
namespace AndonConfig {

// Call once from setup(), AFTER the UI is already showing (SCR-01), so the
// terminal is usable immediately regardless of network state:
//   1. Loads the last cached config from NVS, if any (works offline after
//      the first successful sync, across reboots).
//   2. If WiFi credentials are configured (secrets.h) and connecting
//      succeeds, fetches the latest config; on success, overwrites both
//      the in-memory CATEGORIES[] reasons and the NVS cache.
// Logs every step (and every failure reason) to Serial.
void sync();

} // namespace AndonConfig
