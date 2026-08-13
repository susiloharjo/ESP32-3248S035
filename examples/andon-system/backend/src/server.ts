// ---------------------------------------------------------------------------
// Minimal test backend for the Digital Andon firmware + dashboard.
//
// SCOPE (explicit, per agents.md's scope-discipline rule): this is NOT the
// production backend agents.md §10 specs - it's a minimal stand-in so the
// firmware's already-built pieces (GET config fetch - andon_config.cpp -
// and the MQTT Send Request/production-update paths - andon_mqtt.cpp) and
// the dashboard (see ../../dashboard/PLAN.md) have something real to talk
// to end-to-end before the real backend exists.
//
// Deliberately missing, all required for production per agents.md:
//  - PostgreSQL. Config data is an in-memory seed (config-data.ts);
//    incidents/production counts live in incident-store.ts/production-
//    store.ts's in-memory Maps, not persisted, no migrations, nothing
//    survives a restart.
//  - Auth. No device credentials, no MQTT ACLs, no Bearer auth on HTTP,
//    no dashboard login (agents.md §12 requires all of these in
//    production) - anyone who can reach these ports can publish/
//    subscribe/read/act on anything. LAN-only test tool; do not expose
//    this to the internet or use it beyond local dev.
//  - Real idempotency. In-memory Map dedup only (incident-store.ts) - a
//    production backend needs a DB unique constraint (agents.md §7/§10).
//  - Optimistic versioning, audit trail, outbox pattern, escalation rules,
//    notifications, server-side state-machine validation on the dashboard
//    action endpoints below, confirm-close/reopen/cancel (architectur.md
//    §9) - none of this exists here. See dashboard/PLAN.md §6/§7 for the
//    full list of accepted deviations for this slice.
//
// Do not deploy this anywhere real - it's a local dev/test harness only.
// ---------------------------------------------------------------------------

import { randomUUID } from "node:crypto";
import { createServer } from "node:net";
import Fastify from "fastify";
import websocketPlugin from "@fastify/websocket";
import Aedes from "aedes";
import type { AedesPublishPacket, Client } from "aedes";
import { STATION_CONFIG } from "./config-data";
import { createOrGetIncident, listIncidents, acknowledgeIncident, startHandlingIncident, resolveIncident } from "./incident-store";
import { setProductionCount, listProductionCounts, getProductionCount } from "./production-store";
import { listWorkOrders, listAllWorkOrders } from "./work-order-store";
import { registerClient, broadcast } from "./realtime";

const HTTP_PORT = Number(process.env.HTTP_PORT ?? 8080);
const MQTT_PORT = Number(process.env.MQTT_PORT ?? 1883);

// --- MQTT (aedes, embedded broker - no separate Mosquitto to install) -------
// Declared before the HTTP routes below since a couple of them (the
// dashboard action endpoints) publish back onto it.

const broker = new Aedes();
const mqttServer = createServer(broker.handle);

interface AndonEvent {
  schemaVersion: number;
  eventId: string;
  eventType: string;
  idempotencyKey: string;
  deviceId: string;
  stationId: string;
  deviceTimestamp: string;
  sequence: number;
  correlationId: string;
  payload: Record<string, unknown>;
}

function publishResult(deviceId: string, correlationId: string, incidentId: string) {
  const result = {
    schemaVersion: 1,
    eventId: randomUUID(),
    eventType: "COMMAND_RESULT",
    correlationId,
    status: "ACCEPTED",
    incidentId,
    serverTimestamp: new Date().toISOString(),
    incidentStatus: "OPEN",
    version: 1,
  };
  const topic = `andon/v1/device/${deviceId}/result`;
  broker.publish(
    {
      cmd: "publish",
      topic,
      payload: Buffer.from(JSON.stringify(result)),
      qos: 1,
      retain: false,
      dup: false,
    },
    (err?: Error | null) => {
      if (err) app.log.error({ err, topic }, "Failed to publish COMMAND_RESULT");
      else app.log.info({ topic, incidentId }, "COMMAND_RESULT published");
    },
  );
}

