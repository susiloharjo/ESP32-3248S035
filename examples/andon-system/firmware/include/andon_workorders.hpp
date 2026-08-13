#pragma once

#include <Arduino.h>

// Fetches the list of work orders assigned to this station from the
// backend - a new endpoint (2026-08-13, not in architectur.md/contracts/
// yet, same ad-hoc-extension status as PRODUCTION_COUNT_UPDATED - see
// backend/src/work-order-store.ts's header):
//   GET /api/v1/work-orders/stations/{stationId}
//
// Lets the operator pick which work order the terminal is currently
// producing against (main.cpp's SCR_WORK_ORDER_LIST, reached from SCR-01's
// UPDATE PRODUCTION button) instead of the firmware hardcoding
// "WO-240811-07" everywhere it needs a workOrderId for a submitted event.
//
// Mirrors andon_config.cpp's shape exactly: cached-then-live-refresh,
// every failure path non-fatal and leaves whatever was already known
// (cache, or the single hardcoded placeholder on a first boot with no
// cache and no network) - the operator flow never blocks on or breaks
// because of this module.
namespace AndonWorkOrders {

// Call once from setup(), after AndonConfig::sync() (reuses the same
// WiFi/server-host plumbing - AndonWifi::connectSavedOrFallback()/
// getServerHost()). Loads the cached list from NVS first, then attempts a
// live refresh; on success overwrites both the in-memory list and the
// cache. Also restores the last-selected work order (NVS) if it's still
// present in the (possibly refreshed) list, defaulting to index 0
// otherwise. Logs every step/failure to Serial.
void sync();

// Number of work orders currently known (0 only if fetch AND cache AND
// the hardcoded placeholder all somehow produced nothing - shouldn't
// happen in practice, but callers should still check before indexing).
int count();

// 0-indexed accessors - caller must ensure idx is in [0, count()).
const char *workOrderId(int idx);
const char *product(int idx);
int target(int idx);

// Index of the operator's current selection, restored across reboots via
// NVS - see sync()'s comment. -1 only if count() == 0.
int selectedIndex();

// Persists idx as the new selection (NVS) - call when the operator taps a
// row in main.cpp's SCR_WORK_ORDER_LIST.
void select(int idx);

} // namespace AndonWorkOrders
