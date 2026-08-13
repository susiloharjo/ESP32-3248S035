# Dashboard Demo Implementation Plan — Digital Andon System

**Status (2026-08-13): built and verified end-to-end.** All of §9's build order (backend incident/production store + REST + WebSocket, frontend Vite app, `deploy/docker-compose.yml` wiring) is done. Verified via headless browser against the real `docker compose up --build` stack (not just `vite dev`): MQTT `ANDON_REQUESTED` → alert banner + card appear live, Acknowledge drives OPEN → ACKNOWLEDGED, `PRODUCTION_COUNT_UPDATED` updates its tile live, resolved incidents move to the sidebar list, zero browser console errors.

**Same-day product decision, applied after the above:** Start Handling and Resolve are no longer dashboard actions at all - only Acknowledge is. A technician must be physically at the terminal to advance past Acknowledged; the backend doesn't expose `.../start-handling` or `.../resolve` REST endpoints anymore (removed, not just hidden - agents.md §11: "hiding a button is not authorization"). Firmware now drives those two transitions itself over MQTT (`AndonMqtt::submitStatusUpdate()`, closing §8's old "known gap" below - firmware really does subscribe now, and the old `(DEMO) SIMULATE TECHNICIAN ACK` button is gone). §2/§4/§4.1/§8 below still describe the original all-three-actions design; treat this note as the current behavior where they conflict.

**Also added same day:** `production_updated` now carries `workOrderId` alongside `productionCount` (device-selected via the new `SCR_WORK_ORDER_LIST` picker - see `firmware/PLAN.md` §10 and `architectur.md` §8.1), shown under the station name on each production tile (`render.ts`'s `renderProductionTiles()`). The device also now pushes a `production_updated` once automatically on every boot, not only on an operator's manual confirm, so a tile won't look stale right after a terminal power-cycles.

**Corrected same day (still 2026-08-13):** the first pass above kept production keyed by `stationId` alone, so switching work orders on the device silently overwrote the previous work order's tile - a station producing against two work orders in one shift only ever showed whichever was updated most recently. `backend/src/production-store.ts` is now keyed by `(stationId, workOrderId)` instead, `GET /api/v1/production` returns an array of `{stationId, workOrderId, productionCount, updatedAt}` (was `Record<stationId, {...}>`), and the dashboard renders one tile per (station, work order) pair, sorted by station then work order - `api.ts`/`main.ts`/`render.ts`/`types.ts` all updated to match. A station with three work orders reporting counts now shows three tiles.

**Corrected again same day:** `GET /api/v1/production` only ever returns work orders that have reported at least one `PRODUCTION_COUNT_UPDATED` - a freshly-seeded work order the device's `SCR_WORK_ORDER_LIST` picker already lists (with count 0) simply wasn't on the dashboard yet, so "list di hardware dengan list di dashboard beda." The dashboard's initial `reconcile()` now calls the new `GET /api/v1/work-orders` (all stations, added alongside the per-station version `firmware/PLAN.md` already documents - see `architectur.md` §8.1) instead of `GET /api/v1/production` - same enrichment (`productionCount` joined in from `production-store.ts`), but sourced from the full work-order catalog so every work order shows up immediately, not just ones already reported on. `production_updated` WebSocket events still update individual tiles live on top of that, unchanged. `api.ts`'s `fetchProduction()` renamed to `fetchWorkOrderCatalog()` to match.

**New same day, two new panels:**
- **OEE** ("di dashboard bisa itungin oee ngga bro?") - new `oee.ts`, `computeOeeForAllStations()`, rendered by `render.ts`'s `renderOeeTiles()` into a new `#oee-tiles` panel, one card per station. Computed client-side from data the dashboard already holds (`incidents`/`production` Maps in `main.ts`) - no new backend endpoint for the math itself. **Explicitly a simplified/approximate OEE, not audit-grade** (label says so, with a hover tooltip spelling out exactly what's cut):
  - *Availability* = (8h planned shift − downtime) / 8h. Downtime = every incident's duration (`openedAt` → `resolvedAt`, or → now if still open) for that station, **regardless of category** - this system has no "did this actually halt the line" flag, so a QUALITY or SUPERVISOR call counts the same as MAINTENANCE.
  - *Performance* = actual good count / work-order target quantity (both summed across every WO reported for that station), used as a stand-in for real OEE's `(ideal cycle time × count) / run time` since there's no measured ideal-rate figure anywhere in this system (SCR-01's "18 pcs/h" is a hardcoded display string).
  - *Quality* = good / (good + reject) - this part is real, fed by the device's new REJECTS counter (see `firmware/PLAN.md`'s matching entry) rather than assumed 100%.
  - User explicitly chose "add real reject tracking first" over "assume 100% quality" and "8-hour shift" over "24-hour nonstop" when asked.
