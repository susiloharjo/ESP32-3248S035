// In-memory incident store - replaces the old idempotencyKey -> incidentId
// -only Map that used to live in server.ts (category/reason/station were
// logged and discarded, never retained). See dashboard/PLAN.md §1 for why
// that was a blocker and §6 for the deviations this still accepts (no
// PostgreSQL, no optimistic versioning, no audit trail, no state-machine
// validation server-side - the dashboard frontend is trusted to only show
// the one valid action per card).

export type IncidentStatus = "OPEN" | "ACKNOWLEDGED" | "HANDLING" | "RESOLVED";

export interface Incident {
  incidentId: string;
  stationId: string;
  deviceId: string;
  categoryCode: string;
  reasonCode: string;
  workOrderId: string;
  status: IncidentStatus;
  openedAt: string; // ISO 8601
  acknowledgedAt?: string;
  handledAt?: string;
  resolvedAt?: string;
}

const incidentsById = new Map<string, Incident>();
const incidentIdByIdempotencyKey = new Map<string, string>();
let nextIncidentSeq = 1;

export interface CreateIncidentParams {
  idempotencyKey: string;
  stationId: string;
  deviceId: string;
  categoryCode: string;
  reasonCode: string;
  workOrderId: string;
}

// Returns the incident plus whether it was just created. `isNew: false`
// means this idempotencyKey was already seen (architectur.md §8.3: "at
// least once delivery, duplicates are expected") - server.ts should reply
// with the same ACCEPTED result but skip broadcasting incident_created
// again.
export function createOrGetIncident(
  params: CreateIncidentParams,
): { incident: Incident; isNew: boolean } {
  const existingId = incidentIdByIdempotencyKey.get(params.idempotencyKey);
  if (existingId) {
    const existing = incidentsById.get(existingId);
    if (existing) return { incident: existing, isNew: false };
  }

  const incident: Incident = {
    incidentId: `test-incident-${nextIncidentSeq++}`,
    stationId: params.stationId,
    deviceId: params.deviceId,
    categoryCode: params.categoryCode,
    reasonCode: params.reasonCode,
    workOrderId: params.workOrderId,
    status: "OPEN",
    openedAt: new Date().toISOString(),
  };
  incidentsById.set(incident.incidentId, incident);
  incidentIdByIdempotencyKey.set(params.idempotencyKey, incident.incidentId);
  return { incident, isNew: true };
}

export function listIncidents(): Incident[] {
  // Newest first - dashboard/PLAN.md §2's GET /api/v1/incidents contract.
  return [...incidentsById.values()].sort((a, b) => b.openedAt.localeCompare(a.openedAt));
}

export function getIncident(incidentId: string): Incident | undefined {
  return incidentsById.get(incidentId);
}

export function acknowledgeIncident(incidentId: string): Incident | undefined {
  const incident = incidentsById.get(incidentId);
  if (!incident) return undefined;
  incident.status = "ACKNOWLEDGED";
  incident.acknowledgedAt = new Date().toISOString();
  return incident;
}

export function startHandlingIncident(incidentId: string): Incident | undefined {
  const incident = incidentsById.get(incidentId);
  if (!incident) return undefined;
  incident.status = "HANDLING";
  incident.handledAt = new Date().toISOString();
  return incident;
}

export function resolveIncident(incidentId: string): Incident | undefined {
  const incident = incidentsById.get(incidentId);
  if (!incident) return undefined;
  incident.status = "RESOLVED";
  incident.resolvedAt = new Date().toISOString();
  return incident;
}
