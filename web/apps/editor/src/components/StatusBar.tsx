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
    return () => {
      offStats();
      offCursor();
      offFreeze();
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
      {/* Right-aligned group: PAUSED state + always-on spawn hint
          (the legacy main.cpp's permanent rightmost pane). */}
      <div className="ml-auto flex items-center gap-3">
        {paused && (
          <span className="font-mono font-semibold tracking-wide text-warning-fg">
            PAUSED
          </span>
        )}
        <span className="text-text-3">⇧ Shift: spawn instance</span>
      </div>
    </footer>
  );
}
