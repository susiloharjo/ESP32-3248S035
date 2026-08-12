// Copy this file to secrets.h and fill in your real values.
// secrets.h is gitignored - it never gets committed.
#pragma once

// `static` matters here, unlike in examples/gemini-chatbot's single-.cpp
// version of this file: secrets.h is #included from more than one .cpp in
// this firmware (andon_wifi.cpp AND andon_config.cpp), and `const char*`
// (a non-const pointer to const data) has external linkage by default -
// without `static`, each translation unit's copy of these variables
// collides at link time ("multiple definition of `WIFI_SSID'" etc).
// `static` gives each TU its own private copy instead.

// WiFi credentials - used by AndonWifi::connectSavedOrFallback() (see
// andon_wifi.cpp) as the fallback when nothing's saved yet in NVS.
// Deviates from design.md's original "hidden admin gesture/physical
// provisioning, not the operator flow" plan - explicit user instruction,
// see andon_wifi.hpp's header comment - but flash-time secrets.h is also a
// valid seed/fallback even with an on-screen setup screen: leave
// WIFI_SSID empty to boot with no fallback configured (the on-screen setup
// screen, or an already-saved NVS network, still work either way).
static const char* WIFI_SSID = "";
static const char* WIFI_PASSWORD = "";

// Backend station-configuration endpoint (architectur.md
// GET /api/v1/configuration/stations/{stationId} - see
// contracts/http/v1/get-configuration-stations.md). No trailing slash.
// Same host as MQTT_BROKER_HOST below when running backend/'s test server
// (deploy/docker-compose.yml exposes both HTTP 8080 and MQTT 1883 on that
// one host) - use your machine's LAN IP, not localhost/127.0.0.1, since
// the device is a separate machine on the network.
static const char* CONFIG_API_BASE_URL = "http://192.168.1.100:8080";
static const char* STATION_ID = "STATION-01";
static const char* PLANT_ID = "PLANT-01";

// MQTT broker - used by AndonMqtt::submitAndonRequest() (see
// andon_mqtt.cpp) for the Send Request flow (architectur.md SS8). Points
// at backend/'s embedded test broker by default (deploy/docker-compose.yml).
static const char* MQTT_BROKER_HOST = "192.168.1.100";
static const int MQTT_BROKER_PORT = 1883;
