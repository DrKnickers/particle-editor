// Toolbar — Particle Editor 2026 layout. Grouped sections with
// dividers, spacer to the right, theme toggle at the rightmost edge.
//
// Group 1 (file actions):       New · Open · Save · Save As
// Group 2 (edit):               Undo · Redo
// Group 3 (playback):           Play|Pause · Step · Step 10 · Timeline
//   While the timeline is open (lib/timeline.ts) the three transport
//   buttons drive it instead of free-run, which the host refuses then.
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
//   .toolbar, .tb-group, .tb-divider, .tb-spacer — and IconButton's default
//   "toolbar" variant (.tb-btn), which sets the tooltip and aria-label from
//   one label.

import {
  FilePlus,
  FolderOpen,
  Save,
  SaveAll,
  Undo2,
  Redo2,
  Play,
  Pause,
  ChevronRight,
  ChevronsRight,
  Film,
  Sparkles,
  CirclePlus,
  Lightbulb,
  LayoutGrid,
} from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { BackgroundPopover } from "@/components/BackgroundPopover";
import { ReferenceObjectPopover } from "@/components/ReferenceObjectPopover";
import { GroundPopover } from "@/components/GroundPopover";
import { useRightDock, toggleDock } from "@/lib/right-dock";
import { IconButton } from "@/primitives/IconButton";
import { replaceDocument, runFileOp } from "@/lib/file-op";
import { useEngineField } from "@/lib/use-engine-snapshot";
import { fireAndReport } from "@/lib/status-feedback";
import { setTimelinePlaying, stepTimeline, toggleTimeline, useTimeline } from "@/lib/timeline";

type Props = { bridge: Bridge };

const ICON = { className: "size-3.5" } as const;

