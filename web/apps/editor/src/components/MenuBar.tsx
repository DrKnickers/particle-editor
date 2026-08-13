// React-rendered menu bar using Radix UI Menubar.
//
// Restructured to legacy top-level order
//   File / Edit / Emitters / Mods / View / Help
// (legacy [src/ParticleEditor.en.rc:565-630]). Changes vs the original
// 5-menu shape:
//   - Added top-level `Emitters` menu with New Emitter submenu
//     (Root / Lifetime Child / Death Child), Rename Emitter (via
//     `tree-action` atom), Rescale Emitter… (via `tree-context`
//     atom), Spawner… (was under Tools), plus disabled placeholders
//     for Toggle Visibility / Show All / Hide All (design lock —
//     wiring deferred to a future polish batch).
//   - Promoted `Mods` from a Tools submenu to a top-level menu.
//     Placeholder list unchanged (dynamic mod detection still
//     deferred).
//   - Moved `Lighting…` and `Bloom Settings…` from Tools to View.
//   - Removed `Tools` menu entirely (its remaining item, Spawner,
//     lives in Emitters now).
// All items wired to existing bridge calls + atoms; deferred items
// log a `[Menu] X — TODO` marker and render as `disabled`.

import { useState } from "react";
import * as Menubar from "@radix-ui/react-menubar";
import { Check, ChevronRight } from "lucide-react";
import type {
  Bridge,
  EmitterTreeNode,
} from "@particle-editor/bridge-schema";
import { promptSaveChanges, useFileState } from "@/lib/file-state";
import { runFileOp } from "@/lib/file-op";
import { useEngineField } from "@/lib/use-engine-snapshot";
import { basename } from "@/lib/paths";
import { requestDeleteEmitters } from "@/lib/delete-emitters";
import { announceWhenOk } from "@/lib/status-feedback";
import { bumpTextureEpoch } from "@/lib/atlas-preview-cache";
import {
  useEmitterSelectionPrimary,
  useEmitterSelectionIds,
  getEmitterSelectionSnapshot,
} from "@/lib/emitter-selection";
import {
  markEmittersCopied,
  useEmitterClipboardHasContent,
} from "@/lib/emitter-clipboard";
import { useTreeContextStore } from "@/lib/tree-context";
import { requestEmitterRename } from "@/lib/tree-action";
import { toggleDock } from "@/lib/right-dock";
import { RESET_CAMERA } from "@/lib/reset-camera";
import { Modal } from "@/components/Modal";
import { PreferencesDialog } from "@/screens/PreferencesDialog";
import { ShortcutsDialog } from "@/screens/ShortcutsDialog";
import { ModsMenu } from "./ModsMenu";

type Props = {
  bridge: Bridge;
  onOpenImportEmittersDialog: () => void;
  onOpenAboutDialog: () => void;
  onOpenRescaleDialog: () => void;
  onResetPanelLayout: () => void;
};

// Style constants — shared across triggers and items so the Tailwind
// class strings don't drift between menus.
const TRIGGER =
  // focus-ring: keyboard focus on a CLOSED trigger was invisible (outline-none
  // with no replacement) — a 2.4.7 gap the PRODUCT.md conformance check caught.
  "px-2 py-1 text-xs font-medium text-text-2 transition-colors motion-reduce:transition-none hover:bg-bg-2 rounded data-[state=open]:bg-bg-2 data-[state=open]:text-text outline-none focus-ring select-none cursor-default";
const CONTENT =
  "min-w-[200px] bg-bg-2 border border-border rounded-md shadow-[var(--shadow-soft)] p-1 z-50 popover-animate";
const ITEM =
  "flex items-center gap-2 px-2 py-1 text-xs text-text rounded hover:bg-panel-2 data-[highlighted]:bg-panel-2 outline-none cursor-pointer data-[disabled]:text-text-3 data-[disabled]:opacity-40 data-[disabled]:cursor-not-allowed data-[disabled]:hover:bg-transparent select-none";
const SEPARATOR = "my-1 h-px bg-panel-2";

