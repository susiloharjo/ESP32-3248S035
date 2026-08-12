// ---------------------------------------------------------------------------
// Minimal test backend for the Digital Andon firmware.
//
// SCOPE (explicit, per agents.md's scope-discipline rule): this is NOT the
// production backend agents.md §10 specs - it's a minimal stand-in so the
// firmware's already-built pieces (GET config fetch - andon_config.cpp -
// and the MQTT Send Request path once wired up - see andon_mqtt.cpp) have
// something real to talk to end-to-end before the real backend exists.
//
// Deliberately missing, all required for production per agents.md:
//  - PostgreSQL. Config data is an in-memory seed (config-data.ts);
//    incidents are an in-memory Map, not persisted, no migrations, nothing
//    survives a restart.
//  - Auth. No device credentials, no MQTT ACLs, no Bearer auth on HTTP
//    (agents.md §12 requires all of these in production) - anyone who can
//    reach these ports can publish/subscribe/read anything. LAN-only test
//    tool; do not expose this to the internet or use it beyond local dev.
//  - Real idempotency. In-memory Set dedup only (see below) - a production
//    backend needs a DB unique constraint (agents.md §7/§10).
//  - Optimistic versioning, audit trail, outbox pattern, escalation rules,
//    notifications, the REST POST endpoints (acknowledge/start-handling/
//    resolve/confirm-close/reopen/cancel - architectur.md §9) - none of
//    this exists here. Only GET config and the ANDON_REQUESTED -> ACCEPTED
//    round trip are implemented.
//
// Do not deploy this anywhere real - it's a local dev/test harness only.
// ---------------------------------------------------------------------------

import { randomUUID } from "node:crypto";
import { createServer } from "node:net";
import Fastify from "fastify";
import Aedes from "aedes";
import type { AedesPublishPacket, Client } from "aedes";
import { STATION_CONFIG } from "./config-data";

const HTTP_PORT = Number(process.env.HTTP_PORT ?? 8080);
const MQTT_PORT = Number(process.env.MQTT_PORT ?? 1883);

// --- HTTP (Fastify) ----------------------------------------------------------

const app = Fastify({ logger: true });

app.get<{ Params: { stationId: string } }>(
  "/api/v1/configuration/stations/:stationId",
  async (request) => {
    // Real backend would 404 for an unknown stationId (architectur.md's
    // stations table) - this test backend serves the same seed for any id.
    return { ...STATION_CONFIG, stationId: request.params.stationId };
  },
);

app.get("/healthz", async () => ({ status: "ok" }));

// Debug-only, not a real endpoint from any contract - lets you check
// whether a PRODUCTION_COUNT_UPDATED event actually landed without
// tailing container logs.
app.get<{ Params: { stationId: string } }>(
  "/debug/production/:stationId",
  async (request) => {
    const count = latestProductionCountByStation.get(request.params.stationId);
    return { stationId: request.params.stationId, productionCount: count ?? null };
  },
);

// --- MQTT (aedes, embedded broker - no separate Mosquitto to install) -------

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

const broker = new Aedes();
const mqttServer = createServer(broker.handle);

// In-memory only - see the SCOPE note above. Keyed by idempotencyKey so a
// duplicate/retried publish (architectur.md §8.3: "at-least-once delivery;
// duplicates are expected") replays the same ACCEPTED result instead of
// minting a second incident.
const incidentsByIdempotencyKey = new Map<string, string>();
let nextIncidentSeq = 1;

// PRODUCTION_COUNT_UPDATED handling - see the ad-hoc-extension note where
// it's read below. In-memory only, keyed by stationId (matches PRD.md's
// "each terminal is assigned to one station" assumption).
const latestProductionCountByStation = new Map<string, number>();

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
    // Not in architectur.md's MQTT design (SS8) or PRD.md - this event type
    // doesn't exist in any governing doc. It reuses the same device event
    // topic/envelope/idempotency handling as ANDON_REQUESTED (consistent
    // with the existing convention) rather than inventing a new topic, but
    // is otherwise an ad-hoc extension - flag this if/when a real backend
    // replaces this test one. No incident semantics apply (no
    // acknowledge/resolve/etc lifecycle), so it's acked directly rather
    // than routed through incidentsByIdempotencyKey.
    const count = evt.payload.productionCount;
    latestProductionCountByStation.set(evt.stationId, count as number);
    app.log.info({ stationId: evt.stationId, productionCount: count }, "Production count updated");
    publishResult(evt.deviceId, evt.correlationId, "n/a");
    return;
  }

  if (evt.eventType !== "ANDON_REQUESTED") {
    app.log.info(
      { eventType: evt.eventType },
      "AndonEvent: unhandled eventType (only ANDON_REQUESTED/PRODUCTION_COUNT_UPDATED are wired up in this test backend)",
    );
    return;
  }

  let incidentId = incidentsByIdempotencyKey.get(evt.idempotencyKey);
  if (incidentId) {
    app.log.info(
      { idempotencyKey: evt.idempotencyKey, incidentId },
      "Duplicate idempotencyKey - replaying ACCEPTED without creating a new incident",
    );
  } else {
    incidentId = `test-incident-${nextIncidentSeq++}`;
    incidentsByIdempotencyKey.set(evt.idempotencyKey, incidentId);
    app.log.info(
      { incidentId, category: evt.payload.categoryCode, reason: evt.payload.reasonCode },
      "New incident accepted",
    );
  }

  publishResult(evt.deviceId, evt.correlationId, incidentId);
});

broker.on("client", (client: Client) => {
  app.log.info({ clientId: client.id }, "MQTT client connected");
});
broker.on("clientDisconnect", (client: Client) => {
  app.log.info({ clientId: client.id }, "MQTT client disconnected");
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
