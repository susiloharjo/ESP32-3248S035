// OEE = Availability × Performance × Quality, computed client-side from
// data the dashboard already has in memory (main.ts's incidents/
// production Maps) - no new backend endpoint needed for the math itself
// (2026-08-13, "di dashboard bisa itungin oee ngga bro?").
//
// This is a SIMPLIFIED/approximate OEE, not an audit-grade figure - said
// explicitly here and in the "(approximate)" label next to the panel
// heading, because each factor cuts a corner real OEE tooling wouldn't:
//
//   Availability = (planned time − downtime) / planned time
//     Downtime = sum of every incident's duration for that station
//     (openedAt → resolvedAt, or → now if still open/unresolved),
//     regardless of category - a QUALITY or SUPERVISOR call may not
//     actually stop the line the way a MAINTENANCE one does, but this
//     system has no "did this actually halt production" flag to
//     distinguish them, so all incidents count as downtime.
//     Planned time is a fixed 8-hour shift (2026-08-13 decision) - not
//     configurable per station/shift-calendar.
//
//   Performance = actual good count / target count, both summed across
//     every work order reported for that station. Real OEE performance
//     is (ideal cycle time × count) / run time - this system has no
//     ideal-cycle-time figure anywhere (SCR-01's "18 pcs/h" is a
//     hardcoded display string, not a real measured rate), so target
//     quantity is used as the stand-in baseline instead.
//
//   Quality = good / (good + reject) - this part IS real, fed by the
//     device's REJECTS counter (see andon_workorders.hpp's
//     rejectCount()) rather than assumed 100%. 100% only when nothing's
//     been produced yet (0/0 - avoids a NaN/misleading 0%).

import type { Incident, ProductionEntry } from "./types";

const PLANNED_MINUTES = 8 * 60; // 8-hour shift - see this file's header comment

export interface OeeResult {
  stationId: string;
  availability: number; // 0..1
  performance: number; // 0..1
  quality: number; // 0..1
  oee: number; // 0..1
  downtimeMinutes: number;
  goodCount: number;
  rejectCount: number;
  targetCount: number;
}

function clamp01(n: number): number {
  if (!Number.isFinite(n)) return 0;
  return Math.max(0, Math.min(1, n));
}

export function computeOee(stationId: string, incidents: Incident[], production: ProductionEntry[]): OeeResult {
  const now = Date.now();
  const downtimeMs = incidents
    .filter((i) => i.stationId === stationId)
    .reduce((sum, i) => {
      const start = new Date(i.openedAt).getTime();
      const end = i.resolvedAt ? new Date(i.resolvedAt).getTime() : now;
      return sum + Math.max(0, end - start);
    }, 0);
  const downtimeMinutes = downtimeMs / 60000;
  const availability = clamp01((PLANNED_MINUTES - downtimeMinutes) / PLANNED_MINUTES);

  const stationEntries = production.filter((p) => p.stationId === stationId);
  const goodCount = stationEntries.reduce((sum, p) => sum + p.productionCount, 0);
  const rejectCount = stationEntries.reduce((sum, p) => sum + p.rejectCount, 0);
  const targetCount = stationEntries.reduce((sum, p) => sum + (p.target ?? 0), 0);

  const performance = targetCount > 0 ? clamp01(goodCount / targetCount) : 0;
  const totalCount = goodCount + rejectCount;
  const quality = totalCount > 0 ? clamp01(goodCount / totalCount) : 1;

  return {
    stationId,
    availability,
    performance,
    quality,
    oee: availability * performance * quality,
    downtimeMinutes,
    goodCount,
    rejectCount,
    targetCount,
  };
}

// One result per distinct stationId seen in production - derives its own
// station list rather than taking one as a parameter, so main.ts doesn't
// need a separate "known stations" source.
export function computeOeeForAllStations(incidents: Incident[], production: ProductionEntry[]): OeeResult[] {
  const stationIds = [...new Set(production.map((p) => p.stationId))].sort();
  return stationIds.map((stationId) => computeOee(stationId, incidents, production));
}