function Hint({ children }: { children: string }) {
  return <span className="ml-auto text-[10px] text-text-3">{children}</span>;
}

function CheckSlot({ active }: { active: boolean }) {
  return (
    <span className="size-3.5 shrink-0 flex items-center justify-center">
      {active && <Check className="size-3.5" />}
    </span>
  );
}

// Depth-first search for a node by id in the emitter tree returned by
// `emitters/list`. Used by Emitters → Toggle Visibility to read the
// primary emitter's current `visible` flag at click time (one-shot, so the
// menu doesn't hold a standing tree subscription).
function findTreeNode(node: EmitterTreeNode, id: number): EmitterTreeNode | null {
  if (node.id === id) return node;
  for (const child of node.children) {
    const found = findTreeNode(child, id);
    if (found !== null) return found;
  }
  return null;
}

/** Extract the basename from a full path for the Recent Files submenu
 *  labels. Splits on the last `/` or `\\`; falls back to the whole
 *  string. */
export function MenuBar({
  bridge,
  onOpenImportEmittersDialog,
  onOpenAboutDialog,
  onOpenRescaleDialog,
  onResetPanelLayout,
}: Props) {
  // View → Reset View Settings prompt visibility.
  const [resetViewOpen, setResetViewOpen] = useState(false);
  const [prefsOpen, setPrefsOpen] = useState(false);
  const [shortcutsOpen, setShortcutsOpen] = useState(false);
  const [menuValue, setMenuValue] = useState("");

  const handleResetViewConfirm = async () => {
    setResetViewOpen(false);
    await bridge.request({
      kind: "engine/action/reset-view-settings",
      params: {},
    });
  };

  const ground = useEngineField(bridge, (s) => s.ground) ?? false;
  const gridVisible = useEngineField(bridge, (s) => s.gridVisible) ?? false;
  const refLocked = useEngineField(bridge, (s) => s.referenceObjectLocked) ?? false;
  const hasRefObject = (useEngineField(bridge, (s) => s.referenceObjectName) ?? "") !== "";
  // Bloom on/off now lives in the viewport display-options overlay (it moved off
  // the toolbar); its settings stay in the Lighting pane — so no bloom state is
  // read here (no View-menu Bloom item).
  const paused = useEngineField(bridge, (s) => s.paused) ?? false;
  const heatDebug = useEngineField(bridge, (s) => s.heatDebug) ?? false;
  const canUndo = useEngineField(bridge, (s) => s.canUndo) ?? false;
  const canRedo = useEngineField(bridge, (s) => s.canRedo) ?? false;

  // Primary selection drives the Emitters-menu item enabled state.
  // Rename / Rescale / Add Child operate on the primary; Add Root is
  // selection-independent.
  const primaryEmitterId = useEmitterSelectionPrimary();
  const hasPrimary = primaryEmitterId !== null;

  // File-menu wiring needs the recent-files list +
  // the prompt-save-changes helper.
  const { recentFiles } = useFileState();

  // Edit-menu clipboard + delete act on the current emitter
  // selection — the same actions the tree's Ctrl+C/X/V/Del use. Paste gates
  // on whether anything has been copied this session.
  const selectedIds = useEmitterSelectionIds();
  const hasSelection = selectedIds.length > 0;
  const hasClipboard = useEmitterClipboardHasContent();

  const send =
    (req: Parameters<Bridge["request"]>[0]) =>
    () => {
      void bridge.request(req);
    };

  // ── File menu handlers ───────────────────────────────────────────
  // All destructive ops (New / Open / Recent) route through
  // promptSaveChanges() which gates on the current dirty flag and
  // either runs the action immediately (clean) or pops the
  // SaveChangesPrompt (dirty). Save / Save As don't need the gate —
  // they ARE the save path.

  const handleNew = () => {
    promptSaveChanges(async () => {
      await bridge.request({ kind: "file/new", params: {} });
    });
  };

  const handleOpen = () => {
    promptSaveChanges(async () => {
      await runFileOp(bridge, { kind: "file/open", params: {} });
    });
  };

  const handleSave = () => {
    void runFileOp(bridge, { kind: "file/save", params: {} });
  };

  const handleSaveAs = () => {
    void runFileOp(bridge, { kind: "file/save-as", params: {} });
  };

  const handleOpenRecent = (path: string) => {
    promptSaveChanges(async () => {
      await runFileOp(bridge, { kind: "file/open", params: { path } });
    });
  };

  const handleExit = () => {
    // Route through promptSaveChanges so a dirty
    // particle system gets the Save/Discard/Cancel prompt before
    // the host tears down. app/quit posts WM_CLOSE on the host
    // side; the existing WM_DESTROY chain handles compositor +
    // engine cleanup. Cancel from the prompt is a silent no-op,
    // matching legacy DoCheckChanges semantics.
    promptSaveChanges(async () => {
      await bridge.request({ kind: "app/quit", params: {} });
    });
  };

  // ── Emitters menu handlers ─────────────────────────────────

  const handleAddRoot = () => {
    announceWhenOk(bridge.request({ kind: "emitters/add-root", params: {} }), "Added emitter — Ctrl+Z to undo");
  };

  const handleAddLifetimeChild = () => {
    if (primaryEmitterId === null) return;
    announceWhenOk(bridge.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: primaryEmitterId },
    }), "Added lifetime child — Ctrl+Z to undo");
  };

  const handleAddDeathChild = () => {
    if (primaryEmitterId === null) return;
    announceWhenOk(bridge.request({
      kind: "emitters/add-death-child",
      params: { parentId: primaryEmitterId },
    }), "Added death child — Ctrl+Z to undo");
  };

  const handleRenameEmitter = () => {
    if (primaryEmitterId === null) return;
    requestEmitterRename(primaryEmitterId);
  };

  const handleRescaleEmitter = () => {
    if (primaryEmitterId === null) return;
    useTreeContextStore.getState().openDialog("rescale", primaryEmitterId);
  };

  // ── Edit-menu clipboard / delete ─────────────────────────
  // Snapshot the selection at click time (not the render-time `selectedIds`)
  // so the action always uses the live set.
  const handleCopy = () => {
    const ids = getEmitterSelectionSnapshot().ids;
    if (ids.length === 0) return;
    void bridge.request({ kind: "emitters/copy", params: { ids } });
    markEmittersCopied();
  };
  const handleCut = () => {
    const ids = getEmitterSelectionSnapshot().ids;
    if (ids.length === 0) return;
    announceWhenOk(bridge.request({ kind: "emitters/cut", params: { ids } }), `Cut ${ids.length === 1 ? "emitter" : `${ids.length} emitters`} — Ctrl+Z to undo`);
    markEmittersCopied();
  };
  const handlePaste = () => {
    announceWhenOk(bridge.request({ kind: "emitters/paste", params: {} }), "Pasted — Ctrl+Z to undo");
  };
  const handleDeleteSelection = () => {
    const ids = getEmitterSelectionSnapshot().ids;
    if (ids.length === 0) return;
    requestDeleteEmitters(bridge, ids);
  };

  // ── Emitters-menu visibility ─────────────────────────────
  const handleShowAll = () => {
    void bridge.request({ kind: "emitters/set-all-visible", params: { visible: true } });
  };
  const handleHideAll = () => {
    void bridge.request({ kind: "emitters/set-all-visible", params: { visible: false } });
  };
  const handleToggleVisibility = async () => {
    if (primaryEmitterId === null) return;
    try {
      const t = await bridge.request({ kind: "emitters/list", params: {} });
      const node = findTreeNode(t.root, primaryEmitterId);
      if (node === null) return;
      void bridge.request({
        kind: "emitters/set-visible",
        params: { id: primaryEmitterId, visible: !node.visible },
      });
    } catch (err) {
      console.warn("[MenuBar] toggle-visibility failed:", err);
    }
  };

  return (
    <>
    <Menubar.Root className="flex items-center gap-0.5" value={menuValue} onValueChange={setMenuValue}>
      {/* ─── File ─── */}
      <Menubar.Menu value="file">
        <Menubar.Trigger className={TRIGGER}>File</Menubar.Trigger>
        <Menubar.Portal>
          <Menubar.Content
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            <Menubar.Item className={ITEM} onSelect={handleNew}>
              New<Hint>Ctrl+N</Hint>
            </Menubar.Item>
            <Menubar.Item className={ITEM} onSelect={handleOpen}>
              Open…<Hint>Ctrl+O</Hint>
            </Menubar.Item>
            <Menubar.Item className={ITEM} onSelect={handleSave}>
              Save<Hint>Ctrl+S</Hint>
            </Menubar.Item>
            <Menubar.Item className={ITEM} onSelect={handleSaveAs}>
              Save As…
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item
              className={ITEM}
              onSelect={() => onOpenImportEmittersDialog()}
            >
              Import Emitters…
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Sub>
              <Menubar.SubTrigger className={ITEM}>
                Recent Files
                <ChevronRight className="ml-auto size-3.5" />
              </Menubar.SubTrigger>
              <Menubar.Portal>
                <Menubar.SubContent
                  className={CONTENT}
                  sideOffset={2}
                  alignOffset={-4}
                >
                  {recentFiles.length === 0 ? (
                    <Menubar.Item className={ITEM} disabled>
                      (none)
                    </Menubar.Item>
                  ) : (
                    recentFiles.map((path) => (
                      <Menubar.Item
                        key={path}
                        className={ITEM}
                        onSelect={() => handleOpenRecent(path)}
                      >
                        {basename(path)}
                      </Menubar.Item>
                    ))
                  )}
                </Menubar.SubContent>
              </Menubar.Portal>
            </Menubar.Sub>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item className={ITEM} onSelect={handleExit}>
              Exit<Hint>Alt+F4</Hint>
            </Menubar.Item>
          </Menubar.Content>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── Edit ─── */}
      <Menubar.Menu value="edit">
        <Menubar.Trigger className={TRIGGER}>Edit</Menubar.Trigger>
        <Menubar.Portal>
          <Menubar.Content
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            <Menubar.Item
              className={ITEM}
              disabled={!canUndo}
              onSelect={send({
                kind: "undo/perform",
                params: { direction: "undo" },
              })}
            >
              Undo<Hint>Ctrl+Z</Hint>
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!canRedo}
              onSelect={send({
                kind: "undo/perform",
                params: { direction: "redo" },
              })}
            >
              Redo<Hint>Ctrl+Shift+Z</Hint>
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item
              className={ITEM}
              disabled={!hasSelection}
              onSelect={handleCut}
            >
              Cut<Hint>Ctrl+X</Hint>
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!hasSelection}
              onSelect={handleCopy}
            >
              Copy<Hint>Ctrl+C</Hint>
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!hasClipboard}
              onSelect={handlePaste}
            >
              Paste<Hint>Ctrl+V</Hint>
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!hasSelection}
              onSelect={handleDeleteSelection}
            >
              Delete<Hint>Del</Hint>
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item className={ITEM} onSelect={() => onOpenRescaleDialog()}>
              Rescale…
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              onSelect={send({
                kind: "engine/action/clear",
                params: {},
              })}
            >
              Clear All Particles<Hint>Ctrl+Del</Hint>
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item className={ITEM} onSelect={() => setPrefsOpen(true)}>
              Preferences…
            </Menubar.Item>
          </Menubar.Content>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── Emitters ─── */}
      <Menubar.Menu value="emitters">
        <Menubar.Trigger className={TRIGGER}>Emitters</Menubar.Trigger>
        <Menubar.Portal>
          <Menubar.Content
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            <Menubar.Sub>
              <Menubar.SubTrigger className={ITEM}>
                New Emitter
                <ChevronRight className="ml-auto size-3.5" />
              </Menubar.SubTrigger>
              <Menubar.Portal>
                <Menubar.SubContent
                  className={CONTENT}
                  sideOffset={2}
                  alignOffset={-4}
                >
                  <Menubar.Item className={ITEM} onSelect={handleAddRoot}>
                    Root Emitter
                  </Menubar.Item>
                  <Menubar.Item
                    className={ITEM}
                    disabled={!hasPrimary}
                    onSelect={handleAddLifetimeChild}
                  >
                    Lifetime Child
                  </Menubar.Item>
                  <Menubar.Item
                    className={ITEM}
                    disabled={!hasPrimary}
                    onSelect={handleAddDeathChild}
                  >
                    Death Child
                  </Menubar.Item>
                </Menubar.SubContent>
              </Menubar.Portal>
            </Menubar.Sub>
            <Menubar.Item
              className={ITEM}
              disabled={!hasPrimary}
              onSelect={handleRenameEmitter}
            >
              Rename Emitter<Hint>F2</Hint>
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!hasPrimary}
              onSelect={handleRescaleEmitter}
            >
              Rescale Emitter…
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            {/* Toggle Visibility acts on the primary selection (reads
                its current `visible` via a one-shot list); Show/Hide All use
                set-all-visible. The per-row eye affordance covers per-row
                toggling; these mirror the legacy Emitters-menu commands. */}
            <Menubar.Item
              className={ITEM}
              disabled={!hasPrimary}
              onSelect={handleToggleVisibility}
            >
              Toggle Visibility
            </Menubar.Item>
            <Menubar.Item className={ITEM} onSelect={handleShowAll}>
              Show All Emitters
            </Menubar.Item>
            <Menubar.Item className={ITEM} onSelect={handleHideAll}>
              Hide All Emitters
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item className={ITEM} onSelect={() => toggleDock("spawner")}>
              Spawner<Hint>F7</Hint>
            </Menubar.Item>
          </Menubar.Content>
        </Menubar.Portal>
      </Menubar.Menu>

      <ModsMenu
        bridge={bridge}
        onMenuValueChange={setMenuValue}
        triggerClass={TRIGGER}
        contentClass={CONTENT}
        itemClass={ITEM}
        separatorClass={SEPARATOR}
      />

      {/* ─── View ─── */}
      <Menubar.Menu value="view">
        <Menubar.Trigger className={TRIGGER}>View</Menubar.Trigger>
        <Menubar.Portal>
          <Menubar.Content
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            <Menubar.Item
              className={ITEM}
              onSelect={send({
                kind: "engine/set/ground",
                params: { enabled: !ground },
              })}
            >
              <CheckSlot active={ground} />
              Ground
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              onSelect={send({
                kind: "engine/set/grid-visible",
                params: { visible: !gridVisible },
              })}
            >
              <CheckSlot active={gridVisible} />
              Grid
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!hasRefObject}
              onSelect={send({
                kind: "engine/set/reference-object-lock",
                params: { locked: !refLocked },
              })}
            >
              <CheckSlot active={refLocked} />
              Lock reference object
            </Menubar.Item>
            {/* Lighting opens the docked right-dock pane (shared with the
                Spawner). Bloom is fully handled elsewhere:
                its settings live as a section inside the Lighting pane, and its
                on/off toggle is in the viewport display-options overlay (it
                moved off the toolbar) — so the former View-menu "Bloom" +
                "Bloom Settings…" items stay retired. */}
            <Menubar.Item
              className={ITEM}
              onSelect={() => toggleDock("lighting")}
            >
              <CheckSlot active={false} />
              Lighting…
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              onSelect={() => toggleDock("atlas")}
            >
              <CheckSlot active={false} />
              Atlas Frame Picker…
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item
              className={ITEM}
              onSelect={send({
                kind: "engine/set/paused",
                params: { paused: !paused },
              })}
            >
              <CheckSlot active={paused} />
              Pause<Hint>F8</Hint>
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!paused}
              onSelect={send({
                kind: "engine/action/step-frames",
                params: { frames: 1 },
              })}
            >
              <CheckSlot active={false} />
              Step Forward
            </Menubar.Item>
            {/* Dispatches engine/set/camera with the legacy
                default vectors. Shares RESET_CAMERA with the Ctrl+Home
                accelerator (lib/reset-camera.ts) so the two can't drift —
                no new bridge kind required, the camera setter already exists. */}
            <Menubar.Item
              className={ITEM}
              onSelect={send({
                kind: "engine/set/camera",
                params: RESET_CAMERA,
              })}
            >
              <CheckSlot active={false} />
              Reset Camera
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item
              className={ITEM}
              onSelect={send({
                kind: "engine/action/reload-shaders",
                params: {},
              })}
            >
              <CheckSlot active={false} />
              Reload Shaders
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              onSelect={() => {
                void bridge
                  .request({ kind: "engine/action/reload-textures", params: {} })
                  .then(() => bumpTextureEpoch()) // re-fetch atlas previews with fresh content
                  .catch(() => {}); // host-side reload failure shouldn't surface as an unhandled rejection
              }}
            >
              <CheckSlot active={false} />
              Reload Textures
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item
              className={ITEM}
              onSelect={send({
                kind: "engine/set/heat-debug",
                params: { enabled: !heatDebug },
              })}
            >
              <CheckSlot active={heatDebug} />
              Heat Debug
            </Menubar.Item>
            {/* Pop the confirm modal; the modal's
                Reset button fires engine/action/reset-view-settings
                which cascades background / ground / bloom / skydome
                back to defaults in one host-side action. Lighting
                reset rides separately. */}
            <Menubar.Item
              className={ITEM}
              onSelect={() => setResetViewOpen(true)}
            >
              <CheckSlot active={false} />
              Reset View Settings
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            {/* Clears the four alo:layout:* localStorage keys
                and remounts PanelLayout (via an epoch bump in App.tsx)
                so every Group reads in-code defaults on next mount. No
                confirm prompt — the gesture is cheap to recover from
                (just drag the splitters back). */}
            <Menubar.Item
              className={ITEM}
              onSelect={() => onResetPanelLayout()}
            >
              <CheckSlot active={false} />
              Reset panel layout
            </Menubar.Item>
          </Menubar.Content>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── Help ─── */}
      <Menubar.Menu value="help">
        <Menubar.Trigger className={TRIGGER}>Help</Menubar.Trigger>
        <Menubar.Portal>
          <Menubar.Content
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            <Menubar.Item
              className={ITEM}
              data-testid="menu-help-shortcuts"
              onSelect={() => setShortcutsOpen(true)}
            >
              Keyboard Shortcuts&#8230;
            </Menubar.Item>
            <Menubar.Item className={ITEM} onSelect={() => onOpenAboutDialog()}>
              About
            </Menubar.Item>
          </Menubar.Content>
        </Menubar.Portal>
      </Menubar.Menu>
    </Menubar.Root>

    <PreferencesDialog bridge={bridge} open={prefsOpen} onOpenChange={setPrefsOpen} />

    {/* Help -> Keyboard Shortcuts... (design follow-ups, F1) */}
    <ShortcutsDialog open={shortcutsOpen} onOpenChange={setShortcutsOpen} />

    {/* Confirm prompt for View → Reset View Settings.
        Body copy preserves the established reset-settings wording.
        Sits as a sibling of Menubar.Root rather than inside it so
        Radix's child-list semantics for keyboard nav aren't disturbed.
        Modal manages its own portal, so DOM position doesn't matter. */}
    <Modal
      open={resetViewOpen}
      onOpenChange={setResetViewOpen}
      title="Reset View Settings"
      size="sm"
    >
      <Modal.Body>
        <p className="text-sm text-text-2">
          Reset background color, ground plane visibility, ground texture,
          ground Z offset, skydome, and bloom to defaults?
        </p>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton>Cancel</Modal.CancelButton>
        <Modal.OkButton onClick={() => void handleResetViewConfirm()}>Reset</Modal.OkButton>
      </Modal.Footer>
    </Modal>
    </>
  );
}
