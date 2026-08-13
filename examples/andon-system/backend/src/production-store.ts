// In-memory production-count store, fed by PRODUCTION_COUNT_UPDATED MQTT
// events (see server.ts). Same ad-hoc-extension status as before (not in
// architectur.md §8 or PRD.md - see server.ts's note on that event type).
//
// Keyed by (stationId, workOrderId) - NOT stationId alone (2026-08-13
// revision). A station can have several work orders in flight over a
// shift (see firmware/src/andon_workorders.cpp's picker - the operator
// switches which WO they're producing against, doesn't retire one),
// and each one's count needs to keep showing on the dashboard
// independently instead of the latest update silently overwriting
// whatever the previous work order's tile showed. The device itself
// doesn't enforce "one active WO" either - it's free to report against
// any WO in its fetched list at any time.

export interface ProductionEntry {
  stationId: string;
  workOrderId: string;
  productionCount: number;
  // Reject/defect count - added 2026-08-13 for OEE Quality (good /
  // (good + reject)), see getQuality()'s comment. Optional on the wire
  // (older firmware won't send it) but always a number here - defaults
  // to 0 in setProductionCount().
  rejectCount: number;
  updatedAt: string;
}

function key(stationId: string, workOrderId: string): string {
  return `${stationId}::${workOrderId}`;
}

const productionByKey = new Map<string, ProductionEntry>();

export function setProductionCount(
  stationId: string,
  count: number,
  workOrderId: string,
  rejectCount = 0,
): ProductionEntry {
  const entry: ProductionEntry = {
    stationId,
    workOrderId,
    productionCount: count,
    rejectCount,
    updatedAt: new Date().toISOString(),
  };
  productionByKey.set(key(stationId, workOrderId), entry);
  return entry;
}

// Newest-updated first - matches listIncidents()'s convention in
// incident-store.ts, so the dashboard's most-recently-touched work order
// naturally sorts to the top without the frontend needing its own sort.
export function listProductionCounts(): ProductionEntry[] {
  return [...productionByKey.values()].sort((a, b) => b.updatedAt.localeCompare(a.updatedAt));
}

// Single-entry lookup, 0/0 if this work order has never reported a count.
// Used by server.ts to enrich GET /api/v1/work-orders/stations/:stationId
// (and the all-stations GET /api/v1/work-orders) with each work order's
// current count (2026-08-13) - the device's picker fetches that endpoint
// fresh every time it opens (see firmware/src/andon_workorders.cpp's
// sync()) specifically so its counter screen starts from the server's own
// number instead of a possibly-stale device-local one ("ga bisa fetch
// dulu dr dashboard... kalau gini jadi ngga sesuai dgn yang sudah di
// server").
export function getProductionCount(stationId: string, workOrderId: string): number {
  return productionByKey.get(key(stationId, workOrderId))?.productionCount ?? 0;
}

export function getRejectCount(stationId: string, workOrderId: string): number {
  return productionByKey.get(key(stationId, workOrderId))?.rejectCount ?? 0;
}