// dashboard/PLAN.md §2/§8: published on every dashboard-driven state
// change so the firmware has something real to subscribe to whenever it
// grows that capability - nothing consumes this yet (as of 2026-08-13),
// so the exact shape here is provisional and may need reconciling once a
// real firmware subscriber exists (topic naming in particular - see
// PLAN.md §8's note that this may already be built elsewhere).
function publishDeviceState(deviceId: string, incidentId: string, status: string) {
  const event = {
    schemaVersion: 1,
    eventId: randomUUID(),
    eventType: "INCIDENT_STATE_CHANGED",
    incidentId,
    status,
    serverTimestamp: new Date().toISOString(),
  };
  const topic = `andon/v1/device/${deviceId}/state`;
  broker.publish(
    {
      cmd: "publish",
      topic,
      payload: Buffer.from(JSON.stringify(event)),
      qos: 1,
      retain: false,
      dup: false,
    },
    (err?: Error | null) => {
      if (err) app.log.error({ err, topic }, "Failed to publish device state");
    },
  );
}

broker.on("publish", (packet: AedesPublishPacket, client: Client | null) => {
  if (!client) return; // ignore the broker's own internal/retained publishes
  if (!packet.topic.startsWith("andon/v1/") || !packet.topic.endsWith("/event")) return;

  let evt: AndonEvent;
  try {
    evt = JSON.parse(packet.payload.toString());
  } catch {
    app.log.warn({ topic: packet.topic }, "AndonEvent: invalid JSON, ignoring");
    return;
  }

  app.log.info({ evt }, "AndonEvent received");

  if (evt.eventType === "PRODUCTION_COUNT_UPDATED") {
    // Not in architectur.md's MQTT design (§8) or PRD.md - this event type
    // doesn't exist in any governing doc. It reuses the same device event
    // topic/envelope/idempotency handling as ANDON_REQUESTED (consistent
    // with the existing convention) rather than inventing a new topic, but
    // is otherwise an ad-hoc extension - flag this if/when a real backend
    // replaces this test one. No incident semantics apply (no
    // acknowledge/resolve/etc lifecycle), so it's acked directly rather
    // than routed through the incident store.
    const count = evt.payload.productionCount as number;
    const workOrderId = (evt.payload.workOrderId as string) ?? "";
    setProductionCount(evt.stationId, count, workOrderId);
    broadcast({ type: "production_updated", stationId: evt.stationId, productionCount: count, workOrderId });
    app.log.info({ stationId: evt.stationId, productionCount: count, workOrderId }, "Production count updated");
    publishResult(evt.deviceId, evt.correlationId, "n/a");
    return;
  }

  if (evt.eventType === "INCIDENT_STATUS_UPDATE") {
    // Device-initiated Start Handling / Resolve - see andon_mqtt.hpp's
    // submitStatusUpdate() and dashboard/PLAN.md's note on why these two
    // transitions are deliberately NOT reachable from the dashboard's REST
    // endpoints (technician must be physically at the terminal - product
    // decision, 2026-08-13). Acknowledge is the one transition the
    // dashboard can still drive remotely.
    const incidentId = evt.payload.incidentId as string;
    const status = evt.payload.status as string;
    const incident =
      status === "HANDLING" ? startHandlingIncident(incidentId)
      : status === "RESOLVED" ? resolveIncident(incidentId)
      : undefined;

    if (!incident) {
      app.log.warn(
        { incidentId, status },
        "INCIDENT_STATUS_UPDATE: unknown incidentId or invalid status - not replying (device will time out and can retry)",
      );
      return;
    }
    broadcast({ type: "incident_updated", incident });
    app.log.info({ incidentId, status }, "Incident status updated from device");
    publishResult(evt.deviceId, evt.correlationId, incident.incidentId);
    return;
  }

  if (evt.eventType !== "ANDON_REQUESTED") {
    app.log.info(
      { eventType: evt.eventType },
      "AndonEvent: unhandled eventType (only ANDON_REQUESTED/PRODUCTION_COUNT_UPDATED/INCIDENT_STATUS_UPDATE are wired up in this test backend)",
    );
    return;
  }

  const { incident, isNew } = createOrGetIncident({
    idempotencyKey: evt.idempotencyKey,
    stationId: evt.stationId,
    deviceId: evt.deviceId,
    categoryCode: evt.payload.categoryCode as string,
    reasonCode: evt.payload.reasonCode as string,
    workOrderId: evt.payload.workOrderId as string,
  });

  if (isNew) {
    app.log.info(
      { incidentId: incident.incidentId, category: incident.categoryCode, reason: incident.reasonCode },
      "New incident accepted",
    );
    broadcast({ type: "incident_created", incident });
  } else {
    app.log.info(
      { idempotencyKey: evt.idempotencyKey, incidentId: incident.incidentId },
      "Duplicate idempotencyKey - replaying ACCEPTED without creating a new incident",
    );
  }

  publishResult(evt.deviceId, evt.correlationId, incident.incidentId);
});

