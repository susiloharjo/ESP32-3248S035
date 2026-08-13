// WebSocket broadcast registry for dashboard clients - see
// dashboard/PLAN.md §2 for the three message shapes and §4.1 for why
// incident_created and incident_updated are distinct types (only the
// former should trigger the dashboard's alert banner). No auth/scoping
// (§6's deviations) - every connected client gets every event.

import type { WebSocket } from "ws";
import type { Incident } from "./incident-store";

export type DashboardEvent =
  | { type: "incident_created"; incident: Incident }
  | { type: "incident_updated"; incident: Incident }
  | { type: "production_updated"; stationId: string; productionCount: number; workOrderId: string };

const clients = new Set<WebSocket>();

export function registerClient(socket: WebSocket): void {
  clients.add(socket);
  socket.on("close", () => clients.delete(socket));
}

export function broadcast(event: DashboardEvent): void {
  const payload = JSON.stringify(event);
  for (const socket of clients) {
    // 1 === WebSocket.OPEN - avoid importing the ws value export just for
    // this constant when the type-only import above already covers typing.
    if (socket.readyState === 1) socket.send(payload);
  }
}