- **Andon calls by category** ("chart statistic andon boleh jg... berapa kali maintenance? quality? dst") - new `#category-chart` panel, `render.ts`'s `renderCategoryChart()`: plain CSS bar chart (no charting library, consistent with §3's "don't need React/etc.'s overhead"), counts every incident seen this session by `categoryCode`, always shows all four known categories (even at 0) so it reads as a complete breakdown rather than a growing list.
- `ProductionEntry` (`types.ts`) gained optional `product`/`target` (only ever populated from the work-order-catalog fetch, not from live `production_updated` events - `main.ts`'s WS handler preserves them across a live update instead of clobbering with `undefined`) and non-optional `rejectCount`, both needed for the OEE calc above.

| Field | Value |
|---|---|
| Scope | `examples/andon-system/dashboard/` (new) + a small, explicit extension to `backend/` — the current backend can't back an interactive dashboard as-is (see §2) |
| Goal | Live-viewable incident board + production-count feed, with real acknowledge/handling/resolve actions a "technician" can actually take from the browser |
| Out of scope | Auth/RBAC, PostgreSQL, admin CRUD (WEB-003), KPI reports (WEB-005), CSV export (WEB-004), notifications/escalation, confirm-close/reopen/cancel actions — see §7 |
| Reference docs | [PRD.md](../PRD.md) §9.6 (WEB-001–005), [agents.md](../agents.md) §11 (Dashboard rules), [architectur.md](../architectur.md) §9 (REST API), §10 (data model) |
| Precedence note | Same posture as `firmware/PLAN.md`: this narrows the full spec down to a demoable slice against the **existing test backend**, not the production system agents.md describes. Deviations recorded in §6. |

## 1. Why this isn't just a frontend task

`agents.md` §11 assumes a dashboard sits on top of a real incident domain service (REST `/incidents*`, WebSocket gateway, PostgreSQL). None of that exists yet — `backend/src/server.ts`'s own scope note is explicit: *"Only GET config and the ANDON_REQUESTED → ACCEPTED round trip are implemented."* Specifically, today:

- Incidents are stored as `Map<idempotencyKey, incidentId>` — **only the generated ID is kept**. Category, reason, station, and open time are logged to console and then discarded, never stored anywhere retrievable.
- No REST endpoints exist for listing incidents or changing their state (`architectur.md` §9's `acknowledge`/`start-handling`/`resolve`/etc. are all unimplemented).
- No WebSocket/SSE gateway exists — nothing pushes updates to a browser.
- Production-count updates land in `latestProductionCountByStation` (a `Map`) with only a `/debug/production/:stationId` polling endpoint, no push.

A dashboard that's actually "interaktif" per the request needs all of the above to exist first, even in minimal/in-memory form. This plan covers both pieces together since neither is independently demoable.

## 2. Scope for this pass

**Backend additions** (still in-memory, still no auth — same test-harness posture `backend/`'s existing scope note already commits to; extending it, not upgrading its production-readiness):

- Replace the idempotency-key-only `Map` with a real in-memory incident record: `{ incidentId, stationId, categoryCode, reasonCode, workOrderId, status, openedAt, acknowledgedAt?, handledAt?, resolvedAt? }`.
- New REST endpoints (subset of `architectur.md` §9 — only what a demo "technician" actually needs):
  - `GET /api/v1/incidents` — list, newest first (dashboard's active board)
  - `POST /api/v1/incidents/:id/acknowledge`
  - `POST /api/v1/incidents/:id/start-handling`
  - `POST /api/v1/incidents/:id/resolve`
  - `GET /api/v1/production` — current count per station (replaces the ad-hoc `/debug/production/:stationId`, returns all stations at once)
- A WebSocket endpoint (`@fastify/websocket`, since Fastify's already the HTTP framework) broadcasting to all connected dashboard clients whenever state changes — from a new incident, a REST action above, or an MQTT event. Two distinct incident message types, not one, specifically so the frontend can tell "brand new call" apart from "status changed on something already visible" (see §4.1 - only the former should trigger the alert):
  - `{type: "incident_created", incident}` — fired once, exactly when a new `ANDON_REQUESTED` MQTT event creates a record
  - `{type: "incident_updated", incident}` — fired on every subsequent state change (acknowledge/start-handling/resolve)
  - `{type: "production_updated", stationId, productionCount}` — fired on every `PRODUCTION_COUNT_UPDATED` MQTT event
- Each REST action **also publishes an MQTT message** (`andon/v1/device/{deviceId}/state`, new topic — nothing currently subscribes to it) reflecting the change, so the firmware has something real to subscribe to *whenever* that gets built (see §8) — this dashboard doesn't wait for firmware subscribe support to be useful on its own via the web view, but shouldn't design itself into a corner that makes wiring firmware up later harder.

**Frontend**: one page, no login.

- **Active board**: cards/rows for incidents in `OPEN`/`ACKNOWLEDGED`/`HANDLING`, grouped/sortable by station, each showing category, reason, station, age (live-ticking), status — matches `agents.md` §11's "show category, station, age, status, owner... without opening detail."
- Each card has the one explicit action valid for its current state (`agents.md` §11: "require explicit action") — Acknowledge on `OPEN`, Start Handling on `ACKNOWLEDGED`, Resolve on `HANDLING` — calling the matching REST endpoint above.
- Resolved incidents drop into a simple "Recently resolved" list (not full history/filtering — that's WEB-004, out of scope here).
- A small production-count tile per known station, **updating live off the WebSocket feed, no polling** — the whole point of the request that prompted this section: the count on screen should move the instant `PRODUCTION_COUNT_UPDATED` arrives, same as the active board.
- WebSocket-fed live updates; falls back to the `GET /api/v1/incidents` list on reconnect (`agents.md` §11: "use live updates but reconcile from REST after reconnect" — cheap to honor even at demo scope, and it's the one architectural rule here that's genuinely free to get right from day one).

## 3. Stack

- **Backend**: stays inside the existing `backend/` Fastify app (same process, same `docker-compose.yml`) — add routes/websocket there rather than standing up a second service. Consistent with `architectur.md` §3's MVP guidance ("may run in one Node.js deployment... do not split into microservices before load justifies it").
- **Frontend**: plain Vite + TypeScript, no framework. A demo with one page, a handful of cards, and a WebSocket listener doesn't need React/etc.'s overhead, and it keeps the whole `dashboard/` package dependency-light like `backend/` already is. Revisit if/when WEB-003's admin CRUD screens actually get built (that's a real multi-view app; this isn't yet).
- Served via `vite build` static output, added as a third service in `deploy/docker-compose.yml` (nginx or a trivial static server) — or just `vite dev` against the backend's published port for the demo itself; decide when it's time to actually wire the compose file.

## 4. Screen inventory

| Area | Shows | Actions |
|---|---|---|
| Header | Connection status (WebSocket connected/reconnecting), station filter (if >1 station appears) | — |
| **Alert banner** | Fires on a brand-new call only (see §4.1) — category + station + reason, category-colored | Dismiss (or auto-dismiss ~8s) |
| Active board | Incident cards: category badge (color per `design.md` §5.1's category tokens, reused for visual consistency with the terminal), reason, station, live age timer, status pill | Acknowledge / Start Handling / Resolve (one per card, matching current state) |
| Production tile(s) | Latest count per station, last-updated timestamp, **updates live off WebSocket push (no polling)** | — |
| Recently resolved | Last N resolved incidents (downtime, handled-by placeholder — no real user/auth yet so this is just "resolved via dashboard") | — |

### 4.1 Andon-call alert

The active board alone (a card quietly appearing in a list) doesn't read as "someone needs help right now" - a real Andon call should interrupt whoever's looking at the dashboard, matching the physical world it's replacing (a light/alarm that demands attention, not a line item). On `incident_created` (never on `incident_updated` - an Ack/Handling/Resolve transition shouldn't re-alert):

- A prominent banner/toast, category-colored (reuse `design.md` §5.1's `fault`/`quality`/`material`/`waiting` tokens - same visual language as the terminal, per `agents.md` §11 "keep operator terminal wording consistent with HMI labels"), showing category + station + reason.
- Auto-dismisses after ~8s - it's a *notice*, not the record of the incident. The incident itself stays fully visible and actionable in the active board regardless of whether the toast was seen or already faded; the board, not the toast, is the source of truth for "still needs attention."
- Audio alert: worth having but flagged as a stretch item, not baseline - browsers block autoplaying sound before any user interaction on the page, so a first-load call could arrive silently no matter what's built. Needs either an explicit "enable sound" affordance or accepting that constraint; decide when this gets built, not speculatively now.

## 5. Data flow

```
ESP32 --MQTT ANDON_REQUESTED--> backend --stores incident, replies COMMAND_RESULT--> ESP32
                                    |
                                    +--broadcasts incident_created--> dashboard (WebSocket, triggers §4.1's alert)

dashboard --POST /incidents/:id/acknowledge--> backend --updates record-->
    +--broadcasts incident_updated--> all dashboard clients (no alert - see §4.1)
    +--publishes andon/v1/device/{id}/state--> MQTT (unconsumed by firmware today, see §8)
```

## 6. Deliberate deviations from agents.md / architectur.md

- No auth (`agents.md` §12) — anyone reaching the dashboard's URL can acknowledge/resolve anything. Matches `backend/`'s existing "LAN-only test tool" posture; do not deploy beyond local dev.
- No PostgreSQL/migrations/audit trail/optimistic versioning (`architectur.md` §10.2) — in-memory only, resets on backend restart, no `incident_events` timeline.
- No outbox pattern, no idempotent-retry story for the dashboard's own POST actions (unlike the device side's `submitAndonRequest()`, a double-tapped Acknowledge button just runs twice — harmless against an in-memory store with no side effects beyond a status field, but wouldn't be safe against the production data model).
- `POST .../start-handling` and `.../resolve` don't enforce `architectur.md` §7's state-machine legality server-side yet (e.g. resolving an `OPEN` incident that was never acknowledged) — trust the frontend to only show the one valid action per card. Production needs this validated backend-side per `agents.md`'s own review checklist ("Can duplicate delivery produce duplicate incidents or side effects?").
- Single global broadcast, no per-user/per-scope filtering (`agents.md` §11: "Enforce permissions in backend; hiding a button is not authorization" — there's no permission model to enforce yet, so this is a wider deviation than "not done," it's "not applicable until auth exists").

## 7. Explicitly out of scope for this plan

- WEB-002's "authorized users" / any RBAC — there are no users yet.
- WEB-003 admin CRUD (stations, devices, reasons, teams, escalation rules).
- WEB-004 history filters + CSV export — only "recently resolved," no date range/search/export.
- WEB-005 KPIs (MTTA, MTTR, downtime, frequency, recurrence) — needs real timestamps/aggregation this in-memory store doesn't retain long enough to compute meaningfully.
- `confirm-close`, `reopen`, `cancel` actions (`architectur.md` §9) — resolve is the last state this pass reaches; closing is the *operator's* confirmation (SCR-06 `CONFIRM & RUN`), which is a device-side action already built (though also still local-mock, see `firmware/PLAN.md` §10) — wiring the two ends together is a follow-on, not this plan.
- Notifications/escalation (`architectur.md` §14).
- Mobile-specific responsive polish (`agents.md` §11 asks for it eventually) — build for desktop first, revisit once the desktop flow is proven.

## 8. Known gap this plan intentionally doesn't close

The firmware's SCR-05 "Technician on the way" / "Issue being handled" progression has **no MQTT subscribe capability today** — `andon_mqtt.hpp` only publishes (see `firmware/PLAN.md` §10). This dashboard's Acknowledge/Start Handling actions will publish a real `andon/v1/device/{id}/state` MQTT message (§2), but until firmware grows a subscriber, the physical terminal's `(DEMO) SIMULATE TECHNICIAN ACK` button remains the only way to see that transition on-device. Closing this loop (firmware subscribes, drops the DEMO button) is future work, tracked here so it isn't lost, not part of this plan.

## 9. Build order (phased)

1. Backend: in-memory incident record (replace ID-only map), `GET /api/v1/incidents`, `GET /api/v1/production`.
2. Backend: WebSocket broadcast on incident create (already happens via MQTT today) and on production update.
3. Backend: the three POST action endpoints, each updating the record and broadcasting.
4. Frontend: Vite scaffold, WebSocket client with reconnect + REST reconcile, static active-board render (no actions yet) against steps 1–2.
5. Frontend: wire the three action buttons to step 3's endpoints.
6. Frontend: production tiles + recently-resolved list.
7. `deploy/docker-compose.yml`: add the dashboard as a served static service; verify the whole loop (ESP32 → backend → dashboard, dashboard action → backend, in both directions) end-to-end on real hardware + real browser.
