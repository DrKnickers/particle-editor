// SpawnerPanel — modeless tool window for the programmable particle
// spawner (Phase 3 Screen 8 Batch 4). Replaces the legacy
// `SpawnerDlgProc` at [src/main.cpp:5824] for the React UI; the Win32
// dialog stays for `--legacy-ui` until Phase 4.2.
//
// Sections (top-to-bottom):
//   1. Mode      — Manual / Auto radio.
//   2. Enabled   — checkbox (Auto-only; hidden in Manual).
//   3. Burst size, Spacing, Interval (Auto-only).
//   4. Position  — Vec3 spinner row.
//   5. Velocity  — Vec3 spinner row.
//   6. Lifetime  — single Spinner.
//   7. Jitter position / Jitter velocity — Vec3 rows.
//   8. Spawn now — manual-only button (fires spawner/trigger).
//
// State sync. The panel reads `snapshot.spawner` on mount and listens
// to `engine/state/changed` for external mutations (legacy `--legacy-ui`
// edits, devtools). Local edits commit immediately via
// `spawner/start { params: <full config> }` — matches the host's
// `SpawnerDriver::SetConfig` semantics (full-config replace, not patch).
//
// Header badge. Subscribes to `spawner/active-count` and renders the
// count as a small pill in the header. A small Stop glyph fires
// `spawner/stop` and is disabled when count == 0.

import { useEffect, useRef, useState } from "react";
import { Square, X } from "lucide-react";
import type {
  Bridge,
  SpawnerParamsDto,
  SpawnerMode,
  Vec3,
} from "@particle-editor/bridge-schema";
import { Spinner } from "@/primitives/Spinner";
import { ToolPanel } from "@/components/ToolPanel";
import { makeDefaultSpawnerParams } from "@/bridge/mock-state";
import { setDock } from "@/lib/right-dock";

type Props = {
  bridge: Bridge;
};

/** Hard caps from `SpawnerDriver` — mirror the C++ constants at
 *  [src/SpawnerDriver.h:50-57] so the panel clamps where the host would
 *  clamp anyway. */
const MAX_BURST_SIZE = 10;
const MAX_SPACING_SEC = 10;
const MAX_INTERVAL_SEC = 60;
const MAX_LIFETIME_SEC = 600;

/** Build a new config from the current one with a single field replaced.
 *  Returns a fresh object so the host/mock sees a full SpawnerParamsDto
 *  on every `spawner/start`. */
function patchSpawner<K extends keyof SpawnerParamsDto>(
  base: SpawnerParamsDto,
  key: K,
  value: SpawnerParamsDto[K],
): SpawnerParamsDto {
  return { ...base, [key]: value };
}

function patchVec(base: Vec3, idx: 0 | 1 | 2, v: number): Vec3 {
  const out: [number, number, number] = [base[0], base[1], base[2]];
  out[idx] = v;
  return out as Vec3;
}

