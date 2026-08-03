// StatusBar — 5-column readout: FPS · Emitters · Particles · Instances · Cursor.
// FPS / Emitters / Particles / Instances subscribe to the stats/tick event
// emitted by the C++ host at 4 Hz. The Cursor cell subscribes to
// `cursor/position-3d`, emitted at ~30 Hz while the
// mouse is over the viewport popup. In browser mode (MockBridge) neither
// event fires; the component renders placeholder em-dashes.
//
// The four stats cells live in a React.memo'd StatsCells child so the ~30 Hz
// cursor updates (which re-render this parent) SKIP them — they only re-render
// when `stats` actually changes (stats/tick, 4 Hz). See the #549 Profiler audit:
// StatusBar was re-rendering all five cells on every cursor move.
import { memo, useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useEngineField } from "@/lib/use-engine-snapshot";
import { usePresence } from "@/lib/use-presence";
import { STATUS_FEEDBACK_CLEAR_MS, useStatusFeedback } from "@/lib/status-feedback";

// Mirrors --motion-fast-out (tokens.css) — the .fade-animate-fast exit duration.
const PAUSED_EXIT_MS = 110;

type Stats = { fps: number; emitters: number; particles: number; instances: number; overload: boolean };
type Cursor3D = { x: number; y: number; z: number };

// A single label/value readout cell. Pure + module-level so it closes over
// nothing and stays cheap to call from both StatsCells and the cursor cell.
function cell(label: string, value: string, dim: boolean, warn = false) {
  return (
    <span className="flex items-baseline gap-1.5">
      <span className="text-text-3">{label}</span>
      <span
        className={`font-mono tabular-nums ${
          warn ? "text-warning-fg" : dim ? "text-text-3" : "text-text-2"
        }`}
      >
        {value}
      </span>
    </span>
  );
}

// Test-only render counter for StatsCells — lets a jsdom test prove that a
// cursor/position-3d event does NOT re-render the stats cells (memo working),
// while a stats/tick event does. Inert in production (a plain integer).
let statsCellsRenders = 0;
export function __statsCellsRenderCount(): number {
  return statsCellsRenders;
}

// The FPS/Emitters/Particles/Instances cells (+ their separators), memoized on
// the `stats` prop. Returns a fragment so the flex layout is identical to the
// inline version — StatsCells adds no DOM node of its own.
const StatsCells = memo(function StatsCells({ stats }: { stats: Stats | null }) {
  statsCellsRenders++;
  const placeholder = stats === null;
  return (
    <>
      {cell("FPS", placeholder ? "—" : stats!.fps.toFixed(0), placeholder)}
      <span className="text-text-3">·</span>
      {cell("Emitters", placeholder ? "—" : stats!.emitters.toString(), placeholder)}
      <span className="text-text-3">·</span>
      {cell("Particles", placeholder ? "—" : stats!.particles.toString(), placeholder, !placeholder && stats!.overload)}
      <span className="text-text-3">·</span>
      {cell("Instances", placeholder ? "—" : stats!.instances.toString(), placeholder)}
    </>
  );
});

