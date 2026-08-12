# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A board support package (BSP) for the Sunton ESP32-3248S035(C) dev board (ESP32-WROOM-32 MCU, 3.5" 480x320 ST7796 SPI LCD, GT911 I²C capacitive touch, RGB LED, PWM audio amp, photoresistor). The BSP proper lives in [include/](include/) and is meant to drive peripherals directly (no TFT_eSPI/LovyanGFX) via lvgl 9.0. The actual working application code lives in [examples/](examples/) as separate, self-contained PlatformIO projects that do **not** use the BSP — see "Two codebases" below. There are currently two:

- [examples/gemini-chatbot/](examples/gemini-chatbot/) — on-device Gemini AI chat companion (single PlatformIO project).
- [examples/andon-system/](examples/andon-system/) — a multi-component "Digital Andon" shop-floor system (ESP32 terminal + backend + dashboard); currently only its `firmware/` component exists. This one has its own spec docs and agent instructions — see its section below before touching it.

## Build / run

There is no root-level build; everything buildable lives under each example's own directory.

```bash
cd examples/gemini-chatbot            # or examples/andon-system/firmware
pio run                 # build
pio run -t upload       # flash to device
pio run -t upload -t monitor   # flash and open serial monitor (115200 baud)
pio device monitor -b 115200   # serial monitor only
```

Neither example has unit tests configured (`test/` is the empty PlatformIO scaffold) or lint/format tooling.

## Two codebases — don't conflate them

1. **`include/ESP323248S035.hpp` + `include/bsp_view.hpp`** — the actual library/BSP. Template-based `Controller` classes (`TPC_LCD`, `RGB_PWM`, `AMP_PWM`, `CDS_ADC`) each implement `init()`/`update(elapsed)`, composed via an `Atomic<>` CRTP mixin for critical-section-guarded setters. The application hooks in by subclassing `bsp::View` (see `examples/gemini-chatbot/include/main.hpp` for the intended pattern: define a `View`, instantiate `bsp::ESP323248S035C<YourView> target(view)`). Neither example below is currently wired into this pattern.
2. **`examples/gemini-chatbot/src/main.cpp`** — an on-device Gemini AI chat companion with a T9 feature-phone-style keypad UI and its own WiFi manager (no captive portal — scan/select/type-password all happen on the touchscreen). This is a large, mostly flat single-file Arduino sketch that talks to hardware directly:
   - Hand-rolled bit-banged I²C driver for the GT911 touch controller (functions prefixed `IIC_`/`GT911_`), not using the BSP's touch support, plus an on-device 2-point touch calibration flow (`g_touchCalib`).
   - `TFT_eSPI` (vendored in `examples/gemini-chatbot/lib/TFT_eSPI`) for the display instead of the BSP's `TPC_LCD`.
   - lvgl 8.3.3 (vendored in `examples/gemini-chatbot/lib/lvgl`, configured via `examples/gemini-chatbot/lib/lvgl/lv_conf.h`) for the chat UI, built with `LV_USE_LOG`/draw-buffer setup in `main.cpp` directly. Note: despite `library.json` pinning lvgl 9.x for BSP consumers, this example vendors and builds against 8.3.3 — v8 API only (e.g. `lv_textarea_del_char`, no `LV_BTNMATRIX_CTRL_WIDTH`).
   - `WiFi.h` + `HTTPClient` + `ArduinoJson` + `Preferences` (NVS) for the on-device WiFi setup screen and for calling the Gemini Interactions API (`queryGemini()`), which carries multi-turn conversation memory server-side via `previous_interaction_id`.
3. **`examples/andon-system/firmware/src/main.cpp`** — the ESP32 terminal for the Digital Andon system (see below). GT911 driver, on-device touch calibration, and lvgl/TFT_eSPI plumbing carried over from `gemini-chatbot` (vendored lvgl 8.3.3 + TFT_eSPI again), plus the full 6-screen operator UI (design.md-driven, mostly local state), a `WiFi`/`HTTPClient`/`ArduinoJson` config-sync module (`andon_config.cpp`/`.hpp`) that fetches per-category assistance reasons+codes from `backend/`'s config endpoint (NVS-cache and hardcoded-placeholder fallbacks), an on-device WiFi setup screen (`andon_wifi.cpp`/`.hpp`) ported from `gemini-chatbot`'s WiFi manager (T9 keypad scan/select/password, NVS-saved network) — freely reachable from SCR-01's gear icon, a deliberate deviation from design.md's original admin-only intent (explicit user instruction, recorded in design.md) — and an MQTT client (`andon_mqtt.cpp`/`.hpp`, PubSubClient, QoS 0 only despite architectur.md asking for QoS 1) that submits Send Request (`ANDON_REQUESTED`) and the production-count screen's update (`PRODUCTION_COUNT_UPDATED`, an ad-hoc extension not in architectur.md - see that doc's §8.1 note) to `backend/`, blocking briefly for its COMMAND_RESULT before ever claiming backend-accepted.