export function SpawnerPanel({ bridge }: Props) {
  // Cache the spawner config locally — fed by snapshot + state/changed.
  // Defaults to the same struct the engine ships with so the panel never
  // renders empty Spinners (which would clamp to NaN on first commit).
  const [config, setConfig] = useState<SpawnerParamsDto>(() =>
    makeDefaultSpawnerParams(),
  );
  const [activeCount, setActiveCount] = useState(0);

  // Track the "last config we committed" so we don't echo our own
  // changes back through the state/changed handler (avoids feedback
  // loops). A ref keeps the comparison cheap.
  const lastCommitted = useRef<SpawnerParamsDto | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (!cancelled) setConfig(s.spawner);
      })
      .catch((err) => console.warn("[SpawnerPanel] snapshot failed:", err));

    const offState = bridge.on("engine/state/changed", (e) => {
      // Skip our own echoes — every commit triggers a state/changed,
      // and applying it back into the same store is wasteful.
      const inbound = e.payload.spawner;
      if (
        lastCommitted.current &&
        JSON.stringify(lastCommitted.current) === JSON.stringify(inbound)
      ) {
        return;
      }
      setConfig(inbound);
    });

    const offActive = bridge.on("spawner/active-count", (e) => {
      setActiveCount(e.payload.count);
    });

    return () => {
      cancelled = true;
      offState();
      offActive();
    };
  }, [bridge]);

  /** Commit a new config to the host. Stores it in `lastCommitted` so
   *  the round-trip state/changed handler skips the echo. */
  const commit = (next: SpawnerParamsDto) => {
    lastCommitted.current = next;
    setConfig(next);
    void bridge.request({ kind: "spawner/start", params: next });
  };

  const setMode = (mode: SpawnerMode) =>
    commit(patchSpawner(config, "mode", mode));
  const setEnabled = (enabled: boolean) =>
    commit(patchSpawner(config, "enabled", enabled));
  const setBurstSize = (v: number) =>
    commit(patchSpawner(config, "burstSize", Math.round(v)));
  const setSpacingSec = (v: number) =>
    commit(patchSpawner(config, "spacingSec", v));
  const setIntervalSec = (v: number) =>
    commit(patchSpawner(config, "intervalSec", v));
  const setPositionAxis = (idx: 0 | 1 | 2, v: number) =>
    commit(patchSpawner(config, "position", patchVec(config.position, idx, v)));
  const setVelocityAxis = (idx: 0 | 1 | 2, v: number) =>
    commit(patchSpawner(config, "velocity", patchVec(config.velocity, idx, v)));
  const setMaxLifetimeSec = (v: number) =>
    commit(patchSpawner(config, "maxLifetimeSec", v));
  const setJitterPosAxis = (idx: 0 | 1 | 2, v: number) =>
    commit(
      patchSpawner(
        config,
        "jitterPosition",
        patchVec(config.jitterPosition, idx, v),
      ),
    );
  const setJitterVelAxis = (idx: 0 | 1 | 2, v: number) =>
    commit(
      patchSpawner(
        config,
        "jitterVelocity",
        patchVec(config.jitterVelocity, idx, v),
      ),
    );

  const handleTrigger = () => {
    void bridge.request({ kind: "spawner/trigger", params: {} });
  };

  const handleStop = () => {
    void bridge.request({ kind: "spawner/stop", params: {} });
  };

  const isAuto = config.mode === "auto";

  return (
    <div className="panel h-full" aria-label="Spawner">
      <div className="panel-header">
        <span>Spawner</span>
        <div className="panel-actions">
          <span
            aria-label="Active instance count"
            className="inline-flex h-5 min-w-[24px] items-center justify-center rounded bg-panel-2 px-1.5 text-[11px] font-medium text-text"
          >
            {activeCount}
          </span>
          <button
            type="button"
            onClick={handleStop}
            disabled={activeCount === 0}
            aria-label="Stop spawner"
            className="icon-btn disabled:cursor-not-allowed disabled:opacity-40"
          >
            <Square className="size-3" />
          </button>
          <button
            type="button"
            className="icon-btn"
            aria-label="Close Spawner"
            onClick={() => setDock(null)}
          >
            <X className="size-3.5" />
          </button>
        </div>
      </div>
      <div className="panel-body p-3">
      <ToolPanel.Section title="Mode" alwaysOpen>
        {/* Native radios under a role="radiogroup" wrapper — keyboard
            arrows + tab navigation come from the browser, and the
            Vitest jsdom harness can drive them with a plain click()
            without needing Radix's pointer-capture shim. */}
        <div role="radiogroup" aria-label="Spawner mode" className="flex gap-3">
          <label className="flex items-center gap-2 text-xs text-text">
            <input
              type="radio"
              name="spawner-mode"
              value="manual"
              checked={config.mode === "manual"}
              onChange={() => setMode("manual")}
              aria-label="Manual mode"
              className="size-3 accent-sky-500"
            />
            <span>Manual</span>
          </label>
          <label className="flex items-center gap-2 text-xs text-text">
            <input
              type="radio"
              name="spawner-mode"
              value="auto"
              checked={config.mode === "auto"}
              onChange={() => setMode("auto")}
              aria-label="Auto mode"
              className="size-3 accent-sky-500"
            />
            <span>Auto</span>
          </label>
        </div>

        {isAuto && (
          <label className="mt-2 flex items-center gap-2 text-xs text-text">
            <input
              type="checkbox"
              checked={config.enabled}
              onChange={(e) => setEnabled(e.target.checked)}
              aria-label="Enable spawner"
              className="size-3 accent-sky-500"
            />
            <span>Enabled</span>
          </label>
        )}

        {!isAuto && (
          <button
            type="button"
            onClick={handleTrigger}
            aria-label="Spawn now"
            className="mt-2 rounded bg-accent px-3 py-1 text-xs font-medium text-white outline-none transition hover:bg-accent focus:ring-2 focus:ring-accent"
          >
            Spawn now
          </button>
        )}
      </ToolPanel.Section>

      <ToolPanel.Section title="Burst" defaultOpen>
        <ToolPanel.Row label="Burst size">
          <Spinner
            value={config.burstSize}
            onChange={setBurstSize}
            min={1}
            max={MAX_BURST_SIZE}
            step={1}
            decimals={0}
            aria-label="Burst size"
          />
        </ToolPanel.Row>
        <ToolPanel.Row label="Spacing">
          <Spinner
            value={config.spacingSec}
            onChange={setSpacingSec}
            min={0}
            max={MAX_SPACING_SEC}
            step={0.05}
            decimals={2}
            unit="s"
            aria-label="Burst spacing"
          />
        </ToolPanel.Row>
        {isAuto && (
          <ToolPanel.Row label="Interval">
            <Spinner
              value={config.intervalSec}
              onChange={setIntervalSec}
              min={0}
              max={MAX_INTERVAL_SEC}
              step={0.5}
              decimals={2}
              unit="s"
              aria-label="Burst interval"
            />
          </ToolPanel.Row>
        )}
      </ToolPanel.Section>

      <ToolPanel.Section title="Position" defaultOpen>
        <div className="grid grid-cols-3 gap-1">
          <div className="axis-cell">
            <span className="axis-lbl">X</span>
            <Spinner
              value={config.position[0]}
              onChange={(v) => setPositionAxis(0, v)}
              step={0.1}
              aria-label="Position X"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Y</span>
            <Spinner
              value={config.position[1]}
              onChange={(v) => setPositionAxis(1, v)}
              step={0.1}
              aria-label="Position Y"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Z</span>
            <Spinner
              value={config.position[2]}
              onChange={(v) => setPositionAxis(2, v)}
              step={0.1}
              aria-label="Position Z"
            />
          </div>
        </div>
      </ToolPanel.Section>

      <ToolPanel.Section title="Velocity" defaultOpen>
        <div className="grid grid-cols-3 gap-1">
          <div className="axis-cell">
            <span className="axis-lbl">X</span>
            <Spinner
              value={config.velocity[0]}
              onChange={(v) => setVelocityAxis(0, v)}
              step={0.1}
              aria-label="Velocity X"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Y</span>
            <Spinner
              value={config.velocity[1]}
              onChange={(v) => setVelocityAxis(1, v)}
              step={0.1}
              aria-label="Velocity Y"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Z</span>
            <Spinner
              value={config.velocity[2]}
              onChange={(v) => setVelocityAxis(2, v)}
              step={0.1}
              aria-label="Velocity Z"
            />
          </div>
        </div>
      </ToolPanel.Section>

      <ToolPanel.Section title="Lifetime" alwaysOpen>
        <ToolPanel.Row label="Max lifetime">
          <Spinner
            value={config.maxLifetimeSec}
            onChange={setMaxLifetimeSec}
            min={0}
            max={MAX_LIFETIME_SEC}
            step={0.5}
            decimals={2}
            unit="s"
            aria-label="Max lifetime"
          />
        </ToolPanel.Row>
      </ToolPanel.Section>

      <ToolPanel.Section title="Jitter position">
        <div className="grid grid-cols-3 gap-1">
          <div className="axis-cell">
            <span className="axis-lbl">X</span>
            <Spinner
              value={config.jitterPosition[0]}
              onChange={(v) => setJitterPosAxis(0, v)}
              step={0.05}
              aria-label="Jitter position X"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Y</span>
            <Spinner
              value={config.jitterPosition[1]}
              onChange={(v) => setJitterPosAxis(1, v)}
              step={0.05}
              aria-label="Jitter position Y"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Z</span>
            <Spinner
              value={config.jitterPosition[2]}
              onChange={(v) => setJitterPosAxis(2, v)}
              step={0.05}
              aria-label="Jitter position Z"
            />
          </div>
        </div>
      </ToolPanel.Section>

      <ToolPanel.Section title="Jitter velocity">
        <div className="grid grid-cols-3 gap-1">
          <div className="axis-cell">
            <span className="axis-lbl">X</span>
            <Spinner
              value={config.jitterVelocity[0]}
              onChange={(v) => setJitterVelAxis(0, v)}
              step={0.05}
              aria-label="Jitter velocity X"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Y</span>
            <Spinner
              value={config.jitterVelocity[1]}
              onChange={(v) => setJitterVelAxis(1, v)}
              step={0.05}
              aria-label="Jitter velocity Y"
            />
          </div>
          <div className="axis-cell">
            <span className="axis-lbl">Z</span>
            <Spinner
              value={config.jitterVelocity[2]}
              onChange={(v) => setJitterVelAxis(2, v)}
              step={0.05}
              aria-label="Jitter velocity Z"
            />
          </div>
        </div>
      </ToolPanel.Section>

      </div>
    </div>
  );
}