export function StatusBar({ bridge }: { bridge: Bridge }) {
  const [stats, setStats] = useState<Stats | null>(null);
  const [cursor, setCursor] = useState<Cursor3D | null>(null);
  // PAUSED indicator. Mirrors the Toolbar's pause signal
  // (engine/state snapshot + changed → EngineStateDto.paused) so the
  // status bar shows the paused state without a new bridge command.
  const paused = useEngineField(bridge, (s) => s.paused) ?? false;
  // Presence fade for the PAUSED tag (design pass) — fast tier, so the
  // indicator eases in/out instead of popping with the 4 Hz cadence around it.
  const pausedPresence = usePresence(paused, PAUSED_EXIT_MS);

  // Transient action feedback (F4): latest-wins message from the
  // status-feedback store, auto-cleared after STATUS_FEEDBACK_CLEAR_MS —
  // epoch-guarded so a rapid follow-up action restarts the timer instead of
  // being clipped by the previous one's clear.
  const feedbackMessage = useStatusFeedback((s) => s.message);
  const feedbackEpoch = useStatusFeedback((s) => s.epoch);
  const feedbackPresence = usePresence(feedbackMessage !== null, PAUSED_EXIT_MS);
  // Keep the last text through the exit fade (message is already null then).
  const [lastFeedback, setLastFeedback] = useState("");
  useEffect(() => {
    if (feedbackMessage !== null) setLastFeedback(feedbackMessage);
  }, [feedbackMessage]);
  useEffect(() => {
    if (feedbackMessage === null) return;
    const t = window.setTimeout(
      () => useStatusFeedback.getState().clear(feedbackEpoch),
      STATUS_FEEDBACK_CLEAR_MS,
    );
    return () => window.clearTimeout(t);
  }, [feedbackMessage, feedbackEpoch]);

  // `true` until the host says otherwise: an editor that has not yet run an
  // autosave has nothing to warn about, and a default of `false` would cry wolf
  // on every launch.
  const [autosaveHealthy, setAutosaveHealthy] = useState(true);

  useEffect(() => {
    const offStats = bridge.on("stats/tick", (e) => {
      setStats(e.payload);
    });
    const offCursor = bridge.on("cursor/position-3d", (e) => {
      setCursor(e.payload);
    });
    // When the host signals stats are frozen (test-only
    // knob set via stats/set-frozen), drop the local state so all
    // cells fall back to `—` placeholders. The host stops emitting
    // stats/tick while frozen, so the cleared state stays cleared.
    // Cursor is cleared too since it's part of the StatusBar's
    // volatile per-frame surface. (Clearing stats flows into StatsCells
    // via its `stats` prop → it re-renders to placeholders, as before.)
    const offFreeze = bridge.on("stats/frozen-changed", (e) => {
      if (e.payload.frozen) {
        setStats(null);
        setCursor(null);
      }
    });
    // Autosave health (2026-07 audit, P2-06). Durable state, deliberately NOT a
    // toast: a failed autosave stays failed until a write succeeds, so a
    // warning that expired on a timer would tell the user the recovery net
    // recovered when nothing of the sort happened. The host emits only on a
    // change and replays a known-bad state on app/ready.
    const offAutosave = bridge.on("autosave/health", (e) => {
      setAutosaveHealthy(e.payload.healthy);
    });
    return () => {
      offStats();
      offCursor();
      offFreeze();
      offAutosave();
    };
  }, [bridge]);

  // 2dp cursor readout, matching legacy ("Mouse: x, y, z" at 2dp).
  const cursorText = cursor === null
    ? "—"
    : `${cursor.x.toFixed(2)}, ${cursor.y.toFixed(2)}, ${cursor.z.toFixed(2)}`;

  return (
    <footer className="flex h-7 shrink-0 items-center gap-3 border-t border-border bg-bg px-4 text-xs">
      <StatsCells stats={stats} />
      <span className="text-text-3">·</span>
      {cell("Cursor", cursorText, cursor === null)}
      {/* Right-aligned group: transient action feedback + PAUSED + the
          always-on spawn hint (the legacy main.cpp's permanent rightmost
          pane). Feedback and PAUSED each own a PERSISTENT polite live region
          (a live region must pre-exist its content change to fire, and
          sharing one region would interleave/re-announce unrelated
          messages — plan-review finding). Feedback sits LEFT of the
          right-anchored pair and truncates, so the hint never moves. */}
      <div className="ml-auto flex min-w-0 items-center gap-3">
        <span role="status" aria-live="polite" className="min-w-0" data-testid="status-feedback">
          {feedbackPresence.mounted && (
            <span
              className="fade-animate-fast block max-w-[300px] truncate whitespace-nowrap text-text-3"
              data-state={feedbackPresence.state}
              onAnimationEnd={feedbackPresence.onAnimationEnd}
            >
              {feedbackMessage ?? lastFeedback}
            </span>
          )}
        </span>
        <span aria-live="polite">
          {pausedPresence.mounted && (
            <span
              className="fade-animate-fast font-mono font-semibold tracking-wide text-warning-fg"
              data-state={pausedPresence.state}
              onAnimationEnd={pausedPresence.onAnimationEnd}
            >
              PAUSED
            </span>
          )}
        </span>
        {/* Autosave failure. Persistent by design — it stays until a write
            succeeds — so no presence/fade wrapper: nothing here animates in or
            out, which also keeps it out of the a11y-snapshot settle list.
            role="alert" (assertive) rather than the polite regions above:
            losing the crash-recovery net is not a status update, and the user
            may be minutes from needing it. */}
        {!autosaveHealthy && (
          <span
            role="alert"
            data-testid="status-autosave-failed"
            title="The last autosave write failed. Your most recent changes are not recoverable after a crash — save the file manually."
            className="shrink-0 font-semibold text-warning-fg"
          >
            ⚠ Autosave failing
          </span>
        )}
        <span className="shrink-0 text-text-3">⇧ Shift: spawn instance</span>
      </div>
    </footer>
  );
}
