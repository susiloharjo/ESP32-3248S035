// REST calls against backend/src/server.ts's dashboard-facing endpoints
// (PLAN.md §2). Vite's dev-server proxy (vite.config.ts) forwards /api to
// the backend so these can stay relative paths in both dev and the built
// static output served alongside the backend.

import type { Incident } from "./types";

export async function fetchIncidents(): Promise<Incident[]> {
  const res = await fetch("/api/v1/incidents");
  const body = (await res.json()) as { incidents: Incident[] };
  return body.incidents;
}

export async function fetchProduction(): Promise<Record<string, number>> {
  const res = await fetch("/api/v1/production");
  const body = (await res.json()) as { production: Record<string, number> };
  return body.production;
}

async function postAction(incidentId: string, action: string): Promise<Incident> {
  const res = await fetch(`/api/v1/incidents/${incidentId}/${action}`, { method: "POST" });
  if (!res.ok) throw new Error(`${action} on ${incidentId} failed: HTTP ${res.status}`);
  const body = (await res.json()) as { incident: Incident };
  return body.incident;
}

// No optimistic UI update on any of these (PLAN.md §6: no idempotent-retry
// story, no server-side state-machine validation yet) - the caller waits
// for the REST response, and the WebSocket incident_updated broadcast that
// the backend also sends is what actually drives the re-render (see
// main.ts) so a second browser tab stays in sync too.
export const acknowledgeIncident = (id: string) => postAction(id, "acknowledge");
export const startHandlingIncident = (id: string) => postAction(id, "start-handling");
export const resolveIncident = (id: string) => postAction(id, "resolve");
