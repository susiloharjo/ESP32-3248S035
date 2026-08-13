// Plain DOM rendering - no framework (PLAN.md §3: "a demo with one page,
// a handful of cards, and a WebSocket listener doesn't need React/etc.'s
// overhead"). Each render* function owns one container and fully replaces
// its contents - cheap enough at this scale (a handful of incidents) that
// there's no need for a diffing layer.

import type { Incident, ProductionEntry } from "./types";
import type { OeeResult } from "./oee";
import { CATEGORY_COLORS, STATUS_COLORS } from "./tokens";

function formatAge(openedAt: string): string {
  const elapsedMs = Date.now() - new Date(openedAt).getTime();
  const totalSec = Math.max(0, Math.floor(elapsedMs / 1000));
  const mm = Math.floor(totalSec / 60);
  const ss = totalSec % 60;
  return `${mm}:${String(ss).padStart(2, "0")}`;
}

// Called once a second (see main.ts) to tick every visible age label
// without re-rendering the whole board just for a clock.
export function tickAges(): void {
  document.querySelectorAll<HTMLElement>("[data-opened-at]").forEach((el) => {
    el.textContent = formatAge(el.dataset.openedAt!);
  });
}

// Acknowledge is the only transition the dashboard can drive - Start
// Handling and Resolve are deliberately device-only (see backend/src/
// server.ts's INCIDENT_STATUS_UPDATE handler and its comment): a
// technician has to be physically at the terminal to advance past
// Acknowledged. Once acknowledged, the card just says so instead of
// offering a button for something this UI isn't allowed to do (backend
// doesn't expose those endpoints either - see agents.md §11, "hiding a
// button is not authorization" - this mirrors that on both ends).
const NEXT_ACTION: Record<string, { action: string; label: string } | undefined> = {
  OPEN: { action: "acknowledge", label: "Acknowledge" },
};

const WAITING_ON_DEVICE_TEXT: Record<string, string | undefined> = {
  ACKNOWLEDGED: "Awaiting technician on-site to start handling",
  HANDLING: "Awaiting technician on-site to resolve",
};

function incidentCard(incident: Incident): string {
  const categoryColor = CATEGORY_COLORS[incident.categoryCode] ?? "#52636C";
  const statusColor = STATUS_COLORS[incident.status] ?? "#52636C";
  const next = NEXT_ACTION[incident.status];
  const waitingText = WAITING_ON_DEVICE_TEXT[incident.status];
  const actionHtml = next
    ? `<button class="action-btn" data-incident-id="${incident.incidentId}" data-action="${next.action}" style="background:${categoryColor}">${next.label}</button>`
    : waitingText
      ? `<p class="waiting-on-device">${waitingText}</p>`
      : "";

  return `
    <article class="card" style="border-color:${categoryColor}">
      <div class="card-top">
        <span class="badge" style="background:${categoryColor}">${incident.categoryCode}</span>
        <span class="status-pill" style="color:${statusColor};border-color:${statusColor}">${incident.status}</span>
      </div>
      <div class="card-reason">${incident.reasonCode}</div>
      <div class="card-meta">
        <span>${incident.stationId}</span>
        <span data-opened-at="${incident.openedAt}">${formatAge(incident.openedAt)}</span>
      </div>
      ${actionHtml}
    </article>
  `;
}

export function renderActiveBoard(container: HTMLElement, incidents: Incident[]): void {
  const active = incidents.filter((i) => i.status !== "RESOLVED");
  container.innerHTML = active.length
    ? active.map(incidentCard).join("")
    : `<p class="empty-state">No active incidents.</p>`;
}

export function renderResolvedList(container: HTMLElement, incidents: Incident[]): void {
  const resolved = incidents
    .filter((i) => i.status === "RESOLVED")
    .sort((a, b) => (b.resolvedAt ?? "").localeCompare(a.resolvedAt ?? ""))
    .slice(0, 10);

  container.innerHTML = resolved.length
    ? resolved
        .map(
          (i) => `
      <li>
        <span class="badge" style="background:${CATEGORY_COLORS[i.categoryCode] ?? "#52636C"}">${i.categoryCode}</span>
        ${i.reasonCode} — ${i.stationId}
        <span class="resolved-at">${i.resolvedAt ? new Date(i.resolvedAt).toLocaleTimeString() : ""}</span>
      </li>`,
        )
        .join("")
    : `<li class="empty-state">Nothing resolved yet.</li>`;
}