export function Toolbar({ bridge }: Props) {
  const paused = useEngineField(bridge, (s) => s.paused) ?? false;
  const timelineActive = useTimeline((s) => s.active);
  const timelinePlaying = useTimeline((s) => s.playing);
  const playing = timelineActive ? timelinePlaying : !paused;
  const canUndo = useEngineField(bridge, (s) => s.canUndo) ?? false;
  const canRedo = useEngineField(bridge, (s) => s.canRedo) ?? false;
  // leave-particles is a sim-behaviour toggle (default on); the ground/grid/bloom
  // visibility toggles moved to the viewport display-options overlay.
  const leaveParticles = useEngineField(bridge, (s) => s.leaveParticles) ?? true;
  const dock = useRightDock();
  const spawnerVisible = dock === "spawner";
  const lightingVisible = dock === "lighting";
  const atlasVisible = dock === "atlas";
  const hasSelectedEmitter = useEngineField(bridge, (s) => s.selectedEmitterId) != null;

  return (
    <div data-testid="toolbar" className="toolbar">
      {/* Group 1: file actions. New / Open route through replaceDocument
          (the promptSaveChanges gate) so a dirty document gets the
          Save/Discard/Cancel prompt before being replaced (same gate the MenuBar uses). Save + Save As are
          themselves the save path so they don't need the gate. */}
      <div className="tb-group">
        <IconButton
          label="New"
          onClick={() => {
            replaceDocument(bridge, { kind: "file/new", params: {} });
          }}
        >
          <FilePlus {...ICON} />
        </IconButton>
        <IconButton
          label="Open"
          onClick={() => {
            replaceDocument(bridge, { kind: "file/open", params: {} });
          }}
        >
          <FolderOpen {...ICON} />
        </IconButton>
        <IconButton
          label="Save"
          onClick={() => {
            void runFileOp(bridge, { kind: "file/save", params: {} });
          }}
        >
          <Save {...ICON} />
        </IconButton>
        <IconButton
          label="Save As"
          onClick={() => {
            void runFileOp(bridge, { kind: "file/save-as", params: {} });
          }}
        >
          <SaveAll {...ICON} />
        </IconButton>
      </div>

      <span className="tb-divider" />

      {/* Group 2: edit (undo / redo). Drives the same `undo/perform` bridge
          kind + `canUndo`/`canRedo` engine-state the Edit menu uses; each
          button is disabled when there's nothing to undo / redo. */}
      <div className="tb-group">
        <IconButton
          label="Undo"
          disabled={!canUndo}
          onClick={() => {
            void fireAndReport(bridge, { kind: "undo/perform", params: { direction: "undo" } }, "Undo");
          }}
        >
          <Undo2 {...ICON} />
        </IconButton>
        <IconButton
          label="Redo"
          disabled={!canRedo}
          onClick={() => {
            void fireAndReport(bridge, { kind: "undo/perform", params: { direction: "redo" } }, "Redo");
          }}
        >
          <Redo2 {...ICON} />
        </IconButton>
      </div>

      <span className="tb-divider" />

      {/* Group 3: playback */}
      <div className="tb-group">
        <IconButton
          label={playing ? "Pause" : "Play"}
          pressed={playing}
          onClick={() => {
            if (timelineActive) void setTimelinePlaying(bridge, !timelinePlaying);
            else
              void fireAndReport(
                bridge,
                { kind: "engine/set/paused", params: { paused: !paused } },
                paused ? "Play" : "Pause",
              );
          }}
        >
          {playing ? <Pause {...ICON} /> : <Play {...ICON} />}
        </IconButton>
        <IconButton
          label="Step"
          tip="Step one frame"
          onClick={() => {
            if (timelineActive) stepTimeline(bridge, 1);
            else void fireAndReport(bridge, { kind: "engine/action/step-frames", params: { frames: 1 } }, "Step");
          }}
        >
          <ChevronRight {...ICON} />
        </IconButton>
        <IconButton
          label="Step 10"
          tip="Step 10 frames"
          onClick={() => {
            if (timelineActive) stepTimeline(bridge, 10);
            else void fireAndReport(bridge, { kind: "engine/action/step-frames", params: { frames: 10 } }, "Step 10");
          }}
        >
          <ChevronsRight {...ICON} />
        </IconButton>
        <IconButton
          label="Toggle timeline"
          tip="Timeline: replay the effect frame by frame"
          pressed={timelineActive}
          onClick={() => {
            void toggleTimeline(bridge);
          }}
        >
          <Film {...ICON} />
        </IconButton>
      </div>

      <span className="tb-divider" />

      {/* Group 4: viewport toggle. The ground / grid / bloom toggles moved to the
          bottom-left viewport display-options overlay (ViewportToggleOverlay);
          leave-particles is a sim-behaviour toggle and stays here. */}
      <div className="tb-group">
        <IconButton
          label="Leave particles after instance death"
          pressed={leaveParticles}
          onClick={() => {
            void fireAndReport(
              bridge,
              { kind: "engine/set/leave-particles", params: { enabled: !leaveParticles } },
              "Leave particles",
            );
          }}
        >
          <Sparkles {...ICON} />
        </IconButton>
      </div>

      <span className="tb-divider" />

      {/* Group 5: right-dock panel toggles. Spawner, Lighting, and Atlas share
          one exclusive slot (opening one closes the others — see lib/right-dock.ts),
          so their aria-pressed states are mutually exclusive. */}
      <div className="tb-group">
        <IconButton label="Toggle Spawner panel" pressed={spawnerVisible} onClick={() => toggleDock("spawner")}>
          <CirclePlus {...ICON} />
        </IconButton>
        <IconButton label="Toggle Lighting panel" pressed={lightingVisible} onClick={() => toggleDock("lighting")}>
          <Lightbulb {...ICON} />
        </IconButton>
        <IconButton
          label="Toggle Atlas frame picker"
          pressed={atlasVisible}
          disabled={!hasSelectedEmitter}
          onClick={() => toggleDock("atlas")}
        >
          <LayoutGrid {...ICON} />
        </IconButton>
      </div>

      <span className="tb-spacer" />

      {/* Group 6: environment */}
      <GroundPopover bridge={bridge} />
      <BackgroundPopover bridge={bridge} />
      <ReferenceObjectPopover bridge={bridge} />
    </div>
  );
}
