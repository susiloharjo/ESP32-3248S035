// Seed data for GET /api/v1/work-orders/stations/:stationId - a new
// endpoint (2026-08-13), not in architectur.md or contracts/ yet, same
// ad-hoc-extension status as PRODUCTION_COUNT_UPDATED (see server.ts's note
// on that event type and andon_mqtt.hpp's SCOPE note). Lets the device pick
// which work order it's currently producing against, instead of the
// firmware hardcoding "WO-240811-07" everywhere - see
// firmware/src/andon_workorders.cpp for the fetch/cache/select side.
//
// In-memory only, keyed by stationId, same pattern as config-data.ts's
// STATION_CONFIG (static seed, not real ERP/MES integration - agents.md
// §10 wants that for production).

// Doesn't include productionCount - that's this station's static seed
// data (id/product/target), not live state. server.ts's route handler
// joins each entry with production-store.ts's getProductionCount() before
// it ever reaches an HTTP response.
export interface WorkOrder {
  workOrderId: string;
  product: string;
  target: number;
}

const WORK_ORDERS_BY_STATION: Record<string, WorkOrder[]> = {
  "STATION-01": [
    { workOrderId: "WO-240811-07", product: "PVC Pipe 4in", target: 120 },
    { workOrderId: "WO-240812-03", product: "PVC Pipe 2in", target: 200 },
    { workOrderId: "WO-240812-09", product: "HDPE Fitting 90deg", target: 80 },
  ],
};

export function listWorkOrders(stationId: string): WorkOrder[] {
  return WORK_ORDERS_BY_STATION[stationId] ?? [];
}

// Flattened across every station - backs GET /api/v1/work-orders (all
// stations), added same day as listWorkOrders() started returning
// productionCount too (2026-08-13): the dashboard was only ever showing
// work orders that had already reported at least one count (via
// production-store.ts, fed by PRODUCTION_COUNT_UPDATED events), so a
// freshly-seeded work order the device's picker already lists (with
// productionCount: 0) simply didn't appear on the dashboard yet - "list
// di hardware dengan list di dashboard beda". This gives the dashboard
// the same full catalog the device's picker uses, not just the subset
// that happens to have reported something.
export function listAllWorkOrders(): Array<WorkOrder & { stationId: string }> {
  return Object.entries(WORK_ORDERS_BY_STATION).flatMap(([stationId, workOrders]) =>
    workOrders.map((wo) => ({ ...wo, stationId })),
  );
}