When asked to change chat/keypad/WiFi-manager behavior, edit `examples/gemini-chatbot/src/main.cpp` directly.

## `examples/andon-system/` — read its own docs first

This example is a full "Digital Andon" shop-floor incident system, not just firmware: an ESP32 touchscreen terminal, a Node.js/Fastify + MQTT + PostgreSQL backend, and a supervisor web dashboard. It's specced independently of this file, with its own governing docs at the example root:

- `examples/andon-system/agents.md` — **read this first**, before changing anything under `examples/andon-system/`. It defines a stricter read order (agents.md → PRD.md → design.md → architectur.md → ADRs), non-negotiable safety/scope rules, the intended repo layout (`firmware/`, `backend/`, `dashboard/`, `contracts/`, `deploy/`, `docs/adr/` as siblings under `examples/andon-system/`), and a required change-handoff report format. Its rules take precedence over generic conventions in this file for anything inside that directory.
- `examples/andon-system/PRD.md`, `design.md`, `architectur.md` — product scope, HMI/UI design spec, and technical architecture (incident state machine, MQTT topic/contract rules, etc).

**Documentation must stay in sync with the code, every time — this is not optional.** `agents.md` §16 lists exactly what triggers a required doc update in the same change (screen flows/labels/colors/touch interactions, incident states, MQTT/HTTP contracts, DB model, deployment/secrets processes, safety boundaries). In practice: touch `design.md` when a screen changes, `architectur.md`/`contracts/` when a payload or endpoint changes, and this file (`CLAUDE.md`) when the *description of what exists* (scope/architecture-level facts, not andon-system's own screen/contract details) goes stale — e.g. "no UI built yet" or "no WiFi dependency" statements here rot fast once real work lands, as just happened. When a change deviates from a written spec on explicit user instruction, don't silently edit the doc to match — mark the change in the doc itself (what changed, why, precedence) per `agents.md` §2/§16, the way design.md's SCR-01 section now does.

`firmware/` (UI + config sync + MQTT client, see above), `contracts/` (versioned HTTP contract for the config-sync endpoint, now implemented by `backend/`), `backend/` (Node/TS/Fastify + an embedded `aedes` MQTT broker — **test harness only**, in-memory, no auth/PostgreSQL, not the production backend `agents.md` §10 specs; see that package's `src/server.ts` header comment for the full scope note), and `deploy/` (`docker-compose.yml` running `backend/` — `docker compose up --build` from that directory) all exist now. `dashboard/` hasn't been created yet.

## Known issue: hardcoded credentials

`examples/gemini-chatbot/src/main.cpp` had WiFi SSID/password and an OpenWeatherMap API key hardcoded as `const char*` literals near the top of the file (`WIFI_SSID`, `WIFI_PASSWORD`, `WEATHER_API_KEY`) — these are committed to git history from an earlier version of that example. WiFi credentials are now entered on-device and stored in NVS instead. The Gemini API key lives in `examples/gemini-chatbot/include/secrets.h` (gitignored; see `secrets.example.h` for the template). Do not add further real secrets in tracked files — prefer flagging it to the user rather than propagating the pattern if editing that region.

## Build flags (espressif32/arduino target)

Each example's `platformio.ini` sets board `esp32dev` and `board_build.partitions = huge_app.csv` (needed once WiFi/HTTPClient/ArduinoJson/lvgl are combined in one firmware — both `gemini-chatbot` and, since its config-sync module was added, `andon-system/firmware` need this now). The library-level `library.json` (for BSP consumers) pins `-std=gnu++17`, `LV_CONF_INCLUDE_SIMPLE=1`, `LV_CONF_SKIP=1`, `LV_COLOR_DEPTH=16`, and lvgl `^9.1.0` — each example instead vendors its own lvgl 8.3.3 and supplies its own `lv_conf.h` under `lib/lvgl/`.

## Pin reference

`docs/` has the datasheet and pinout diagrams; the GPIO capability table in [README.md](README.md) is the quick reference before assuming any pin is free (many are I2S/strapping/ADC2-constrained). Peripheral pin assignments used by the BSP classes are in `include/ESP323248S035.hpp` (e.g. RGB LED on GPIO 25/26/27, amp on GPIO 32); each example's touch driver uses its own pin set defined near the top of `main.cpp` (`IIC_SCL`/`IIC_SDA`/`IIC_RST` = 32/33/25) — note these differ from and can collide with the BSP's assumptions, since neither example routes through the BSP.