// One tile per (station, work order) pair (2026-08-13) - a station can
// have several work orders reporting counts independently (see
// backend/src/production-store.ts's header), so this can no longer assume
// one tile == one station. Sorted by station then work order so a
// station's tiles group together instead of jumping around as updates
// arrive in arbitrary order.
export function renderProductionTiles(container: HTMLElement, production: ProductionEntry[]): void {
  const sorted = [...production].sort(
    (a, b) => a.stationId.localeCompare(b.stationId) || a.workOrderId.localeCompare(b.workOrderId),
  );
  container.innerHTML = sorted.length
    ? sorted
        .map(
          (entry) => `
      <div class="production-tile">
        <div class="tile-station">${entry.stationId}<span class="tile-wo">${entry.workOrderId || "—"}</span></div>
        <div class="tile-count">${entry.productionCount}</div>
      </div>`,
        )
        .join("")
    : `<p class="empty-state">No production data yet.</p>`;
}

// One card per station, sorted (OeeResult[] already comes pre-sorted by
// stationId from computeOeeForAllStations()). "(approx)" is repeated on
// every card, not just the section heading, so it stays visible even if
// someone screenshots/crops just this panel - see oee.ts's header
// comment for exactly what's approximated and why.
export function renderOeeTiles(container: HTMLElement, results: OeeResult[]): void {
  container.innerHTML = results.length
    ? results
        .map((r) => {
          const pct = (n: number) => Math.round(n * 100);
          return `
      <div class="oee-tile">
        <div class="oee-station">${r.stationId}</div>
        <div class="oee-big">${pct(r.oee)}<span class="oee-unit">% OEE (approx)</span></div>
        <div class="oee-breakdown">
          <span title="Availability">A ${pct(r.availability)}%</span>
          <span title="Performance">P ${pct(r.performance)}%</span>
          <span title="Quality">Q ${pct(r.quality)}%</span>
        </div>
      </div>`;
        })
        .join("")
    : `<p class="empty-state">No production data yet.</p>`;
}

// Andon call counts by category ("berapa kali maintenance? quality?
// dst" - 2026-08-13). Always shows all four known categories (even at
// 0), not just ones that have happened, so the breakdown reads as a
// complete picture rather than a list that only grows. Counts every
// incident ever seen this session (not just active ones) - same
// since-backend-started scope as everything else in this in-memory demo
// (agents.md's own scope note: no persistence, resets on restart).
// Plain CSS bar chart, no charting library - matches PLAN.md §3's "don't
// need React/etc.'s overhead" call for the rest of this dashboard.
export function renderCategoryChart(container: HTMLElement, incidents: Incident[]): void {
  const counts: Record<string, number> = {};
  for (const i of incidents) counts[i.categoryCode] = (counts[i.categoryCode] ?? 0) + 1;

  const categories = Object.keys(CATEGORY_COLORS);
  const max = Math.max(1, ...categories.map((c) => counts[c] ?? 0));

  container.innerHTML = categories
    .map((c) => {
      const count = counts[c] ?? 0;
      const pct = Math.round((count / max) * 100);
      const color = CATEGORY_COLORS[c];
      return `
      <div class="chart-row">
        <span class="chart-label">${c}</span>
        <div class="chart-bar-track"><div class="chart-bar" style="width:${pct}%;background:${color}"></div></div>
        <span class="chart-count">${count}</span>
      </div>`;
    })
    .join("");
}

export function renderConnectionStatus(el: HTMLElement, connected: boolean): void {
  el.textContent = connected ? "● Connected" : "○ Reconnecting…";
  el.style.color = connected ? "#35C759" : "#F2A914";
}

// PLAN.md §4.1: fires only for a brand-new call (incident_created), never
// on a status transition. Auto-dismisses; the active board (not this
// toast) is the source of truth for what still needs attention.
export function showAlert(container: HTMLElement, incident: Incident): void {
  const color = CATEGORY_COLORS[incident.categoryCode] ?? "#52636C";
  const el = document.createElement("div");
  el.className = "alert-banner";
  el.style.background = color;
  el.innerHTML = `<strong>${incident.categoryCode}</strong> called at <strong>${incident.stationId}</strong> — ${incident.reasonCode}`;
  el.addEventListener("click", () => el.remove());
  container.appendChild(el);
  setTimeout(() => el.remove(), 8000);
}
