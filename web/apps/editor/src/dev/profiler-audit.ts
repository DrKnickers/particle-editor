// DEV-ONLY React Profiler audit seam for the Playwright "web" lane
// (tests-web/profiler-audit.spec.ts).
//
// Loaded ONLY behind `import.meta.env.DEV` via a dynamic import in main.tsx, so
// `vite build` excludes this module from the production bundle entirely. A test
// (production-bundle guard, scripts/lib/no-test-seam-in-prod.test.mjs) greps the
// built dist to prove `__profilerAudit` / `profiler-audit` are absent.
//
// Design (see tasks/2026-07-07-react-profiler-audit-plan.md §3 + §7):
//  - The five audited mount sites (App.tsx: Toolbar, StatusBar; PanelLayout.tsx:
//    EmitterTree, CurveEditorPanel, AtlasPickerPanel) are each wrapped, behind an
//    `import.meta.env.DEV` gate, in React's built-in <Profiler>. Their onRender
//    calls `window.__profilerAudit?.record`. Production files never import THIS
//    module — they reference only the `window.__profilerAudit` global (typed by
//    the `declare global` below) — so the dev module has exactly one referrer:
//    the DEV-gated dynamic import in main.tsx, which folds away in prod.
//  - `record` is a Profiler onRender callback. It stores one row per commit; the
//    metric is a SUBTREE commit count (onRender.actualDuration accumulates the
//    wrapped subtree, not the component's own body — rank by count, not duration).
//  - StatusBar's real re-render driver is `cursor/position-3d` (~30 Hz) and
//    `stats/tick` (4 Hz), which the MockBridge does NOT emit organically. The
//    `emitCursor`/`emitStats` helpers inject them through the live bridge exposed
//    at `window.bridge` (bridge/expose.ts). MockBridge.emit is `private` (TS
//    compile-time only); the cast is contained here in a DEV-only module that
//    never ships. The spec spaces these emits one-per-animation-frame so React
//    batching cannot collapse N events into one commit (assert distinct
//    commitTimes on the StatusBar rows).

import type { ProfilerOnRenderCallback } from "react";

type CommitPhase = "mount" | "update" | "nested-update";

type CommitRow = {
  id: string;
  phase: CommitPhase;
  actualDuration: number;
  commitTime: number;
};

/** Per-Profiler-id aggregate. `commits` is the primary ranking signal;
 *  `distinctCommitTimes` exposes React batching (N synthetic emits collapsing
 *  into fewer commits). `totalActualMs` is subtree-inclusive — secondary only. */
export type ProfilerAggregate = {
  commits: number;
  mounts: number;
  updates: number;
  totalActualMs: number;
  distinctCommitTimes: number;
};

type StatsPayload = {
  fps: number;
  emitters: number;
  particles: number;
  instances: number;
  overload: boolean;
};

export type ProfilerAuditApi = {
  /** Passed as <Profiler onRender>; one call == one commit of the wrapped subtree. */
  record: ProfilerOnRenderCallback;
  /** Clear all collected rows (call before each measured interaction). */
  reset: () => void;
  /** Raw per-commit rows, in commit order. */
  rows: () => CommitRow[];
  /** Rows aggregated per Profiler id. */
  dump: () => Record<string, ProfilerAggregate>;
  /** Inject one synthetic `cursor/position-3d` event through the live bridge. */
  emitCursor: (x: number, y: number, z: number) => void;
  /** Inject one synthetic `stats/tick` event through the live bridge. */
  emitStats: (s: StatsPayload) => void;
};

declare global {
  interface Window {
    __profilerAudit?: ProfilerAuditApi;
  }
}

// MockBridge.emit is a private method (TS compile-time visibility only); at
// runtime the instance has it and it dispatches to `.on(...)` listeners. Reach
// it through a structural cast contained to this DEV-only module.
type EmittableBridge = { emit?: (e: { kind: string; payload: unknown }) => void };

export function installProfilerAuditSeam(): void {
  const rows: CommitRow[] = [];

  const emitThroughBridge = (kind: string, payload: unknown): void => {
    const bridge = window.bridge as unknown as EmittableBridge | undefined;
    // Fail loudly rather than silently no-op: a missing/uncallable emit means the
    // audit would undercount StatusBar's synthetic storm to zero without failing.
    if (!bridge || typeof bridge.emit !== "function") {
      throw new Error(
        `[profiler-audit] window.bridge.emit is unavailable — cannot inject "${kind}". ` +
          `Ensure the app is running the MockBridge (browser/dev) and window.bridge is set.`,
      );
    }
    bridge.emit({ kind, payload });
  };

  window.__profilerAudit = {
    record(id, phase, actualDuration, _baseDuration, _startTime, commitTime) {
      rows.push({ id, phase: phase as CommitPhase, actualDuration, commitTime });
    },
    reset() {
      rows.length = 0;
    },
    rows() {
      return rows.slice();
    },
    dump() {
      const agg: Record<string, ProfilerAggregate> = {};
      const timesById: Record<string, Set<number>> = {};
      for (const r of rows) {
        const a = (agg[r.id] ??= {
          commits: 0,
          mounts: 0,
          updates: 0,
          totalActualMs: 0,
          distinctCommitTimes: 0,
        });
        a.commits += 1;
        if (r.phase === "mount") a.mounts += 1;
        else a.updates += 1;
        a.totalActualMs += r.actualDuration;
        (timesById[r.id] ??= new Set()).add(r.commitTime);
      }
      for (const id of Object.keys(agg)) {
        agg[id].distinctCommitTimes = timesById[id].size;
      }
      return agg;
    },
    emitCursor(x, y, z) {
      emitThroughBridge("cursor/position-3d", { x, y, z });
    },
    emitStats(s) {
      emitThroughBridge("stats/tick", s);
    },
  };
}
