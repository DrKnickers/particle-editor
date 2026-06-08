// StatusBar — 5-column readout: FPS · Emitters · Particles · Instances · Cursor.
// FPS / Emitters / Particles / Instances subscribe to the stats/tick event
// emitted by the C++ host at 4 Hz. The Cursor cell subscribes to
// `cursor/position-3d` (, Group A polish), emitted at ~30 Hz while the
// mouse is over the viewport popup. In browser mode (MockBridge) neither
// event fires; the component renders placeholder em-dashes.
import { useEffect, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";

type Stats = { fps: number; emitters: number; particles: number; instances: number };
type Cursor3D = { x: number; y: number; z: number };

export function StatusBar({ bridge }: { bridge: Bridge }) {
  const [stats, setStats] = useState<Stats | null>(null);
  const [cursor, setCursor] = useState<Cursor3D | null>(null);
  //: PAUSED indicator. Mirrors the Toolbar's pause signal
  // (engine/state snapshot + changed → EngineStateDto.paused) so the
  // status bar shows the paused state without a new bridge command.
  const [paused, setPaused] = useState(false);

  useEffect(() => {
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => setPaused(s.paused))
      .catch(() => {});
    const offState = bridge.on("engine/state/changed", (e) => {
      setPaused(e.payload.paused);
    });
    const offStats = bridge.on("stats/tick", (e) => {
      setStats(e.payload);
    });
    const offCursor = bridge.on("cursor/position-3d", (e) => {
      setCursor(e.payload);
    });
    // T9] When the host signals stats are frozen (test-only
    // knob set via stats/set-frozen), drop the local state so all
    // cells fall back to `—` placeholders. The host stops emitting
    // stats/tick while frozen, so the cleared state stays cleared.
    // Cursor is cleared too since it's part of the StatusBar's
    // volatile per-frame surface.
    const offFreeze = bridge.on("stats/frozen-changed", (e) => {
      if (e.payload.frozen) {
        setStats(null);
        setCursor(null);
      }
    });
    return () => {
      offState();
      offStats();
      offCursor();
      offFreeze();
    };
  }, [bridge]);

  const s = stats;
  const placeholder = s === null;

  const cell = (label: string, value: string, dim = placeholder) => (
    <span className="flex items-baseline gap-1.5">
      <span className="text-text-3">{label}</span>
      <span
        className={`font-mono tabular-nums ${
          dim ? "text-text-3" : "text-text-2"
        }`}
      >
        {value}
      </span>
    </span>
  );

  //: 2dp cursor readout, matching legacy ("Mouse: x, y, z" at 2dp).
  const cursorText = cursor === null
    ? "—"
    : `${cursor.x.toFixed(2)}, ${cursor.y.toFixed(2)}, ${cursor.z.toFixed(2)}`;

  return (
    <footer className="flex h-7 shrink-0 items-center gap-3 border-t border-border bg-bg px-4 text-xs">
      {cell("FPS", placeholder ? "—" : s!.fps.toFixed(0))}
      <span className="text-text-3">·</span>
      {cell("Emitters", placeholder ? "—" : s!.emitters.toString())}
      <span className="text-text-3">·</span>
      {cell("Particles", placeholder ? "—" : s!.particles.toString())}
      <span className="text-text-3">·</span>
      {cell("Instances", placeholder ? "—" : s!.instances.toString())}
      <span className="text-text-3">·</span>
      {cell("Cursor", cursorText, cursor === null)}
      {/* Right-aligned group: PAUSED state () + always-on spawn hint
          (, legacy main.cpp:2036's permanent rightmost pane). */}
      <div className="ml-auto flex items-center gap-3">
        {paused && (
          <span className="font-mono font-semibold tracking-wide text-amber-400">
            PAUSED
          </span>
        )}
        <span className="text-text-3">⇧ Shift: spawn instance</span>
      </div>
    </footer>
  );
}