broker.on("client", (client: Client) => {
  app.log.info({ clientId: client.id }, "MQTT client connected");
});
broker.on("clientDisconnect", (client: Client) => {
  app.log.info({ clientId: client.id }, "MQTT client disconnected");
});

// --- HTTP (Fastify) ----------------------------------------------------------

const app = Fastify({ logger: true });

app.register(websocketPlugin);

app.get<{ Params: { stationId: string } }>(
  "/api/v1/configuration/stations/:stationId",
  async (request) => {
    // Real backend would 404 for an unknown stationId (architectur.md's
    // stations table) - this test backend serves the same seed for any id.
    return { ...STATION_CONFIG, stationId: request.params.stationId };
  },
);

// New (2026-08-13), not in architectur.md/contracts/ yet - same ad-hoc-
// extension status as PRODUCTION_COUNT_UPDATED above (see work-order-
// store.ts's header). Lets the device's WORK ORDER picker (SCR_WORK_ORDER_LIST
// in firmware/src/main.cpp) show something real instead of a hardcoded id.
//
// Each work order is enriched with its current productionCount (from
// production-store.ts) - added same day as a follow-up fix: the device
// used to keep its own device-local last-known count per work order,
// which could silently drift from what's actually on the server/
// dashboard. The picker now re-fetches this endpoint every time it
// opens (see firmware/src/andon_workorders.cpp's sync()), so switching
// work orders always starts from the server's own number.
app.get<{ Params: { stationId: string } }>(
  "/api/v1/work-orders/stations/:stationId",
  async (request) => {
    const stationId = request.params.stationId;
    const workOrders = listWorkOrders(stationId).map((wo) => ({
      ...wo,
      productionCount: getProductionCount(stationId, wo.workOrderId),
    }));
    return { workOrders };
  },
);

// All stations' work orders, same productionCount enrichment as the
// per-station route above - added same day (2026-08-13) so the
// dashboard's PRODUCTION panel can show the full catalog every station's
// device picker already shows (including work orders that have never
// reported a count yet), not just the subset that happened to have
// reported at least one PRODUCTION_COUNT_UPDATED. See dashboard/PLAN.md's
// matching note.
app.get("/api/v1/work-orders", async () => {
  const workOrders = listAllWorkOrders().map((wo) => ({
    ...wo,
    productionCount: getProductionCount(wo.stationId, wo.workOrderId),
  }));
  return { workOrders };
});

app.get("/healthz", async () => ({ status: "ok" }));

// --- Dashboard-facing endpoints (dashboard/PLAN.md §2) ----------------------

app.get("/api/v1/incidents", async () => {
  return { incidents: listIncidents() };
});

app.get("/api/v1/production", async () => {
  return { production: listProductionCounts() };
});

// Acknowledge is the only incident transition the dashboard can still
// drive remotely - Start Handling and Resolve are deliberately
// device-initiated only (see the INCIDENT_STATUS_UPDATE MQTT handler
// above): a technician has to physically be at the terminal to advance
// past Acknowledged, by product decision (2026-08-13). Per agents.md §11
// ("hiding a button is not authorization"), that's enforced here by not
// exposing the endpoint at all, not just by the dashboard not showing a
// button for it.
app.post<{ Params: { id: string } }>("/api/v1/incidents/:id/acknowledge", async (request, reply) => {
  const incident = acknowledgeIncident(request.params.id);
  if (!incident) {
    reply.code(404).send({ error: "incident not found" });
    return;
  }
  broadcast({ type: "incident_updated", incident });
  publishDeviceState(incident.deviceId, incident.incidentId, incident.status);
  return { incident };
});

app.register(async (instance) => {
  instance.get("/api/v1/realtime", { websocket: true }, (socket) => {
    registerClient(socket);
  });
});

// --- Startup ------------------------------------------------------------------

async function main() {
  await app.listen({ port: HTTP_PORT, host: "0.0.0.0" });
  app.log.info(`HTTP (config API) listening on :${HTTP_PORT}`);

  await new Promise<void>((resolve) => mqttServer.listen(MQTT_PORT, resolve));
  app.log.info(`MQTT broker listening on :${MQTT_PORT}`);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
