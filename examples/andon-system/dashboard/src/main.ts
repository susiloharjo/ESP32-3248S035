import "./style.css";
import type { Incident, ProductionEntry } from "./types";
import { fetchIncidents, fetchProduction, acknowledgeIncident } from "./api";
import { connectRealtime } from "./realtime";
import { renderActiveBoard, renderResolvedList, renderProductionTiles, renderConnectionStatus, showAlert, tickAges } from "./render";

const boardEl = document.querySelector<HTMLElement>("#active-board")!;
const resolvedEl = document.querySelector<HTMLElement>("#resolved-list")!;
const productionEl = document.querySelector<HTMLElement>("#production-tiles")!;
const alertsEl = document.querySelector<HTMLElement>("#alerts")!;
const connectionEl = document.querySelector<HTMLElement>("#connection-status")!;

// Single in-memory client-side mirror of the backend's incident list -
// incident_created/incident_updated events (and the REST reconcile below)
// all funnel through upsertIncident() so there's one place that decides
// when to re-render.
const incidents = new Map<string, Incident>();
let production: Record<string, ProductionEntry> = {};

function renderAll(): void {
  const list = [...incidents.values()];
  renderActiveBoard(boardEl, list);
  renderResolvedList(resolvedEl, list);
  renderProductionTiles(productionEl, production);
}

function upsertIncident(incident: Incident): void {
  incidents.set(incident.incidentId, incident);
  renderAll();
}

// PLAN.md §2's "reconcile from REST after reconnect" (agents.md §11) -
// called on every WebSocket open, including the very first connect, so
// there's exactly one reconcile code path instead of special-casing the
// first load.
async function reconcile(): Promise<void> {
  const [incidentList, productionData] = await Promise.all([fetchIncidents(), fetchProduction()]);
  incidents.clear();
  for (const incident of incidentList) incidents.set(incident.incidentId, incident);
  production = productionData;
  renderAll();
}

connectRealtime(
  (event) => {
    switch (event.type) {
      case "incident_created":
        upsertIncident(event.incident);
        showAlert(alertsEl, event.incident);
        break;
      case "incident_updated":
        upsertIncident(event.incident);
        break;
      case "production_updated":
        production = {
          ...production,
          [event.stationId]: { productionCount: event.productionCount, workOrderId: event.workOrderId },
        };
        renderProductionTiles(productionEl, production);
        break;
    }
  },
  (connected) => {
    renderConnectionStatus(connectionEl, connected);
    if (connected) reconcile().catch((err) => console.error("Reconcile failed:", err));
  },
);

setInterval(tickAges, 1000);

// Event delegation - one listener for every action button. Acknowledge is
// the only action render.ts ever puts a data-action on (see its
// NEXT_ACTION comment) - Start Handling/Resolve are device-only now, no
// button, no case for them here.
boardEl.addEventListener("click", (e) => {
  const btn = (e.target as HTMLElement).closest<HTMLButtonElement>("[data-action]");
  if (!btn) return;
  const { incidentId, action } = btn.dataset;
  if (!incidentId || action !== "acknowledge") return;

  btn.disabled = true;
  acknowledgeIncident(incidentId)
    .then(upsertIncident) // WebSocket incident_updated will also arrive and upsert again - harmless, same data
    .catch((err) => {
      console.error(err);
      btn.disabled = false;
    });
});
