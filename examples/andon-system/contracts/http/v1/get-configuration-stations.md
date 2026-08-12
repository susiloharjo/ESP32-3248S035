# `GET /api/v1/configuration/stations/{stationId}`

Status: **draft** - defines what the firmware (`firmware/src/andon_config.cpp`,
`AndonConfig::sync()`) expects; the backend that serves this endpoint does
not exist yet (see `agents.md` repository ownership table - `backend/`
hasn't been created). Authored so both sides converge on the same shape
once the backend is built, per `agents.md` §8's contract rules ("Never
change MQTT or HTTP payloads without updating versioned contracts and
tests").

Source: `architectur.md` §9 REST API table (`configuration: categories,
reasons, resolutions, UI config, versioning`) and PRD.md AND-002/AND-003.

## Scope of this version

PRD.md AND-002 fixes the four assistance categories themselves (identity,
icon, color) - only AND-003 says their **reason lists** are plant/line
configurable. This version of the endpoint therefore only needs to carry
`categories[].reasons` for the firmware's purposes; other fields
(`resolutions`, broader `UI config`, per-category icon/color/label
overrides) are explicitly **not yet consumed** by the firmware and may be
added later as additive optional fields without a firmware change (see
Compatibility below).

## Request

```
GET /api/v1/configuration/stations/{stationId}
```

No request body. Per `architectur.md` §9: device flow uses device
credentials (not yet implemented by the firmware - this first pass calls
the endpoint unauthenticated; adding the device credential header is a
follow-up, see the firmware change's handoff report).

## Response `200 OK`

```json
{
  "configVersion": 3,
  "stationId": "STATION-01",
  "categories": [
    {
      "code": "MAINTENANCE",
      "reasons": [
        { "code": "MACHINE_JAM", "label": "Machine jam" },
        { "code": "CUTTING_FAULT", "label": "Cutting fault" },
        { "code": "WELDING_FAULT", "label": "Welding fault" },
        { "code": "SENSOR_FAULT", "label": "Sensor fault" },
        { "code": "UTILITY", "label": "Utility" },
        { "code": "OTHER", "label": "Other" }
      ]
    },
    { "code": "QUALITY", "reasons": [ /* same shape */ ] },
    { "code": "MATERIAL", "reasons": [ /* same shape */ ] },
    { "code": "SUPERVISOR", "reasons": [ /* same shape */ ] }
  ]
}
```

| Field | Type | Notes |
|---|---|---|
| `configVersion` | integer | Not yet used by the firmware (no conditional/`If-None-Match`-style refresh yet) - reserved so a future firmware version can skip re-applying an unchanged config. |
| `stationId` | string | Echo of the requested station; not validated by the firmware in this version. |
| `categories[].code` | string | Must match one of `MAINTENANCE`, `QUALITY`, `MATERIAL`, `SUPERVISOR` (PRD.md AND-002). An unrecognized code is skipped, not rejected - the rest of the document still applies. |
| `categories[].reasons[].code` | string | Not currently stored/used by the firmware (only `.label` is rendered) - required in the shape for forward compatibility with the backend's own taxonomy IDs. |
| `categories[].reasons[].label` | string | Rendered verbatim on the SCR-03 reason tiles. Max **39 characters** (firmware buffer is 40 bytes including the null terminator - see `ANDON_CFG_LABEL_LEN` in `andon_config.cpp`); longer labels are silently truncated by `strlcpy`. |

Limits enforced firmware-side (`andon_config.cpp`):
- At most **8 reasons per category** (`ANDON_CFG_MAX_REASONS`) - extras are
  logged and dropped, not an error.
- A category with zero valid reasons in the response is left untouched
  (keeps its previous/cached/placeholder reasons) rather than being
  blanked out.

## Compatibility

- Additive optional fields (e.g. per-reason `code` beyond what's used
  today, `resolutions`, a broader `uiConfig` object) are safe to add
  without a firmware change - the firmware only reads
  `categories[].code`/`categories[].reasons[].label` and ignores everything
  else in the document.
- A schema-breaking change (renaming/removing `categories`, `code`, or
  `reasons[].label`, or changing their types) requires a new major version
  of this endpoint per `agents.md` §8.

## Firmware behavior on non-conforming responses

| Condition | Behavior |
|---|---|
| Non-200 HTTP status | Logged, response body ignored, previous config kept. |
| Body isn't valid JSON | Logged (parse error), previous config kept. |
| No `categories` array | Logged, previous config kept. |
| A category `code` doesn't match any of the 4 known categories | That category entry skipped; rest of the document still applied. |
| A category has no valid `reasons` | That category's existing reasons kept; other categories in the same response still applied. |

See `firmware/include/andon_config.hpp` for the full fallback chain (NVS
cache, then live fetch, compiled-in placeholders otherwise).

## Example fixture

[`get-configuration-stations.example.json`](get-configuration-stations.example.json)
is a complete example response using the same placeholder reason text
currently hardcoded in `firmware/src/main.cpp` (`REASONS_MAINTENANCE`
etc.) - useful for testing the firmware against a trivial static file
server (e.g. `python3 -m http.server` pointed at this directory) before a
real backend exists.
