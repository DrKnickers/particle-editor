// Phase 3 Screen 2 — React-rendered menu bar using Radix UI Menubar.
//
// Phase 4.1 Fix dispatch 5: restructured to legacy top-level order
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

import { useEffect, useRef, useState, type ComponentProps } from "react";
import * as Menubar from "@radix-ui/react-menubar";
import { Check, ChevronRight } from "lucide-react";
import type {
  Bridge,
  EngineStateDto,
  EmitterTreeNode,
  ModDescriptor,
} from "@particle-editor/bridge-schema";
import { promptSaveChanges, useFileState } from "@/lib/file-state";
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
import { useViewportOcclusion } from "@/lib/viewport-occlusion";
import { toggleDock } from "@/lib/right-dock";
import { RESET_CAMERA } from "@/lib/reset-camera";
import { Modal } from "@/components/Modal";

// follow-up: each MenubarContent needs to register itself with the
// host as a viewport occlusion while open so the popup punches a
// SetWindowRgn hole over the menu rect and the menu HTML shows
// through. This wrapper uses a ref + the useViewportOcclusion hook,
// scoped to the time the menu is mounted (Radix only mounts content
// while the menu is open, so the hook auto-cleans on close).
type MenuContentProps = ComponentProps<typeof Menubar.Content> & {
  bridge: Bridge;
  occlusionId: string;
};

function OccludingMenubarContent({
  bridge,
  occlusionId,
  children,
  ...rest
}: MenuContentProps) {
  const ref = useRef<HTMLDivElement | null>(null);
  //: pad the occlusion rect outward by ~24 CSS px to enclose
  // the menu's shadow-xl drop shadow + rounded-md corners, AND set
  // the compositor's smoothstep feather to the same 24 px. The popup
  // alpha then ramps from full-viewport at the padded outer edge to
  // full-cut at the menu's actual outline — no purple halo where
  // alpha=0 would otherwise expose the parent HWND brush past where
  // the WebView shadow has faded.
  useViewportOcclusion(bridge, occlusionId, ref, 24, 24);
  return (
    <Menubar.Content {...rest}>
      <div ref={ref}>{children}</div>
    </Menubar.Content>
  );
}

// SubContent analogue. Menubar.SubContent renders in its OWN portal
// when the SubMenu opens (e.g. File → Recent Files); without an
// occlusion registration the engine viewport renders over it and the
// user sees only the drop shadow leaking through. Pattern + 24 px
// pad/feather match OccludingMenubarContent verbatim.
type MenuSubContentProps = ComponentProps<typeof Menubar.SubContent> & {
  bridge: Bridge;
  occlusionId: string;
};
function OccludingMenubarSubContent({
  bridge,
  occlusionId,
  children,
  ...rest
}: MenuSubContentProps) {
  const ref = useRef<HTMLDivElement | null>(null);
  useViewportOcclusion(bridge, occlusionId, ref, 24, 24);
  return (
    <Menubar.SubContent {...rest}>
      <div ref={ref}>{children}</div>
    </Menubar.SubContent>
  );
}

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
  "px-2 py-1 text-xs font-medium text-text-2 hover:bg-bg-2 rounded data-[state=open]:bg-bg-2 data-[state=open]:text-text outline-none select-none cursor-default";
// restores the drop shadow. The layered viewport now stamps a
// smoothstep-feathered alpha hole at each occlusion rect (with
//'s edge padding sized to enclose the shadow), so shadow-xl
// blends naturally against the D3D9 scene instead of leaving the
// dark halo the prior HRGN cut produced.
const CONTENT =
  "min-w-[200px] bg-bg-2 border border-border rounded-md shadow-xl p-1 z-50";
const ITEM =
  "flex items-center gap-2 px-2 py-1 text-xs text-text rounded hover:bg-panel-2 focus:bg-panel-2 outline-none cursor-pointer data-[disabled]:text-text-3 data-[disabled]:cursor-not-allowed data-[disabled]:hover:bg-transparent select-none";
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
function basename(path: string): string {
  const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return idx >= 0 ? path.slice(idx + 1) : path;
}

