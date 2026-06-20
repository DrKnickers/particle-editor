// Toolbar — Particle Editor 2026 layout. Grouped sections with
// dividers, spacer to the right, theme toggle at the rightmost edge.
//
// Group 1 (file actions):       New · Open · Save · Save As
// Group 2 (edit):               Undo · Redo
// Group 3 (playback):           Play|Pause · Step · Step 10
// Group 4 (viewport toggles):   Show ground · Show grid · Toggle bloom · Leave particles
// Group 5 (panels):             Spawner toggle
//   spacer
// Group 6 (environment):        Ground dropdown · Background dropdown
//
// Stop and Restart removed per design chat. The three viewport toggles
// (ground / bloom / leave-particles) live here as lucide icon buttons —
// they replaced the floating ViewportPill. Undo/Redo also appear in the
// Edit menu; Reload Shaders/Textures lives in the menubar only.
//
// Uses the design's semantic CSS classes from components.css:
//   .toolbar, .tb-group, .tb-btn, .tb-divider, .tb-spacer

import { useEffect, useState } from "react";
import {
  FilePlus, FolderOpen, Save, SaveAll,
  Undo2, Redo2,
  Play, Pause, ChevronRight, ChevronsRight,
  Grid2x2, PanelBottom, Sun, Sparkles, CirclePlus, Lightbulb, LayoutGrid,
} from "lucide-react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { BackgroundDropdown } from "@/components/BackgroundDropdown";
import { ReferenceObjectDropdown } from "@/components/ReferenceObjectDropdown";
import { GroundDropdown } from "@/components/GroundDropdown";
import { useRightDock, toggleDock } from "@/lib/right-dock";
import { Tip } from "@/primitives/Tip";
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
  const canUndo = state?.canUndo ?? false;
  const canRedo = state?.canRedo ?? false;
  // Viewport engine toggles (formerly the floating ViewportPill). Defaults
  // match the pill: ground/bloom off, leave-particles on.
  const ground = state?.ground ?? false;
  const gridVisible = state?.gridVisible ?? false;
  const bloom = state?.bloom ?? false;
  const leaveParticles = state?.leaveParticles ?? true;
  const dock = useRightDock();
  const spawnerVisible = dock === "spawner";
  const lightingVisible = dock === "lighting";
  const atlasVisible = dock === "atlas";
  const hasSelectedEmitter = state != null && state.selectedEmitterId != null;

  return (
    <div data-testid="toolbar" className="toolbar">
      {/* Group 1: file actions. New / Open route through promptSaveChanges
          so a dirty document gets the Save/Discard/Cancel prompt before
          being replaced (same gate the MenuBar uses). Save + Save As are
          themselves the save path so they don't need the gate. */}
      <div className="tb-group">
        <Tip content="New" occlusionId="tip:toolbar:new">
          <button
            type="button"
            className="tb-btn"
            aria-label="New"
            onClick={() => {
              promptSaveChanges(async () => {
                await bridge.request({ kind: "file/new", params: {} });
              });
            }}
          >
            <FilePlus {...ICON} />
          </button>
        </Tip>
        <Tip content="Open" occlusionId="tip:toolbar:open">
          <button
            type="button"
            className="tb-btn"
            aria-label="Open"
            onClick={() => {
              promptSaveChanges(async () => {
                await runFileOp(bridge, { kind: "file/open", params: {} });
              });
            }}
          >
            <FolderOpen {...ICON} />
          </button>
        </Tip>
        <Tip content="Save" occlusionId="tip:toolbar:save">
          <button
            type="button"
            className="tb-btn"
            aria-label="Save"
            onClick={() => { void runFileOp(bridge, { kind: "file/save", params: {} }); }}
          >
            <Save {...ICON} />
          </button>
        </Tip>
        <Tip content="Save As" occlusionId="tip:toolbar:save-as">
          <button
            type="button"
            className="tb-btn"
            aria-label="Save As"
            onClick={() => { void runFileOp(bridge, { kind: "file/save-as", params: {} }); }}
          >
            <SaveAll {...ICON} />
          </button>
        </Tip>
      </div>

      <span className="tb-divider" />

      {/* Group 2: edit (undo / redo). Drives the same `undo/perform` bridge
          kind + `canUndo`/`canRedo` engine-state the Edit menu uses; each
          button is disabled when there's nothing to undo / redo. */}
      <div className="tb-group">
        <Tip content="Undo" occlusionId="tip:toolbar:undo">
          <button
            type="button"
            className="tb-btn"
            aria-label="Undo"
            disabled={!canUndo}
            onClick={() => { void bridge.request({ kind: "undo/perform", params: { direction: "undo" } }); }}
          >
            <Undo2 {...ICON} />
          </button>
        </Tip>
        <Tip content="Redo" occlusionId="tip:toolbar:redo">
          <button
            type="button"
            className="tb-btn"
            aria-label="Redo"
            disabled={!canRedo}
            onClick={() => { void bridge.request({ kind: "undo/perform", params: { direction: "redo" } }); }}
          >
            <Redo2 {...ICON} />
          </button>
        </Tip>
      </div>

      <span className="tb-divider" />

      {/* Group 3: playback */}
      <div className="tb-group">
        <Tip content={paused ? "Play" : "Pause"} occlusionId="tip:toolbar:play-pause">
          <button
            type="button"
            className="tb-btn"
            aria-label={paused ? "Play" : "Pause"}
            aria-pressed={!paused}
            onClick={() => { void bridge.request({ kind: "engine/set/paused", params: { paused: !paused } }); }}
          >
            {paused ? <Play {...ICON} /> : <Pause {...ICON} />}
          </button>
        </Tip>
        <Tip content="Step one frame" occlusionId="tip:toolbar:step">
          <button
            type="button"
            className="tb-btn"
            aria-label="Step"
            onClick={() => { void bridge.request({ kind: "engine/action/step-frames", params: { frames: 1 } }); }}
          >
            <ChevronRight {...ICON} />
          </button>
        </Tip>
        <Tip content="Step 10 frames" occlusionId="tip:toolbar:step-10">
          <button
            type="button"
            className="tb-btn"
            aria-label="Step 10"
            onClick={() => { void bridge.request({ kind: "engine/action/step-frames", params: { frames: 10 } }); }}
          >
            <ChevronsRight {...ICON} />
          </button>
        </Tip>
      </div>

      <span className="tb-divider" />

      {/* Group 4: viewport engine toggles. aria-pressed + aria-labels are
          ported verbatim from the old ViewportPill so a11y semantics are
          preserved. Each reads the live engine snapshot and dispatches the
          matching engine/set/* with the inverted value. */}
      <div className="tb-group">
        <Tip content="Show ground" occlusionId="tip:toolbar:show-ground">
          <button
            type="button"
            className="tb-btn"
            aria-label="Show ground"
            aria-pressed={ground}
            onClick={() => { void bridge.request({ kind: "engine/set/ground", params: { enabled: !ground } }); }}
          >
            <PanelBottom {...ICON} />
          </button>
        </Tip>
        <Tip content="Show grid" occlusionId="tip:toolbar:show-grid">
          <button
            type="button"
            className="tb-btn"
            aria-label="Show grid"
            aria-pressed={gridVisible}
            onClick={() => { void bridge.request({ kind: "engine/set/grid-visible", params: { visible: !gridVisible } }); }}
          >
            <Grid2x2 {...ICON} />
          </button>
        </Tip>
        <Tip content="Toggle bloom" occlusionId="tip:toolbar:toggle-bloom">
          <button
            type="button"
            className="tb-btn"
            aria-label="Toggle bloom"
            aria-pressed={bloom}
            onClick={() => { void bridge.request({ kind: "engine/set/bloom", params: { enabled: !bloom } }); }}
          >
            <Sun {...ICON} />
          </button>
        </Tip>
        <Tip content="Leave particles after instance death" occlusionId="tip:toolbar:leave-particles">
          <button
            type="button"
            className="tb-btn"
            aria-label="Leave particles after instance death"
            aria-pressed={leaveParticles}
            onClick={() => { void bridge.request({ kind: "engine/set/leave-particles", params: { enabled: !leaveParticles } }); }}
          >
            <Sparkles {...ICON} />
          </button>
        </Tip>
      </div>

      <span className="tb-divider" />

      {/* Group 5: right-dock panel toggles. Spawner, Lighting, and Atlas share
          one exclusive slot (opening one closes the others — see lib/right-dock.ts),
          so their aria-pressed states are mutually exclusive. */}
      <div className="tb-group">
        <Tip content="Toggle Spawner panel" occlusionId="tip:toolbar:toggle-spawner">
          <button
            type="button"
            className="tb-btn"
            aria-label="Toggle Spawner panel"
            aria-pressed={spawnerVisible}
            onClick={() => toggleDock("spawner")}
          >
            <CirclePlus {...ICON} />
          </button>
        </Tip>
        <Tip content="Toggle Lighting panel" occlusionId="tip:toolbar:toggle-lighting">
          <button
            type="button"
            className="tb-btn"
            aria-label="Toggle Lighting panel"
            aria-pressed={lightingVisible}
            onClick={() => toggleDock("lighting")}
          >
            <Lightbulb {...ICON} />
          </button>
        </Tip>
        <Tip content="Toggle Atlas frame picker" occlusionId="tip:toolbar:toggle-atlas">
          <button
            type="button"
            className="tb-btn"
            aria-label="Toggle Atlas frame picker"
            aria-pressed={atlasVisible}
            disabled={!hasSelectedEmitter}
            onClick={() => toggleDock("atlas")}
          >
            <LayoutGrid {...ICON} />
          </button>
        </Tip>
      </div>

      <span className="tb-spacer" />

      {/* Group 6: environment */}
      <GroundDropdown bridge={bridge} />
      <BackgroundDropdown bridge={bridge} />
      <ReferenceObjectDropdown bridge={bridge} />
    </div>
  );
}
