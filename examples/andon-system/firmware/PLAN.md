# Firmware UI Implementation Plan — Digital Andon Terminal

**Status:** All 6 screens + SCR-04B + both confirmation dialogs implemented in `src/main.cpp` and flashed to the board (build order §7 steps 1–6 done in one pass rather than incrementally, for speed — step 7's on-device validation pass against design.md §14 is still pending real tap-testing).

**2026-08-13 update — this plan's original "UI-only, no MQTT/backend" scope (§1, §8, §9 below) is no longer what's shipping.** A real `backend/` + MQTT test broker landed (see CLAUDE.md's andon-system section for the up-to-date architecture description) and this firmware now talks to it for some flows. Sections 1–9 are kept as the historical record of the original UI-only build; **§10 below is the current source of truth for what's actually wired vs. still stubbed.** Don't restart from §1's "no real MQTT/backend calls" framing.

| Field | Value |
|---|---|
| Scope | `examples/andon-system/firmware/` only — the ESP32/CYD touchscreen UI |
| Out of scope | Backend (Node.js/Fastify), MQTT broker, PostgreSQL, dashboard — built separately, elsewhere |
| Reference docs | [design.md](../design.md) (screen spec, tokens, state machine), [PRD.md](../PRD.md) (categories/flow), [`ChatGPT Image Aug 11, 2026, 07_12_17 PM.png`](../ChatGPT%20Image%20Aug%2011%2C%202026%2C%2007_12_17%20PM.png) (visual layout reference) |
| Precedence note | [agents.md](../agents.md) governs the full monorepo (firmware + backend + dashboard). This plan narrows *this repo's* deliverable to firmware UI only — see "Deliberate deviations from agents.md" below. |

## 1. Goal

Implement the 6-screen Andon operator flow on the CYD board (480×320, GT911 touch), pixel-faithful to the reference image and `design.md`'s screen specs (§7) and visual tokens (§5), running entirely on **mocked local state** — no real MQTT/backend calls. The point of this build is to prove out and demo the *touch UI/UX*, not the distributed system.

## 2. What already exists (don't rebuild)

`src/main.cpp` currently has, carried over from `examples/gemini-chatbot`:

- GT911 bit-banged I²C touch driver (`GT911_Scan`, `readRawTouch`)
- On-device 2-point touch calibration (`runTouchCalibration`, `g_touchCalib`) — keep as the first thing `setup()` runs
- lvgl 8.3.3 + TFT_eSPI display/indev plumbing (`my_disp_flush`, `my_touchpad_read`, draw buffer setup)
- A throwaway placeholder screen (`createPlaceholderScreen`) — **delete this**, it gets replaced by SCR-01

Everything else (screens, state, styling) is new.

## 3. Screen inventory

Maps directly to the reference image's numbered frames and `design.md` §7:

| # | Screen ID | Purpose | Reference |
|---|---|---|---|
| 1 | SCR-01 Normal status | `LINE RUNNING`, work order/production/rate, `NEED ASSISTANCE` | image frame 1 |
| 2 | SCR-02 Category selection | `CALL ANDON`, 2×2 tiles (Maintenance/Quality/Material/Supervisor), `CANCEL` | image frame 2 |
| 3 | SCR-03 Reason selection | Category-specific reason grid, `BACK` / `SEND REQUEST` | image frame 3 |
| 4 | SCR-04 Request active | Red banner, elapsed timer, `CANCEL REQUEST` / `ADD NOTE` | image frame 4 |
| — | SCR-04B Queued offline | Amber `QUEUED OFFLINE` variant of SCR-04 | design.md §7 only (not in image) — build after SCR-04, reuses its layout |
| 5 | SCR-05 Acknowledged/handling | Amber `TECHNICIAN ON THE WAY`, responder card, stepper, `UPDATE STATUS` | image frame 5 |
| 6 | SCR-06 Resolved confirmation | Green `ISSUE RESOLVED`, summary, `REOPEN` / `CONFIRM & RUN` | image frame 6 |

Plus shared chrome (build once, reuse everywhere):

- **Global header** (40px): station label, shift, WiFi/MQTT dots (mocked, static for now), clock — design.md §6
- **Send confirmation dialog** and **cancel confirmation dialog** — design.md §8.1–8.2

## 4. Navigation / state machine

Reuse `design.md` §9's state machine verbatim, driven by a local mock struct instead of backend events:

```
Normal → Category → Reason → Submitting → Active ⇄ QueuedOffline
Active → Acknowledged → Handling → Resolved → Normal (confirm) | Handling (reopen)
```

`Submitting` picks `Active` or `QueuedOffline` based on a **fake connectivity toggle** (see §6) rather than a real network check — this exists so the offline-path UI can be demoed without needing a broker.

## 5. Implementation architecture

- **Single app shell, swappable content container** (design.md §11): one root screen object stays alive; each `showScreenXxx()` function does `lv_obj_clean(content_container)` and rebuilds just that region — not the header, not the whole `lv_scr_act()`. This avoids re-registering the indev/rebuilding the calibration every transition.
- **One `AndonState` struct** holding: current screen enum, selected category/reason, incident open time, responder name/id, downtime, connectivity mock flag. All screens render *from* this struct — no screen reads another screen's widgets directly.
- **Centralized style objects** (design.md §11): one `lv_style_t` per token in §5.1/§5.2, created once in `setup()`, reused via `lv_obj_add_style` — not rebuilt per screen.
- **Timer**: one `lv_timer_t` ticking at 1Hz that updates the header clock and, when on SCR-04/SCR-05, the elapsed/wait counters. No per-frame redraws (design.md §12).
- **Backend integration seam**: every place the real system would send an MQTT command (`sendRequest()`, `cancelRequest()`, `confirmRun()`, `reopen()`, `updateStatus()`) is a stub function that just mutates `AndonState` and re-renders — clearly marked `// TODO(backend): replace with MQTT publish, see architectur.md §8 MQTT contract` so wiring it up later is a search-and-replace, not a redesign.

## 6. Mock data plan

Since there's no backend, seed `AndonState` with static fixtures matching the image exactly, so the demo output is directly comparable to the reference:

- Station: `PIPE LINE 02`, Shift: `SHIFT A`
- Work order `WO-240811-07`, production `72 / 120`, rate `18 pcs/h`
- Categories: Maintenance (red), Quality (purple), Material (blue), Supervisor (amber) — colors from design.md §5.1 (`fault`/`quality`/`material`/`waiting`)
- Maintenance reasons: Machine jam, Cutting fault, Welding fault, Sensor fault, Utility, Other (design.md §7 SCR-03; Quality/Material/Supervisor reason sets aren't specified anywhere yet — invent a plausible placeholder 6-tile set per category, consistent in tone, and flag them as placeholders pending the real configurable taxonomy from PRD.md AND-003)
- Technician fixture: `BUDI` / `MT-04`
- A boolean `g_mockConnected` toggled by a hidden long-press on the header (dev-only gesture) to demo the QueuedOffline path on demand, instead of needing real WiFi loss

## 7. Build order (phased)

1. Global header + style/token constants + `AndonState` skeleton
2. SCR-01 (simplest, also the flow's start/end point)
3. SCR-02 → SCR-03 → send confirmation dialog → SCR-04
4. SCR-04B (offline variant, toggled via the dev gesture in §6)
5. SCR-05 (stepper + responder card)
6. SCR-06 + cancel confirmation dialog + reopen path back to SCR-05/Handling
7. Pass over design.md §14 validation checklist (touch target sizes, offline visibly distinct, resolution doesn't auto-imply `LINE RUNNING`, etc.) — the items that apply to UI-only (skip the backend/hardware-glove items that need real integration)

Each step should be flashed and tap-tested on the actual board before moving to the next — touch calibration and dead-zone behavior from `gemini-chatbot` are proven but the new touch targets (2×2/3×2 grids) are not yet verified on real glass.

## 8. Deliberate deviations from agents.md

`agents.md` assumes firmware ships alongside a live backend and holds the project to full contract/testing/observability rules. For this UI-only deliverable:

- No MQTT client, no `contracts/` schema validation, no idempotency keys — those apply once a real backend exists.
- "Backend-accepted state overrides cached projections" (agents.md §9 Firmware rules) doesn't apply yet; the mock struct *is* the source of truth for now.
- Skip agents.md §13 testing requirements that need a broker/backend (offline/retry/duplicate-delivery tests) — those move to whichever repo builds the API demo.
- Everything about touch targets, one-screen-one-job, color+text+icon, and operator-confirms-recovery (agents.md §9 UI rules, design.md §2) still fully applies — those are UI-only concerns.

If/when this firmware is wired to a real backend, re-read `agents.md` in full before adding networking code.

## 9. Explicitly out of scope for this plan

- `backend/`, `dashboard/`, `contracts/`, `deploy/` directories
- Real MQTT (`andon/v1/...` topics), REST API calls, PostgreSQL
- NVS-persisted incident state across reboot (design.md's "restart recovery" requirement — revisit once state has somewhere durable to reconcile against)
- Multi-language support (design.md §13) — English labels only for the first pass

**Superseded by §10 below** — `backend/`, `contracts/`, `deploy/` now exist and real MQTT/REST calls are wired for two flows. `dashboard/` and NVS-persisted incident state are still genuinely not started. Multi-language is still not started either.

## 10. Current implementation status (supersedes §1/§8/§9 above)

Backend/MQTT integration landed via the `wifi-manager-improvements` merge (2026-08-13) plus follow-on work in this repo. This section is the thing to update going forward — see agents.md §16, "documentation must stay in sync with the code, every time."

**Wired to the real backend:**
- Config sync — `andon_config.cpp` fetches per-category reasons from `GET /api/v1/configuration/stations/{stationId}` at boot, NVS-cached, placeholder fallback if unreachable
- On-device WiFi setup — `andon_wifi.cpp`, ported from gemini-chatbot (T9 scan/select/password, NVS-saved network)
- On-device Server IP config — `andon_wifi.cpp`'s password-screen "Server" tab (same T9 keypad, switches target textarea), NVS-saved, falls back to `secrets.h`
- **Send Request** (SCR-03 → SCR-04) — `AndonMqtt::submitAndonRequest()`, blocks for the backend's `COMMAND_RESULT`, only reports success on an actual `ACCEPTED`
- **Production count update** — `AndonMqtt::submitProductionUpdate()`, same accept/reject mechanics (ad-hoc extension, not in architectur.md §8 - see that file's own note)

**Still local mock state only** (no MQTT call at all — `TODO(backend)` comments mark these in `main.cpp`):
- `CANCEL REQUEST` (SCR-04)
- `ADD NOTE` (SCR-04) — literal no-op, not even a preset-note list yet
- `UPDATE STATUS` / the Ack→Handling progression (SCR-05) — architecturally this should arrive *from* the backend (a technician's own device acknowledging), which needs MQTT **subscribe**; only publish exists so far, so `(DEMO) SIMULATE TECHNICIAN ACK` is still standing in for that
- `CONFIRM & RUN` (SCR-06 resolve/close)
- SCR-04B QueuedOffline isn't actually reached by a real MQTT failure yet (`submitAndonRequest()` just reports failure on timeout)

**Reliability gaps already flagged in `andon_mqtt.hpp`'s own scope note** (deliberate, for this MVP test slice — not oversights): no persisted offline queue, no retry-with-backoff, no idempotency key surviving a reboot, no TLS/device credentials. All required for production per agents.md §9/§12.

**Not done from this plan's original §7 build order:** step 7 (pass design.md §14's on-device validation checklist) has never been formally worked through.

**Pending, not yet pulled into this repo:** a work-order list (SCR-01's WORK ORDER card is still the static `WO-240811-07` placeholder, non-clickable) and an "andon fetch from API" piece — both reportedly done at the office, not yet pushed as of 2026-08-13.

**`examples/demo-hub/`** (the combined chat+andon mode-select firmware) was branched *before* all of the above landed — it has none of it (no backend/MQTT, no Server tab, old WiFi-only andon UI). Needs a manual re-sync if it should carry this work too; it isn't automatic.
