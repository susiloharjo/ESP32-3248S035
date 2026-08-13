// In-memory production-count-by-station store, fed by PRODUCTION_COUNT_UPDATED
// MQTT events (see server.ts). Same ad-hoc-extension status as before (not in
// architectur.md §8 or PRD.md - see server.ts's note on that event type) -
// this just replaces the old bare latestProductionCountByStation Map with a
// couple of named accessors so dashboard/PLAN.md's GET /api/v1/production
// has something to read from.
//
// Also carries the workOrderId the count was last reported against
// (2026-08-13, alongside firmware/src/andon_workorders.cpp) - purely
// informational for the dashboard tile, not a validation/authority check;
// the device is free to report against whatever work order it currently
// has selected.

export interface ProductionEntry {
  productionCount: number;
  workOrderId: string;
}

const productionByStation = new Map<string, ProductionEntry>();

export function setProductionCount(stationId: string, count: number, workOrderId: string): void {
  productionByStation.set(stationId, { productionCount: count, workOrderId });
}

export function listProductionCounts(): Record<string, ProductionEntry> {
  return Object.fromEntries(productionByStation);
}
