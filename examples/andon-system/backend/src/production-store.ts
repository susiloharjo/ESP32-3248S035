// In-memory production-count-by-station store, fed by PRODUCTION_COUNT_UPDATED
// MQTT events (see server.ts). Same ad-hoc-extension status as before (not in
// architectur.md §8 or PRD.md - see server.ts's note on that event type) -
// this just replaces the old bare latestProductionCountByStation Map with a
// couple of named accessors so dashboard/PLAN.md's GET /api/v1/production
// has something to read from.

const productionByStation = new Map<string, number>();

export function setProductionCount(stationId: string, count: number): void {
  productionByStation.set(stationId, count);
}

export function listProductionCounts(): Record<string, number> {
  return Object.fromEntries(productionByStation);
}
