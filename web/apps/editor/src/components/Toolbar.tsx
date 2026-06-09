// Toolbar — Particle Editor 2026 layout. Grouped sections with
// dividers, spacer to the right, theme toggle at the rightmost edge.
//
// Group 1 (file actions):       New · Open · Save · Save As
// Group 2 (playback):           Play|Pause · Step · Step 10
// Group 3 (viewport toggles):   Show ground · Toggle bloom · Leave particles
// Group 4 (panels):             Spawner toggle
//   spacer
// Group 5 (environment):        Ground dropdown · Background dropdown
//
// Stop and Restart removed per design chat. The three viewport toggles
// (ground / bloom / leave-particles) live here as lucide icon buttons —
// they replaced the floating ViewportPill. Undo/Redo and Reload
// Shaders/Textures live in the menubar only.
//
// Uses the design's semantic CSS classes from components.css:
//   .toolbar, .tb-group, .tb-btn, .tb-divider, .tb-spacer

import { useEffect, useState } from "react";
import {
  FilePlus, FolderOpen, Save, SaveAll,
  Play, Pause, ChevronRight, ChevronsRight,
  Grid2x2, Sun, Sparkles, CirclePlus, Lightbulb,
} from "lucide-react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { BackgroundDropdown } from "@/components/BackgroundDropdown";
import { GroundDropdown } from "@/components/GroundDropdown";
import { useRightDock, toggleDock } from "@/lib/right-dock";
import { promptSaveChanges } from "@/lib/file-state";
import { runFileOp } from "@/lib/file-op";

type Props = { bridge: Bridge };

const ICON = { className: "size-3.5" } as const;

export function Toolbar({ bridge }: Props) {
  const [state, setState] = useState<EngineStateDto | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge.request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => { if (!cancelled) setState(s); })
      .catch((err) => console.warn("[Toolbar] snapshot failed:", err));
    const off = bridge.on("engine/state/changed", (e) => setState(e.payload));
    return () => { cancelled = true; off(); };
  }, [bridge]);

  const paused = state?.paused ?? false;
  // Viewport engine toggles (formerly the floating ViewportPill). Defaults
  // match the pill: ground/bloom off, leave-particles on.
  const ground = state?.ground ?? false;
  const bloom = state?.bloom ?? false;
  const leaveParticles = state?.leaveParticles ?? true;
  const dock = useRightDock();
  const spawnerVisible = dock === "spawner";
  const lightingVisible = dock === "lighting";

  return (
    <div data-testid="toolbar" className="toolbar">
      {/* Group 1: file actions. New / Open route through promptSaveChanges
          so a dirty document gets the Save/Discard/Cancel prompt before
          being replaced (same gate the MenuBar uses). Save + Save As are
          themselves the save path so they don't need the gate. */}
      <div className="tb-group">
        <button
          type="button"
          className="tb-btn"
          aria-label="New"
          title="New"
          onClick={() => {
            promptSaveChanges(async () => {
              await bridge.request({ kind: "file/new", params: {} });
            });
          }}
        >
          <FilePlus {...ICON} />
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Open"
          title="Open"
          onClick={() => {
            promptSaveChanges(async () => {
              await runFileOp(bridge, { kind: "file/open", params: {} });
            });
          }}
        >
          <FolderOpen {...ICON} />
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Save"
          title="Save"
          onClick={() => { void runFileOp(bridge, { kind: "file/save", params: {} }); }}
        >
          <Save {...ICON} />
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Save As"
          title="Save As"
          onClick={() => { void runFileOp(bridge, { kind: "file/save-as", params: {} }); }}
        >
          <SaveAll {...ICON} />
        </button>
      </div>

      <span className="tb-divider" />

      {/* Group 2: playback */}
      <div className="tb-group">
        <button
          type="button"
          className="tb-btn"
          aria-label={paused ? "Play" : "Pause"}
          title={paused ? "Play" : "Pause"}
          aria-pressed={!paused}
          onClick={() => { void bridge.request({ kind: "engine/set/paused", params: { paused: !paused } }); }}
        >
          {paused ? <Play {...ICON} /> : <Pause {...ICON} />}
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Step"
          title="Step one frame"
          onClick={() => { void bridge.request({ kind: "engine/action/step-frames", params: { frames: 1 } }); }}
        >
          <ChevronRight {...ICON} />
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Step 10"
          title="Step 10 frames"
          onClick={() => { void bridge.request({ kind: "engine/action/step-frames", params: { frames: 10 } }); }}
        >
          <ChevronsRight {...ICON} />
        </button>
      </div>

      <span className="tb-divider" />

      {/* Group 3: viewport engine toggles. aria-pressed + aria-labels are
          ported verbatim from the old ViewportPill so a11y semantics are
          preserved. Each reads the live engine snapshot and dispatches the
          matching engine/set/* with the inverted value. */}
      <div className="tb-group">
        <button
          type="button"
          className="tb-btn"
          aria-label="Show ground"
          title="Show ground"
          aria-pressed={ground}
          onClick={() => { void bridge.request({ kind: "engine/set/ground", params: { enabled: !ground } }); }}
        >
          <Grid2x2 {...ICON} />
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Toggle bloom"
          title="Toggle bloom"
          aria-pressed={bloom}
          onClick={() => { void bridge.request({ kind: "engine/set/bloom", params: { enabled: !bloom } }); }}
        >
          <Sun {...ICON} />
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Leave particles after instance death"
          title="Leave particles after instance death"
          aria-pressed={leaveParticles}
          onClick={() => { void bridge.request({ kind: "engine/set/leave-particles", params: { enabled: !leaveParticles } }); }}
        >
          <Sparkles {...ICON} />
        </button>
      </div>

      <span className="tb-divider" />

      {/* Group 4: right-dock panel toggles. Spawner + Lighting share one
          exclusive slot (opening one closes the other — see lib/right-dock.ts),
          so their aria-pressed states are mutually exclusive. */}
      <div className="tb-group">
        <button
          type="button"
          className="tb-btn"
          aria-label="Toggle Spawner panel"
          title="Toggle Spawner panel"
          aria-pressed={spawnerVisible}
          onClick={() => toggleDock("spawner")}
        >
          <CirclePlus {...ICON} />
        </button>
        <button
          type="button"
          className="tb-btn"
          aria-label="Toggle Lighting panel"
          title="Toggle Lighting panel"
          aria-pressed={lightingVisible}
          onClick={() => toggleDock("lighting")}
        >
          <Lightbulb {...ICON} />
        </button>
      </div>

      <span className="tb-spacer" />

      {/* Group 5: environment */}
      <GroundDropdown bridge={bridge} />
      <BackgroundDropdown bridge={bridge} />
    </div>
  );
}
