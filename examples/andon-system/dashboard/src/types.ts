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

export interface ProductionEntry {
  productionCount: number;
  workOrderId: string;
}

export type DashboardEvent =
  | { type: "incident_created"; incident: Incident }
  | { type: "incident_updated"; incident: Incident }
  | { type: "production_updated"; stationId: string; productionCount: number; workOrderId: string };
