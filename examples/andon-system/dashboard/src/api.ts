// REST calls against backend/src/server.ts's dashboard-facing endpoints
// (PLAN.md §2). Vite's dev-server proxy (vite.config.ts) forwards /api to
// the backend so these can stay relative paths in both dev and the built
// static output served alongside the backend.

import type { Incident, ProductionEntry } from "./types";

export async function fetchIncidents(): Promise<Incident[]> {
  const res = await fetch("/api/v1/incidents");
  const body = (await res.json()) as { incidents: Incident[] };
  return body.incidents;
}

// GET /api/v1/work-orders (all stations) - the full work-order catalog,
// each entry already carrying its current productionCount (server.ts
// joins work-order-store.ts's seed data with production-store.ts's live
// state). Used for the initial PRODUCTION panel fill instead of GET
// /api/v1/production (2026-08-13, "list di hardware dengan list di
// dashboard beda"): that endpoint only ever returned work orders that
// had already reported at least one PRODUCTION_COUNT_UPDATED, so a
// freshly-seeded work order the device's picker already listed (with
// productionCount: 0) wouldn't show up here yet. This endpoint is the
// same one the device's own picker re-fetches every time it opens (see
// firmware/src/andon_workorders.cpp's sync()), so the two now agree.
interface WorkOrderCatalogEntry {
  stationId: string;
  workOrderId: string;
  productionCount: number;
}

export async function fetchWorkOrderCatalog(): Promise<ProductionEntry[]> {
  const res = await fetch("/api/v1/work-orders");
  const body = (await res.json()) as { workOrders: WorkOrderCatalogEntry[] };
  return body.workOrders.map((wo) => ({
    stationId: wo.stationId,
    workOrderId: wo.workOrderId,
    productionCount: wo.productionCount,
    updatedAt: "", // catalog entry, not a reported event - see render.ts (not used for sorting there)
  }));
}

async function postAction(incidentId: string, action: string): Promise<Incident> {
  const res = await fetch(`/api/v1/incidents/${incidentId}/${action}`, { method: "POST" });
  if (!res.ok) throw new Error(`${action} on ${incidentId} failed: HTTP ${res.status}`);
  const body = (await res.json()) as { incident: Incident };
  return body.incident;
}

// No optimistic UI update (PLAN.md §6: no idempotent-retry story, no
// server-side state-machine validation yet) - the caller waits for the
// REST response, and the WebSocket incident_updated broadcast that the
// backend also sends is what actually drives the re-render (see main.ts)
// so a second browser tab stays in sync too.
//
// Acknowledge is the ONLY transition exposed here - Start Handling and
// Resolve are deliberately device-only (technician must be physically at
// the terminal, product decision 2026-08-13); the backend doesn't expose
// those endpoints at all anymore, so there's nothing for this module to
// call even if a button tried to.
export const acknowledgeIncident = (id: string) => postAction(id, "acknowledge");
