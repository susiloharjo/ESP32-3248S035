import "./style.css";
import type { Incident, ProductionEntry } from "./types";
import { fetchIncidents, fetchWorkOrderCatalog, acknowledgeIncident } from "./api";
import { connectRealtime } from "./realtime";
import { computeOeeForAllStations } from "./oee";
import {
  renderActiveBoard,
  renderResolvedList,
  renderProductionTiles,
  renderOeeTiles,
  renderCategoryChart,
  renderConnectionStatus,
  showAlert,
  tickAges,
} from "./render";

const boardEl = document.querySelector<HTMLElement>("#active-board")!;
const resolvedEl = document.querySelector<HTMLElement>("#resolved-list")!;
const productionEl = document.querySelector<HTMLElement>("#production-tiles")!;
const oeeEl = document.querySelector<HTMLElement>("#oee-tiles")!;
const categoryChartEl = document.querySelector<HTMLElement>("#category-chart")!;
const alertsEl = document.querySelector<HTMLElement>("#alerts")!;
const connectionEl = document.querySelector<HTMLElement>("#connection-status")!;

// Single in-memory client-side mirror of the backend's incident list -
// incident_created/incident_updated events (and the REST reconcile below)
// all funnel through upsertIncident() so there's one place that decides
// when to re-render.
const incidents = new Map<string, Incident>();

// Keyed by "stationId::workOrderId", not stationId alone (2026-08-13) -
// mirrors backend/src/production-store.ts's keying, so a station with
// several work orders in flight gets a tile each instead of the latest
// update overwriting whatever the previous work order's tile showed.
const production = new Map<string, ProductionEntry>();
function productionKey(stationId: string, workOrderId: string): string {
  return `${stationId}::${workOrderId}`;
}

// OEE (oee.ts) depends on BOTH incidents (Availability, from downtime)
// and production (Performance/Quality) - renderAll() recomputes it
// alongside everything else instead of only on one of the two sources,
// so it stays live whichever changes.
function renderAll(): void {
  const incidentList = [...incidents.values()];
  const productionList = [...production.values()];
  renderActiveBoard(boardEl, incidentList);
  renderResolvedList(resolvedEl, incidentList);
  renderProductionTiles(productionEl, productionList);
  renderOeeTiles(oeeEl, computeOeeForAllStations(incidentList, productionList));
  renderCategoryChart(categoryChartEl, incidentList);
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
  const [incidentList, catalog] = await Promise.all([fetchIncidents(), fetchWorkOrderCatalog()]);
  incidents.clear();
  for (const incident of incidentList) incidents.set(incident.incidentId, incident);
  production.clear();
  for (const entry of catalog) production.set(productionKey(entry.stationId, entry.workOrderId), entry);
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
      case "production_updated": {
        // product/target only ever come from the catalog fetch (see
        // types.ts's ProductionEntry comment) - preserve whatever was
        // already known instead of clobbering it with undefined, so
        // oee.ts's Performance calc (which needs target) doesn't lose
        // its baseline the moment a live update arrives.
        const existing = production.get(productionKey(event.stationId, event.workOrderId));
        production.set(productionKey(event.stationId, event.workOrderId), {
          stationId: event.stationId,
          workOrderId: event.workOrderId,
          product: existing?.product,
          target: existing?.target,
          productionCount: event.productionCount,
          rejectCount: event.rejectCount,
          updatedAt: new Date().toISOString(),
        });
        renderAll();
        break;
      }
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
