#pragma once

#include <cstdint> // uint32_t/uint8_t - self-contained regardless of include order in callers

// Shared between main.cpp (owns and renders CATEGORIES[]) and
// andon_config.cpp (fetches config from the backend and overwrites
// individual categories' reasons in place - see andon_config.hpp). Kept in
// its own header so neither file has to #include the other.
struct CategoryInfo {
  const char *label; // also doubles as the MQTT/HTTP categoryCode - see architectur.md SS8.2 (e.g. "MAINTENANCE")
  const char *icon; // closest LV_SYMBOL_* match - see main.cpp's note
  uint32_t color;
  const char **reasons;     // display labels, e.g. "Machine jam" - what SCR-03 renders
  const char **reasonCodes; // machine codes, e.g. "MACHINE_JAM" - what AndonMqtt::submitAndonRequest() sends as reasonCode (architectur.md SS8.2). Parallel array, same indices as reasons[].
  uint8_t reasonCount;
};
