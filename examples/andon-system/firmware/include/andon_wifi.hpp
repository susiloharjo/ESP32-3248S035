#pragma once

#include <Arduino.h>
#include <lvgl.h>

// On-device WiFi manager, ported from examples/gemini-chatbot's
// src/main.cpp (scan/select/type-password on the touchscreen via a T9
// keypad, NVS-backed single saved network) - explicit user instruction:
// port it as-is (same T9 keypad, same visual style), reachable freely from
// the operator flow via SCR-01's gear icon (see main.cpp's onOpenConfig()).
//
// This is a DEVIATION from design.md's stated rule ("Provide a hidden
// admin gesture or physical provisioning process; do not put settings in
// the operator flow") - recorded here and in design.md per agents.md §2's
// precedence rules (explicit current instruction outranks an existing
// design spec, but the doc must still reflect what actually ships).
//
// gemini-chatbot's version additionally has SD-card-backed multi-network
// storage layered on top of the NVS single entry - deliberately NOT ported
// here (out of scope for "port the WiFi config screen"; would add a new
// hardware dependency - the TF card slot - to andon's firmware without it
// being asked for). Only the NVS single-saved-network tier came over.
namespace AndonWifi {

// Non-interactive, bounded (~10s worst case): tries the NVS-saved network
// first, then secrets.h's WIFI_SSID/WIFI_PASSWORD once as a fallback if
// nothing saved yet or it no longer works. Safe to call from setup() (see
// AndonConfig::sync(), which calls this before its HTTP fetch) - never
// shows the interactive setup screen, never blocks longer than the two
// bounded connect attempts.
bool connectSavedOrFallback(lv_obj_t *statusLabel);

// Sets a flag consumed at the top level of loop() (see main.cpp) - mirrors
// gemini-chatbot's g_wifiSetupRequested pattern exactly, for the same
// reason: this button's own click handler runs SYNCHRONOUSLY INSIDE an
// lv_timer_handler() call, and runSetupFlow() below has its own internal
// lv_timer_handler() polling loop - calling it directly from the click
// handler would be a reentrant call into LVGL, which corrupted indev
// press/release tracking every time it was tried in gemini-chatbot's
// history (see that file's comments on the same bug). Call this from
// SCR-01's gear button; never call runSetupFlow() directly from a widget
// event callback.
void requestSetup();
bool isSetupRequested();
void clearSetupRequest();

// Blocking (like runTouchCalibration()): wipes the whole screen (header
// included) for the scan/select/password flow and keeps LVGL ticking
// until a network connects or the user backs out/skips. Caller (loop(),
// top-level only - see requestSetup()'s comment) is responsible for
// rebuilding the header/content/SCR-01 afterward, since this tears down
// the entire lv_scr_act(), not just the content container. Returns true
// if WiFi ended up connected.
bool runSetupFlow();

// Backend host (bare IP/hostname, no scheme or port), set via the "Server"
// tab on runSetupFlow()'s password screen (same T9 keypad as the WiFi
// password field - see andon_wifi.cpp's onTabToggle()) and NVS-persisted
// separately from the WiFi credentials. Returns "" if never configured -
// callers (AndonConfig::sync(), AndonMqtt) fall back to secrets.h's
// CONFIG_API_BASE_URL/MQTT_BROKER_HOST in that case, composing the URL
// themselves since this is deliberately just the host, not a full URL
// (HTTP port 8080 / MQTT port 1883 stay fixed - see secrets.example.h).
String getServerHost();

} // namespace AndonWifi
