// Mirrors backend/src/incident-store.ts's Incident and
// backend/src/realtime.ts's DashboardEvent - kept as a hand-copied twin
// rather than a shared package, matching backend/config-data.ts's own
// precedent for this repo (no cross-package build dependency for a
// two-package demo). Keep in sync by hand if the backend's shape changes.

export type IncidentStatus = "OPEN" | "ACKNOWLEDGED" | "HANDLING" | "RESOLVED";

export interface Incident {
  incidentId: string;
  stationId: string;
  deviceId: string;
  categoryCode: string;
  reasonCode: string;
  workOrderId: string;
  status: IncidentStatus;
  openedAt: string;
  acknowledgedAt?: string;
  handledAt?: string;
  resolvedAt?: string;
}

// Keyed by (stationId, workOrderId), not stationId alone (2026-08-13) - a
// station can have several work orders reporting counts independently, see
// backend/src/production-store.ts's header for why.
//
// product/target are optional because they only ever come from the
// work-order catalog fetch (GET /api/v1/work-orders*, see
// fetchWorkOrderCatalog()) - a live production_updated WebSocket event
// doesn't carry them, so main.ts's handler preserves whatever was already
// known instead of clobbering them with undefined. target feeds oee.ts's
// Performance calculation.
export interface ProductionEntry {
  stationId: string;
  workOrderId: string;
  product?: string;
  target?: number;
  productionCount: number;
  rejectCount: number;
  updatedAt: string;
}

export type DashboardEvent =
  | { type: "incident_created"; incident: Incident }
  | { type: "incident_updated"; incident: Incident }
  | {
      type: "production_updated";
      stationId: string;
      productionCount: number;
      rejectCount: number;
      workOrderId: string;
    };
