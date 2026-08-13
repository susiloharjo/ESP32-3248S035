// WebSocket client for backend/src/realtime.ts's broadcast feed. Fixed
// short-delay reconnect (no backoff curve - this is a demo dashboard on a
// LAN, not a battery-powered device that needs to avoid hammering a
// broker; agents.md's "bounded exponential backoff with jitter" rule is a
// firmware/device requirement, not asked of this client).

import type { DashboardEvent } from "./types";

const RECONNECT_DELAY_MS = 2000;

export function connectRealtime(
  onEvent: (event: DashboardEvent) => void,
  onConnectionChange: (connected: boolean) => void,
): void {
  function connect() {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    const ws = new WebSocket(`${proto}//${location.host}/api/v1/realtime`);

    ws.onopen = () => onConnectionChange(true);
    ws.onclose = () => {
      onConnectionChange(false);
      setTimeout(connect, RECONNECT_DELAY_MS);
    };
    ws.onerror = () => ws.close(); // triggers onclose's reconnect path
    ws.onmessage = (msg) => {
      try {
        onEvent(JSON.parse(msg.data as string) as DashboardEvent);
      } catch {
        // Malformed frame - ignore rather than crash the whole dashboard
        // over one bad message.
      }
    };
  }

  connect();
}
