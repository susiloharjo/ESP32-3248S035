# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A board support package (BSP) for the Sunton ESP32-3248S035(C) dev board (ESP32-WROOM-32 MCU, 3.5" 480x320 ST7796 SPI LCD, GT911 I²C capacitive touch, RGB LED, PWM audio amp, photoresistor). The BSP proper lives in [include/](include/) and is meant to drive peripherals directly (no TFT_eSPI/LovyanGFX) via lvgl 9.0. The actual working application code, however, is a standalone PlatformIO demo in [examples/platformio/](examples/platformio/) that does **not** use the BSP — see "Two codebases" below.

## Build / run

There is no root-level build; everything buildable lives under `examples/platformio/`, a self-contained PlatformIO project.

```bash
cd examples/platformio
pio run                 # build
pio run -t upload       # flash to device
pio run -t upload -t monitor   # flash and open serial monitor (115200 baud)
pio device monitor -b 115200   # serial monitor only
```

There are no unit tests configured (`test/` is the empty PlatformIO scaffold) and no lint/format tooling in the repo.

## Two codebases — don't conflate them

1. **`include/ESP323248S035.hpp` + `include/bsp_view.hpp`** — the actual library/BSP. Template-based `Controller` classes (`TPC_LCD`, `RGB_PWM`, `AMP_PWM`, `CDS_ADC`) each implement `init()`/`update(elapsed)`, composed via an `Atomic<>` CRTP mixin for critical-section-guarded setters. The application hooks in by subclassing `bsp::View` (see `examples/platformio/include/main.hpp` for the intended pattern: define a `View`, instantiate `bsp::ESP323248S035C<YourView> target(view)`).
2. **`examples/platformio/src/main.cpp`** — the code actually being developed/iterated on (clock + weather dashboard, per recent commit history). This is a large, mostly flat single-file Arduino sketch that talks to hardware directly:
   - Hand-rolled bit-banged I²C driver for the GT911 touch controller (functions prefixed `IIC_`/`GT911_`), not using the BSP's touch support.
   - `TFT_eSPI` (vendored in `examples/platformio/lib/TFT_eSPI`) for the display instead of the BSP's `TPC_LCD`.
   - lvgl 9 (vendored in `examples/platformio/lib/lvgl`, configured via `examples/platformio/lib/lvgl/lv_conf.h`) for the dashboard UI, built with `LV_USE_LOG`/draw-buffer setup in `main.cpp` directly rather than through `bsp::View`.
   - `WiFi.h` + `HTTPClient` + `ArduinoJson` to poll the OpenWeatherMap forecast API (`fetchWeatherData()`) and an NTP-synced clock.

When asked to change dashboard/clock/weather behavior, edit `examples/platformio/src/main.cpp` directly — `main.hpp`'s `bsp::View`-based `Main` class is not currently wired into that sketch.

## Known issue: hardcoded credentials

`examples/platformio/src/main.cpp` has WiFi SSID/password and an OpenWeatherMap API key hardcoded as `const char*` literals near the top of the file (`WIFI_SSID`, `WIFI_PASSWORD`, `WEATHER_API_KEY`). These are committed to git history. Do not add further real secrets in the same style — prefer flagging it to the user rather than propagating the pattern if editing that region.

## Build flags (espressif32/arduino target)

Set in `examples/platformio/platformio.ini`: board `esp32dev`, `board_build.partitions = huge_app.csv` (needed for the WiFi/HTTPClient/ArduinoJson/lvgl combined firmware size). The library-level `library.json` additionally pins `-std=gnu++17`, `LV_CONF_INCLUDE_SIMPLE=1`, `LV_CONF_SKIP=1`, `LV_COLOR_DEPTH=16` for consumers of the BSP as a PlatformIO library — the example project instead supplies its own `lv_conf.h` under `lib/lvgl/`.

## Pin reference

`docs/` has the datasheet and pinout diagrams; the GPIO capability table in [README.md](README.md) is the quick reference before assuming any pin is free (many are I2S/strapping/ADC2-constrained). Peripheral pin assignments used by the BSP classes are in `include/ESP323248S035.hpp` (e.g. RGB LED on GPIO 25/26/27, amp on GPIO 32); the demo's touch driver uses its own pin set defined near the top of `main.cpp` (`IIC_SCL`/`IIC_SDA`/`IIC_RST` = 32/33/25) — note these differ from and can collide with the BSP's assumptions, since the demo doesn't route through the BSP.
