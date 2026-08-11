# Firmware UI Implementation Plan — Digital Andon Terminal

**Status:** All 6 screens + SCR-04B + both confirmation dialogs implemented in `src/main.cpp` and flashed to the board (build order §7 steps 1–6 done in one pass rather than incrementally, for speed — step 7's on-device validation pass against design.md §14 is still pending real tap-testing).

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