export function MenuBar({
  bridge,
  onOpenImportEmittersDialog,
  onOpenAboutDialog,
  onOpenRescaleDialog,
  onResetPanelLayout,
}: Props) {
  const [state, setState] = useState<EngineStateDto | null>(null);
  // Group D: View → Reset View Settings prompt visibility.
  const [resetViewOpen, setResetViewOpen] = useState(false);

  // D6: list of discovered mods, fetched separately from the
  // engine snapshot because it has a much lower change cadence (only
  // shifts on Refresh or disk mutation). The *active* mod is on the
  // snapshot so the menu's check mark stays reactive without a second
  // round-trip after a select.
  const [mods, setMods] = useState<ModDescriptor[]>([]);
  const refreshModsList = async () => {
    try {
      const r = await bridge.request({ kind: "mods/list", params: {} });
      // Defensive: a partial / mocked response that omits `mods`
      // shouldn't crash the menu's filter step. Fall back to [].
      setMods(Array.isArray(r?.mods) ? r.mods : []);
    } catch (err) {
      console.warn("[MenuBar] mods/list failed:", err);
    }
  };
  const handleModSelect = (path: string | null) => {
    void bridge.request({ kind: "mods/select", params: { path } });
  };
  const handleModRefresh = async () => {
    try {
      const r = await bridge.request({ kind: "mods/refresh", params: {} });
      setMods(Array.isArray(r?.mods) ? r.mods : []);
    } catch (err) {
      console.warn("[MenuBar] mods/refresh failed:", err);
    }
  };
  const handleResetViewConfirm = async () => {
    setResetViewOpen(false);
    await bridge.request({
      kind: "engine/action/reset-view-settings",
      params: {},
    });
  };

  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (!cancelled) setState(s);
      })
      .catch((err) => console.warn("[MenuBar] snapshot failed:", err));
    const off = bridge.on("engine/state/changed", (e) => setState(e.payload));
    // D6: prime the mods list at mount. Active mod arrives via
    // snapshot; the list is a separate channel because it changes
    // rarely.
    void refreshModsList();
    return () => {
      cancelled = true;
      off();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [bridge]);

  const ground = state?.ground ?? false;
  // Bloom enable/disable lives on the toolbar's "Toggle bloom" button, not
  // the View menu (session 11 follow-up), so no bloom state is read here.
  const paused = state?.paused ?? false;
  const heatDebug = state?.heatDebug ?? false;

  // Primary selection drives the Emitters-menu item enabled state.
  // Rename / Rescale / Add Child operate on the primary; Add Root is
  // selection-independent.
  const primaryEmitterId = useEmitterSelectionPrimary();
  const hasPrimary = primaryEmitterId !== null;

  // Screen 8 Batch 3: File-menu wiring needs the recent-files list +
  // the prompt-save-changes helper.
  const { recentFiles } = useFileState();

  // Edit-menu clipboard + delete () act on the current emitter
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
      await bridge.request({ kind: "file/open", params: {} });
    });
  };

  const handleSave = () => {
    void bridge.request({ kind: "file/save", params: {} });
  };

  const handleSaveAs = () => {
    void bridge.request({ kind: "file/save-as", params: {} });
  };

  const handleOpenRecent = (path: string) => {
    promptSaveChanges(async () => {
      await bridge.request({ kind: "file/open", params: { path } });
    });
  };

  const handleExit = () => {
    // Group D: route through promptSaveChanges so a dirty
    // particle system gets the Save/Discard/Cancel prompt before
    // the host tears down. app/quit posts WM_CLOSE on the host
    // side; the existing WM_DESTROY chain handles compositor +
    // engine cleanup. Cancel from the prompt is a silent no-op,
    // matching legacy DoCheckChanges semantics.
    promptSaveChanges(async () => {
      await bridge.request({ kind: "app/quit", params: {} });
    });
  };

  // ── Emitters menu handlers () ─────────────────────────────────

  const handleAddRoot = () => {
    void bridge.request({ kind: "emitters/add-root", params: {} });
  };

  const handleAddLifetimeChild = () => {
    if (primaryEmitterId === null) return;
    void bridge.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: primaryEmitterId },
    });
  };

  const handleAddDeathChild = () => {
    if (primaryEmitterId === null) return;
    void bridge.request({
      kind: "emitters/add-death-child",
      params: { parentId: primaryEmitterId },
    });
  };

  const handleRenameEmitter = () => {
    if (primaryEmitterId === null) return;
    requestEmitterRename(primaryEmitterId);
  };

  const handleRescaleEmitter = () => {
    if (primaryEmitterId === null) return;
    useTreeContextStore.getState().openDialog("rescale", primaryEmitterId);
  };

  // ── Edit-menu clipboard / delete () ─────────────────────────
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
    void bridge.request({ kind: "emitters/cut", params: { ids } });
    markEmittersCopied();
  };
  const handlePaste = () => {
    void bridge.request({ kind: "emitters/paste", params: {} });
  };
  const handleDeleteSelection = () => {
    const ids = getEmitterSelectionSnapshot().ids;
    if (ids.length === 0) return;
    // Descending id order — matches the tree's batch delete + the host
    // contract (deleting ascending would invalidate higher ids mid-loop).
    for (const id of [...ids].sort((a, b) => b - a)) {
      void bridge.request({ kind: "emitters/delete", params: { id } });
    }
  };

  // ── Emitters-menu visibility () ─────────────────────────────
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
    <Menubar.Root className="flex items-center gap-0.5">
      {/* ─── File ─── */}
      <Menubar.Menu>
        <Menubar.Trigger className={TRIGGER}>File</Menubar.Trigger>
        <Menubar.Portal>
          <OccludingMenubarContent
            bridge={bridge}
            occlusionId="menu:file"
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
                <OccludingMenubarSubContent
                  bridge={bridge}
                  occlusionId="menu:file:recent"
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
                        title={path}
                        onSelect={() => handleOpenRecent(path)}
                      >
                        {basename(path)}
                      </Menubar.Item>
                    ))
                  )}
                </OccludingMenubarSubContent>
              </Menubar.Portal>
            </Menubar.Sub>
            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item className={ITEM} onSelect={handleExit}>
              Exit<Hint>Alt+F4</Hint>
            </Menubar.Item>
          </OccludingMenubarContent>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── Edit ─── */}
      <Menubar.Menu>
        <Menubar.Trigger className={TRIGGER}>Edit</Menubar.Trigger>
        <Menubar.Portal>
          <OccludingMenubarContent
            bridge={bridge}
            occlusionId="menu:edit"
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            <Menubar.Item
              className={ITEM}
              disabled={!state?.canUndo}
              onSelect={send({
                kind: "undo/perform",
                params: { direction: "undo" },
              })}
            >
              Undo<Hint>Ctrl+Z</Hint>
            </Menubar.Item>
            <Menubar.Item
              className={ITEM}
              disabled={!state?.canRedo}
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
          </OccludingMenubarContent>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── Emitters () ─── */}
      <Menubar.Menu>
        <Menubar.Trigger className={TRIGGER}>Emitters</Menubar.Trigger>
        <Menubar.Portal>
          <OccludingMenubarContent
            bridge={bridge}
            occlusionId="menu:emitters"
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
            {/*: Toggle Visibility acts on the primary selection (reads
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
          </OccludingMenubarContent>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── Mods (D6: dynamic detected-mod list) ─── */}
      <Menubar.Menu>
        <Menubar.Trigger className={TRIGGER}>Mods</Menubar.Trigger>
        <Menubar.Portal>
          <OccludingMenubarContent
            bridge={bridge}
            occlusionId="menu:mods"
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            {/* Unmodded is always first; selected when activeModPath is null. */}
            <Menubar.Item
              className={ITEM}
              onSelect={() => handleModSelect(null)}
            >
              <CheckSlot active={state?.activeModPath == null} />
              <span>Unmodded</span>
            </Menubar.Item>

            {(() => {
              // Group mods by isFoC (FoC first, Base Game second), each
              // group already alphabetised by the host. Render with a
              // separator before each non-empty group. Display label
              // prefers nickname when set, else folder name — matches
              // the legacy owner-drawn entry's "folderName (nickname)"
              // shape, minus the parenthetical (the Radix menu doesn't
              // do owner-draw, so we collapse to a single label).
              const fyi = mods.filter((m) => m.isFoC);
              const baseGame = mods.filter((m) => !m.isFoC);
              const groups: Array<{ label: string; items: ModDescriptor[] }> = [];
              if (fyi.length > 0) groups.push({ label: "Forces of Corruption", items: fyi });
              if (baseGame.length > 0) groups.push({ label: "Base Game", items: baseGame });
              return groups.map((g) => (
                <div key={g.label}>
                  <Menubar.Separator className={SEPARATOR} />
                  {/* Group header — a disabled item rendered subtly. */}
                  <div className="px-2 py-1 text-[10px] uppercase tracking-wide text-text-3 select-none">
                    {g.label}
                  </div>
                  {g.items.map((m) => {
                    const label = m.nickname || m.folderName;
                    return (
                      <Menubar.Item
                        key={m.path}
                        className={ITEM}
                        onSelect={() => handleModSelect(m.path)}
                      >
                        <CheckSlot active={state?.activeModPath === m.path} />
                        <span>{label}</span>
                      </Menubar.Item>
                    );
                  })}
                </div>
              ));
            })()}

            <Menubar.Separator className={SEPARATOR} />
            <Menubar.Item
              className={ITEM}
              onSelect={() => {
                void handleModRefresh();
              }}
            >
              <CheckSlot active={false} />
              <span>Refresh Mod List</span>
            </Menubar.Item>
          </OccludingMenubarContent>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── View ─── */}
      <Menubar.Menu>
        <Menubar.Trigger className={TRIGGER}>View</Menubar.Trigger>
        <Menubar.Portal>
          <OccludingMenubarContent
            bridge={bridge}
            occlusionId="menu:view"
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
            {/* Lighting opens the docked right-dock pane (shared with the
                Spawner; session 11). Bloom is fully handled elsewhere:
                its settings live as a section inside the Lighting pane, and
                its on/off toggle is the toolbar's "Toggle bloom" button — so
                the former View-menu "Bloom" + "Bloom Settings…" items were
                both retired (session 11 follow-up). */}
            <Menubar.Item
              className={ITEM}
              onSelect={() => toggleDock("lighting")}
            >
              <CheckSlot active={false} />
              Lighting…
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
            {/* Group D: dispatches engine/set/camera with the legacy
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
              onSelect={send({
                kind: "engine/action/reload-textures",
                params: {},
              })}
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
            {/* Group D: pop the confirm modal; the modal's
                Reset button fires engine/action/reset-view-settings
                which cascades background / ground / bloom / skydome
                back to defaults in one host-side action. Lighting
                reset rides separately with D4. */}
            <Menubar.Item
              className={ITEM}
              onSelect={() => setResetViewOpen(true)}
            >
              <CheckSlot active={false} />
              Reset View Settings
            </Menubar.Item>
            <Menubar.Separator className={SEPARATOR} />
            {/* B1.4 T6: clears the four alo:layout:* localStorage keys
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
          </OccludingMenubarContent>
        </Menubar.Portal>
      </Menubar.Menu>

      {/* ─── Help ─── */}
      <Menubar.Menu>
        <Menubar.Trigger className={TRIGGER}>Help</Menubar.Trigger>
        <Menubar.Portal>
          <OccludingMenubarContent
            bridge={bridge}
            occlusionId="menu:help"
            className={CONTENT}
            align="start"
            sideOffset={4}
          >
            <Menubar.Item className={ITEM} onSelect={() => onOpenAboutDialog()}>
              About
            </Menubar.Item>
          </OccludingMenubarContent>
        </Menubar.Portal>
      </Menubar.Menu>
    </Menubar.Root>

    {/* Group D: confirm prompt for View → Reset View Settings.
        Body copy mirrors the legacy MessageBox at main.cpp:1734.
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
        <button
          type="button"
          onClick={() => setResetViewOpen(false)}
          className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 outline-none focus:border-accent"
        >
          Cancel
        </button>
        <button
          type="button"
          onClick={() => void handleResetViewConfirm()}
          className="rounded bg-accent px-3 py-1 text-xs font-medium text-white hover:bg-accent outline-none focus:ring-2 focus:ring-accent"
        >
          Reset
        </button>
      </Modal.Footer>
    </Modal>
    </>
  );
}
