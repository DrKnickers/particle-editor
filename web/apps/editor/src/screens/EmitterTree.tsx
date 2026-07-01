// EmitterTree — sidebar tree of the live ParticleSystem's emitters.
//
// Capabilities:
//   - read-only render + single-select.
//   - right-click context menu + modal dialogs for Rename/Duplicate/
//     Delete/Increment/Rescale/LinkGroupSettings.
//   - Add Lifetime/Death Child, Move Up/Down, Set Link Group… /
//     Leave Link Group, plus React-side multi-select (Ctrl/Cmd + Shift
//     + plain click).
//   - HTML5 drag/drop reorder + reparent.
//   - Link-group bracket gutter + inline rename
//     (F2 / dbl-click / context-menu Rename; replaces the rename modal —
//     `RenameEmitterDialog` is deleted) + keyboard nav (arrows / Home
//     / End / Enter / F2 / Delete / Ctrl+C/X/V).
//
// Multi-select model: server tracks only the primary id (via the
// existing `emitters/select`); React layers an in-memory `ids[]` +
// `primary` atom on top (see `lib/emitter-selection.ts`). Plain click
// = setSingle + bridge select. Ctrl/Cmd+click = toggle. Shift+click =
// range from primary to clicked along rendered tree order. Right-click
// on a row not in the multi-selection promotes that row to single-
// select before opening the menu so the batch operations operate on
// the row the user actually targeted.
//
// Role glyphs (single-character lucide-free alternatives so we don't
// have to negotiate icon-set additions): "root" is a filled disc "●",
// "lifetime" is the cyclic-arrow "↻" (continuous spawn during parent's
// lifetime), "death" is "✕" (one-shot spawn when parent dies). Greyed
// when `visible === false`.
//
// Link-group dot: a small filled circle in `bg-accent` when
// `linkGroup !== 0`. The full coloured-bracket visualization
// renders in the right gutter; the dot itself stays
// as a per-row affordance for quick "this row is linked" recognition.
//
// Inline rename: a string-keyed Zustand atom would be overkill — local
// component state suffices because (a) only the tree owns the input
// HWND, (b) only the tree binds the keyboard handlers that drive the
// transitions. `editing: { id, value } | null`. Triggers: F2 on focused
// row, double-click on row label, or context-menu Rename. Commit on
// Enter / blur / click-outside via `emitters/rename`; cancel on Esc.
// Empty value reverts to the original (no commit).
//
// Keyboard nav: the tree's outer `<div>` carries `tabIndex={0}` so the
// container itself can receive focus, but each row is already a focus-
// able `<button>` — arrows shift focus row-by-row in flat order. The
// handler is attached to the tree container; it doesn't intercept
// keystrokes when the focus target is an `<input>` (so inline rename
// + downstream text fields stay usable).
//
// Clipboard: Ctrl+C / Ctrl+X / Ctrl+V on the focused tree dispatch
// `emitters/copy` / `emitters/cut` / `emitters/paste` against the
// current multi-selection. The C++ host owns the buffer; React just
// fires the bridge call. Paste appends new roots at the end (or after
// `afterId` — not surfaced through the keyboard path; only the future
// "Paste below selection" menu would supply it).

import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  Fragment,
  useState,
  type ComponentProps,
} from "react";
import * as ContextMenu from "@radix-ui/react-context-menu";
import * as Menubar from "@radix-ui/react-menubar";
import { ChevronDown, ChevronUp, Copy, Eye, EyeOff, Plus, Trash2, TriangleAlert } from "lucide-react";
import type {
  Bridge,
  EmitterTreeDto,
  EmitterTreeNode,
} from "@particle-editor/bridge-schema";
import { openTreeContextDialog } from "@/lib/tree-context";
import { useTreeActionStore } from "@/lib/tree-action";
import {
  useEmitterSelectionIds,
  useEmitterSelectionPrimary,
  useEmitterSelectionStore,
} from "@/lib/emitter-selection";
import { markEmittersCopied, useEmitterClipboardHasContent } from "@/lib/emitter-clipboard";
import { rectFromPoints, emittersInMarquee, mergeMarqueeSelection } from "@/lib/marquee";
import { computeAutoscrollDelta } from "@/lib/drag-autoscroll";
import { computeFlipDeltas, pickFlipDuration, type FlipPositions } from "@/lib/flip";
import { useRecording } from "@/lib/record-mode";
import {
  computeRootGapIndex,
  isDescendant,
  resolveReparentSlot,
  type DropZone,
} from "@/lib/drop-zone";
import { colorForGroup } from "@/lib/link-group-colors";
import { estimateChainLoad, estimateSystemLoad, formatChainWarning, type ChainWarning } from "@/lib/chain-load";
import { useOverloadGuardConfig } from "@/lib/overload-guard";
import { useEstimatedLoadPush } from "@/lib/use-estimated-load-push";
import { SystemLoadChip } from "@/components/SystemLoadChip";
import { Tip } from "@/primitives/Tip";
import { ChainWarningTip } from "./ChainWarningTip";
import { useEmitterTreeStore } from "@/lib/emitter-tree";
import { requestDeleteEmitters } from "@/lib/delete-emitters";
import { moveEmitters, duplicateEmitters, reorderManyEmitters } from "@/lib/emitter-reorder";
import {
  isMultiDrag,
  selectedRootIdsInOrder,
  collectSubtreeIds,
  resolveGapFromGeometry,
  resolveSingleRootDrop,
  gapContentY,
  liftedBlockHeight,
  computeChipTarget,
  type RootBlockGeometry,
  type RowGeometry,
} from "@/lib/multi-drag";
import { canMoveSelection } from "@/lib/move-enabled";

type Props = {
  bridge: Bridge;
};

/** Map role → display glyph. Pure presentational — no role-specific
 *  behaviour wiring this batch. */
function roleGlyph(role: EmitterTreeNode["role"]): string {
  switch (role) {
    case "root":     return "●";
    case "lifetime": return "↻";
    case "death":    return "✕";
  }
}

/** Aria label for the role glyph; assistive tech reads this. */
function roleLabel(role: EmitterTreeNode["role"]): string {
  switch (role) {
    case "root":     return "root emitter";
    case "lifetime": return "lifetime child";
    case "death":    return "death child";
  }
}

/** Flatten the rendered tree into a depth-first list of `(node, depth,
 *  siblings, indexInSiblings)`. Used by both rendering and the shift-
 *  click range computation. */
type FlatRow = {
  node: EmitterTreeNode;
  depth: number;
  siblings: EmitterTreeNode[];
  indexInSiblings: number;
};

function flattenTree(tree: EmitterTreeDto | null): FlatRow[] {
  if (tree === null) return [];
  const rows: FlatRow[] = [];
  const walk = (
    siblings: EmitterTreeNode[],
    depth: number,
  ) => {
    siblings.forEach((node, idx) => {
      rows.push({ node, depth, siblings, indexInSiblings: idx });
      walk(node.children, depth + 1);
    });
  };
  walk(tree.root.children, 0);
  return rows;
}

// Drop indicator state. Owned at the EmitterTree level so only one row
// at a time displays a visual indicator. `targetId` is the row currently
// being hovered. `null` means no active drag-over. Both single and multi
// drags resolve geometrically (no live-DOM hovered-row semantics):
//   - "gap":  a make-room spacer at a resolved root gap (reorder — single
//             root or multi block); carries the lifted block's measured height.
//   - "onto": a reparent ring on a target row (single-drag only — drop onto
//             the middle third of a row to nest under it).
type DropIndicator =
  | { kind: "gap"; gapIndex: number; gapHeight: number }
  | { kind: "onto"; targetId: number }
  | null;

// [multi-drag] Chip-magnetize tuning: how far the chip's Y leans toward the
// active gap's center (0 = stays at the pointer, 1 = docks onto the gap), and
// the per-frame spring factor for the glide between targets.
const CHIP_PULL = 0.6;
const CHIP_SPRING = 0.25;

// [glide] FLIP durations: mid-drag reflows track the pointer, so they run
// snappier than the post-drop settle.
const FLIP_DRAG_MS = 120;
const FLIP_SETTLE_MS = 200;
// --record only, active-drag glide (see pickFlipDuration in lib/flip.ts): the FLIP
// glide is a wall-clock CSS transition, but the capture loop grabs each frame only
// after a slow paint+PrintWindow (tens of ms of REAL time per frame). At
// FLIP_DRAG_MS the transition finishes between captures, so the reorder is never
// caught mid-glide and the rows snap in the recorded clip. This value is
// EMPIRICALLY TUNED to the capture cadence (≈800ms spans ~8 captured frames) — not
// an animation-feel choice; re-verify against a --record clip before changing it.
const FLIP_RECORD_MS = 800;

// [glide] Chip despawn: on release the chip flies into the landing gap (or
// the reparent target row) while fading; cancels/no-ops fade in place.
const CHIP_EXIT_MS = 160;

// Validated parameters for the `emitters/drop` bridge call — the output
// of resolveDropIntent. `null` means the drop is refused.
type DropParams =
  | { mode: "reparent"; id: number; targetId: number; slot: "lifetime" | "death" }
  | { mode: "reorder"; id: number; rootIndex: number };

/** [multi-drag] Measure every root block's extent (root row + whole subtree)
 *  in scroll-CONTENT space at drag activation. Measured, never assumed — row
 *  height varies with density and a block's height with its subtree. Returns
 *  null if any row element is missing (defensive: the drag then shows no gap
 *  and a release is a no-op). */
function captureRootBlockGeometry(
  sc: HTMLElement | null,
  roots: EmitterTreeNode[],
): RootBlockGeometry | null {
  if (sc === null) return null;
  // content Y of a rect = rect.top - scroll viewport top + scrollTop
  const scTop = sc.getBoundingClientRect().top - sc.scrollTop;
  const tops: number[] = [];
  const bottoms: number[] = [];
  for (const r of roots) {
    const ids = collectSubtreeIds(r);
    const firstEl = sc.querySelector(`button[data-emitter-id="${ids[0]}"]`);
    const lastEl = sc.querySelector(`button[data-emitter-id="${ids[ids.length - 1]}"]`);
    if (firstEl === null || lastEl === null) return null;
    tops.push(firstEl.getBoundingClientRect().top - scTop);
    bottoms.push(lastEl.getBoundingClientRect().bottom - scTop);
  }
  return { tops, bottoms };
}

/** [single-drag] Measure EVERY row's extent in scroll-CONTENT space at drag
 *  activation, flat (rendered) order. The single-drag resolver hit-tests the
 *  hovered row (for reparent-onto detection) against this snapshot instead of
 *  the live DOM, so a reflowing make-room gap can't corrupt the hit-test.
 *  Returns null if any row element is missing. */
function captureRowGeometry(
  sc: HTMLElement | null,
  rows: FlatRow[],
): RowGeometry | null {
  if (sc === null) return null;
  const scTop = sc.getBoundingClientRect().top - sc.scrollTop;
  const ids: number[] = [];
  const tops: number[] = [];
  const bottoms: number[] = [];
  for (const r of rows) {
    const el = sc.querySelector(`button[data-emitter-id="${r.node.id}"]`);
    if (el === null) return null;
    const rect = el.getBoundingClientRect();
    ids.push(r.node.id);
    tops.push(rect.top - scTop);
    bottoms.push(rect.bottom - scTop);
  }
  return { ids, tops, bottoms };
}

/** Find the parent node of `id` in the tree (null for a root / not found). */
function findParentNode(
  root: EmitterTreeNode,
  id: number,
): EmitterTreeNode | null {
  for (const c of root.children) {
    if (c.id === id) return root;
    const hit = findParentNode(c, id);
    if (hit) return hit;
  }
  return null;
}

/** Pure drop-intent resolution, shared by the pointer-drag controller.
 *  Returns the validated `emitters/drop` params, or null when the drop is
 *  refused:
 *    - self-drop / cycle (target inside source's subtree) → refused
 *    - middle third ("onto") → reparent under target (auto-pick slot;
 *      refuse if both child slots are full or target is already the
 *      source's parent)
 *    - upper/lower third → reorder, only when BOTH source and target are
 *      roots (gap semantics apply to the root list).
 *  This was an inline `resolveDropIntent`; lifted to a pure fn so
 *  the pointer-drag controller (which replaced HTML5 DnD — dead under
 *  composition hosting) can call it for any hovered target row. */
function resolveDropIntent(
  source: EmitterTreeNode,
  target: EmitterTreeNode,
  targetRootIdx: number,
  zone: DropZone,
  tree: EmitterTreeDto | null,
  rootChildren: EmitterTreeNode[],
): DropParams | null {
  if (source.id === target.id) return null;
  if (isDescendant(source, target.id)) return null;
  if (zone === "onto") {
    const slot = resolveReparentSlot(target);
    if (slot === null) return null;
    if (tree !== null) {
      const parent = findParentNode(tree.root, source.id);
      if (parent !== null && parent.id === target.id) return null;
    }
    return { mode: "reparent", id: source.id, targetId: target.id, slot };
  }
  const sourceIsRoot = rootChildren.some((c) => c.id === source.id);
  if (!sourceIsRoot || target.role !== "root" || targetRootIdx === -1) return null;
  return {
    mode: "reorder",
    id: source.id,
    rootIndex: computeRootGapIndex(targetRootIdx, zone),
  };
}

// Inline-rename state. `editing.id` is the row currently in
// rename mode; `editing.value` is the live input value. The original
// name is captured at edit-start (`original`) so an empty-commit can
// revert without a tree re-fetch round-trip.
export type RenameEditingState = {
  id: number;
  value: string;
  original: string;
} | null;

type RowProps = {
  row: FlatRow;
  primaryId: number | null;
  selectedIds: number[];
  orderedIds: number[];
  onRowClick: (id: number, mods: { ctrlKey: boolean; metaKey: boolean; shiftKey: boolean }) => void;
  bridge: Bridge;
  // [pointer-drag] wiring from the parent. The parent owns the drag
  // controller (startDrag); the row just initiates on pointerdown and
  // reads draggingId / indicator for its visual state.
  draggingId: number | null;
  // Ids of every row in the lifted multi-drag block (all dim while dragging).
  draggingIds: number[];
  indicator: DropIndicator;
  startDrag: (node: EmitterTreeNode, e: React.PointerEvent) => void;
  // Inline rename. `editing.id === node.id` means this row
  // renders an `<input>` instead of the label span. `beginEdit` starts
  // a new rename session against this row; `setEditValue` updates the
  // live value; `commitEdit` / `cancelEdit` end the session.
  editing: RenameEditingState;
  beginEdit: (id: number, currentName: string) => void;
  setEditValue: (value: string) => void;
  commitEdit: () => void;
  cancelEdit: () => void;
  // True when this row's link group is the one currently hovered —
  // the row paints a subtle tint so the user sees the whole group light up
  // together. `onHoverLinkGroup` reports this row's group on pointer enter
  // (null on leave) so the parent can drive the tint + bracket highlight.
  linkHover: boolean;
  onHoverLinkGroup: (groupId: number | null) => void;
  // Dissolve the entire link group this row belongs to.
  onDissolveLinkGroup: (groupId: number) => void;
  // Badge / spine click: select every member of this row's link group.
  onSelectLinkGroup: (groupId: number) => void;
  // non-null when this row sits on a chain whose estimated alive
  // count crosses the warning threshold (guard cap, or the 10k advisory).
  chainWarning: ChainWarning | null;
};

// Styled ContextMenu.Content for the emitter-tree row context menu --
// shared border/shadow chrome matching the menubar dropdowns.
function StyledContextMenuContent({ children, ...rest }: ComponentProps<typeof ContextMenu.Content>) {
  return (
    <ContextMenu.Content
      className="z-50 min-w-[220px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-[var(--shadow-soft)]"
      {...rest}
    >
      {children}
    </ContextMenu.Content>
  );
}

// Styled ContextMenu.SubContent (e.g. the "Paste As" submenu), same chrome.
function StyledContextSubContent({ children, ...rest }: ComponentProps<typeof ContextMenu.SubContent>) {
  return (
    <ContextMenu.SubContent
      className="z-50 min-w-[200px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-[var(--shadow-soft)]"
      {...rest}
    >
      {children}
    </ContextMenu.SubContent>
  );
}

function EmitterRow({
  row, primaryId, selectedIds, orderedIds, onRowClick, bridge,
  draggingId, draggingIds, indicator, startDrag,
  editing, beginEdit, setEditValue, commitEdit, cancelEdit,
  linkHover, onHoverLinkGroup, onDissolveLinkGroup, onSelectLinkGroup, chainWarning,
}: RowProps) {
  const { node, depth, siblings } = row;
  const isPrimary = primaryId === node.id;
  const isSelected = selectedIds.includes(node.id);
  const isLinked = node.linkGroup !== 0;
  // Group hue (null when unlinked). `linkColor` is the raw colour used by
  // the always-on badge; `spineTintColor` is the same hue gated to the
  // spine + row-tint case (linked AND unselected — selection styling wins
  // otherwise). Computed once so the style object below doesn't re-call
  // colorForGroup several times per render.
  const linkColor = colorForGroup(node.linkGroup);
  const spineTintColor = isLinked && !isSelected ? linkColor : null;
  const isEditing = editing !== null && editing.id === node.id;
  // Context-menu Paste gates on session clipboard content.
  const hasClipboard = useEmitterClipboardHasContent();
  const inputRef = useRef<HTMLInputElement | null>(null);

  // Auto-focus + select-all on the input the moment editing toggles to
  // this row. The ref binds in the same render where `isEditing` flips
  // to true; the effect fires post-mount.
  useEffect(() => {
    if (isEditing && inputRef.current !== null) {
      inputRef.current.focus();
      inputRef.current.select();
    }
  }, [isEditing]);

  // Disabled states (derived from the tree DTO + the multi-selection).
  // The DTO doesn't expose `spawnDuringLife` / `spawnOnDeath` directly,
  // but children-by-role is equivalent: the slot is filled iff there's
  // a child of that role.
  const hasLifetimeChild = node.children.some((c) => c.role === "lifetime");
  const hasDeathChild    = node.children.some((c) => c.role === "death");
  // Move is a root-only operation. The engine refuses non-root moves
  // (children of the same role can't be swapped — at most one of each).
  const isRoot = node.role === "root";
  // The context-menu Move targets the whole selection when this row is part of
  // it (else just this row — mirrors resolveTargetIds), so its enabled state
  // uses the same preserve rule as move-many over that target set.
  const moveTargetIds = isRoot && selectedIds.includes(node.id) ? selectedIds : [node.id];
  const rootIdsInOrder = siblings.map((s) => s.id);
  const canMoveUp   = isRoot && canMoveSelection(moveTargetIds, rootIdsInOrder, "up");
  const canMoveDown = isRoot && canMoveSelection(moveTargetIds, rootIdsInOrder, "down");

  // Leave Link Group: enabled when at least one of the currently
  // selected emitters has `linkGroup !== 0`. If the right-clicked row
  // isn't in the selection, fall back to the row's own linkGroup.
  // (The handler also promotes a non-selected right-clicked row to
  // single-select before the click reaches this menu state.)
  const selectionLinkGroups = useMemo(() => {
    return selectedIds.length > 0 ? selectedIds : [node.id];
  }, [selectedIds, node.id]);
  const leaveLinkGroupDisabled = useMemo(() => {
    // Need the tree to inspect linkGroup; rebuild a fast lookup.
    const idToLinkGroup = new Map<number, number>();
    const visit = (n: EmitterTreeNode) => {
      idToLinkGroup.set(n.id, n.linkGroup);
      n.children.forEach(visit);
    };
    // We only have the row itself + the row's own subtree here. The
    // ordered-ids list is the in-order walk; we leverage *that* with
    // a child-look-up via the closures the parent passed. To keep
    // memory churn tight, derive the answer from the row + the
    // multi-selection by treating "we don't know the rest" as the
    // most permissive case (enabled). The Leave-LG bridge call is
    // idempotent on linkGroup=0, so the worst case is a redundant
    // round-trip on a no-op, not a wrong-result mutation.
    if (selectionLinkGroups.length === 1) {
      if (selectionLinkGroups[0] === node.id) return node.linkGroup === 0;
    }
    // Walk the row's own subtree as best-effort fallback.
    visit(node);
    const lookup = (id: number): number | undefined => idToLinkGroup.get(id);
    return selectionLinkGroups.every((id) => (lookup(id) ?? 0) === 0);
  }, [selectionLinkGroups, node]);

  // Indent by 12px per depth level.
  const indentPx = depth * 12;

  // Context-menu handlers ────────────────────────────────────────────
  //
  // Each handler promotes the right-clicked row to single-select when
  // it isn't already in the selection. Without that step, "Set Link
  // Group…" would fire on whatever the previous selection was, which
  // is surprising. The promotion routes through the bridge so the
  // server's primary id and React's primary stay in lock-step.

  /** Snapshot the current selection at handler-execution time, falling
   *  back to a single-select of `node.id` when nothing is selected or
   *  when the row isn't already in the selection. */
  const resolveTargetIds = (): number[] => {
    const cur = useEmitterSelectionStore.getState().ids;
    if (cur.includes(node.id) && cur.length > 0) return [...cur];
    // Promote.
    useEmitterSelectionStore.getState().setSingle(node.id);
    void bridge.request({ kind: "emitters/select", params: { id: node.id } });
    return [node.id];
  };

  const handleRename = () => {
    // Context-menu Rename starts inline edit instead of
    // opening a modal. `RenameEmitterDialog` has been removed.
    resolveTargetIds();
    beginEdit(node.id, node.name);
  };
  const handleDuplicate = () => {
    // Duplicate the resolved target set (whole selection if the clicked row
    // is in it, else just that row); the selection moves to the new copies.
    void duplicateEmitters(bridge, resolveTargetIds());
  };
  const handleDelete = () => {
    // Delete the resolved target set — the whole selection when the
    // right-clicked row is part of it, else just the clicked row
    // (resolveTargetIds promotes a non-selected row to a single select).
    // Previously this discarded the return and hardcoded [node.id], so
    // right-click → Delete on a multi-selection deleted only one row and
    // skipped the destructive-confirm.
    requestDeleteEmitters(bridge, resolveTargetIds());
  };
  const handleIncrement = () => {
    resolveTargetIds();
    openTreeContextDialog("increment", node.id);
  };
  const handleRescale = () => {
    resolveTargetIds();
    openTreeContextDialog("rescale", node.id);
  };
  // Context-menu clipboard + New Root — reuse the same
  // bridge calls as the tree's Ctrl+C/X/V so behaviour stays identical.
  const handleNewRoot = () => {
    void bridge.request({ kind: "emitters/add-root", params: {} });
  };
  const handleContextCopy = () => {
    const ids = resolveTargetIds();
    void bridge.request({ kind: "emitters/copy", params: { ids } });
    markEmittersCopied();
  };
  const handleContextCut = () => {
    const ids = resolveTargetIds();
    void bridge.request({ kind: "emitters/cut", params: { ids } });
    markEmittersCopied();
  };
  const handleContextPaste = () => {
    void bridge.request({ kind: "emitters/paste", params: {} });
  };
  // Paste As ▸ Lifetime/Death Child — paste the clipboard into this
  // emitter's child slot (legacy ID_PASTEAS_LIFETIME / ID_PASTEAS_DEATH).
  const handlePasteAsLifetime = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/paste-as-child",
      params: { parentId: node.id, slot: "lifetime" },
    });
  };
  const handlePasteAsDeath = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/paste-as-child",
      params: { parentId: node.id, slot: "death" },
    });
  };
  const handleLinkGroupSettings = () => {
    resolveTargetIds();
    openTreeContextDialog("link-group", node.id, node.linkGroup);
  };
  const handleAddLifetimeChild = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: node.id },
    });
  };
  const handleAddDeathChild = () => {
    resolveTargetIds();
    void bridge.request({
      kind: "emitters/add-death-child",
      params: { parentId: node.id },
    });
  };
  const handleMoveUp = () => {
    void moveEmitters(bridge, resolveTargetIds(), "up");
  };
  const handleMoveDown = () => {
    void moveEmitters(bridge, resolveTargetIds(), "down");
  };
  const handleSetLinkGroup = () => {
    resolveTargetIds();
    openTreeContextDialog("set-link-group", node.id);
  };
  const handleLeaveLinkGroup = () => {
    const ids = resolveTargetIds();
    void bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids, groupId: null },
    });
  };

  // Reference orderedIds so eslint-no-unused-vars in the trimmed file
  // doesn't complain when the only consumer is the parent's click
  // handler. Passing it through props keeps the row symmetric with the
  // tree-flatten output.
  void orderedIds;

  // ── drag/drop handlers ────────────────────────────────
  //
  // The row is both a drag source and a drop target. Drop semantics:
  //   - drop above/below a root → reorder (a make-room gap renders at the
  //     resolved root gap; see the parent's flatRows map)
  //   - drop on any row, middle third → reparent under target (this row's
  //     onto-ring, below)
  // The drag is driven by the parent's pointer-drag controller (startDrag,
  // wired to this row's button below); the row only renders the onto-ring
  // from `indicator` and initiates on pointerdown.

  // Dimmed while lifted: the grabbed row OR any row in the lifted block
  // (selected roots + all their descendants).
  const isDragging = draggingId === node.id || draggingIds.includes(node.id);
  // Reparent target visual: tint the row + ring when this row is the
  // single-drag "onto" target. (Reorder uses the make-room gap, not a ring.)
  const reparentTintClass =
    indicator?.kind === "onto" && indicator.targetId === node.id
      ? "bg-accent-soft ring-1 ring-accent"
      : "";

  const menuItemClass =
    "flex cursor-pointer items-center rounded px-2 py-1 text-xs text-text outline-none data-[disabled]:cursor-not-allowed data-[disabled]:text-text-3 data-[highlighted]:bg-panel-2";
  const separatorClass =
    "my-1 h-px bg-panel-2";

  // Selected-row styling:
  //   - primary       : strong sky-500 left border + sky-500/15 bg
  //   - non-primary   : softer sky-400/50 left border + sky-500/15 bg
  //   - unselected    : transparent border + hover bg
  const borderClass = isPrimary
    ? "border-accent"
    : isSelected
      ? "border-accent/50"
      : "border-transparent";
  const rowBgClass = isSelected
    ? "bg-accent-soft text-text"
    : "text-text-2 hover:bg-bg-2/40";
  const fontClass = isPrimary ? "font-medium" : "";

  return (
    <li
      role="treeitem"
      aria-selected={isSelected}
      // [glide] the FLIP pass measures + animates rows via this attribute;
      // stableId survives reorders (unlike the positional node.id).
      data-stable-id={node.stableId}
      className="relative"
    >
      {/* The reorder affordance is the "make room" gap — a flow spacer in the
          EmitterTree list (see the flatRows map), not a per-row overlay, so the
          rows shift to reveal where the dragged emitter(s) will land. This row
          only paints the reparent onto-ring (reparentTintClass, below). */}
      <ContextMenu.Root>
        <ContextMenu.Trigger asChild>
          <button
            type="button"
            onPointerDown={(e) => startDrag(node, e)}
            // Hovering a linked row lights up its whole group.
            onPointerEnter={() => onHoverLinkGroup(node.linkGroup || null)}
            onPointerLeave={() => onHoverLinkGroup(null)}
            onClick={(e) =>
              onRowClick(node.id, {
                ctrlKey: e.ctrlKey,
                metaKey: e.metaKey,
                shiftKey: e.shiftKey,
              })
            }
            // Right-click also promotes a non-selected row to single-
            // select before the Radix menu opens — keeps menu state
            // consistent with what the user just targeted.
            onContextMenu={() => {
              const cur = useEmitterSelectionStore.getState().ids;
              if (!cur.includes(node.id)) {
                useEmitterSelectionStore.getState().setSingle(node.id);
                void bridge.request({
                  kind: "emitters/select",
                  params: { id: node.id },
                });
              }
            }}
            data-emitter-id={node.id}
            // --record clips target a row by its positional emitter index
            // (testid:emitter-row:<node.id>) — node.id is the same handle
            // emitters/move/select use, so it's stable within a render.
            data-testid={`emitter-row:${node.id}`}
            data-link-group={node.linkGroup}
            data-link-hover={linkHover ? "true" : "false"}
            data-selected={isSelected ? "true" : "false"}
            data-primary={isPrimary ? "true" : "false"}
            data-dragging={isDragging ? "true" : "false"}
            className={[
              "grid w-full items-center gap-1.5 py-0.5 pr-2 text-left text-sm transition-colors",
              "focus-ring-inset",
              "border-l-2",
              borderClass,
              rowBgClass,
              reparentTintClass,
              fontClass,
              // Hovering a group tints its member rows. On linked
              // rows the inline group-tint (below) already covers this and
              // would override the class, so only apply the generic accent
              // tint where there's no inline tint (selected linked rows, or
              // an unlinked row sharing the hovered state).
              linkHover && spineTintColor === null ? "bg-accent/10" : "",
              // Lifted rows read distinctly from hidden ones (plain
              // opacity-50): dragging also desaturates. Dragging wins when
              // both apply.
              isDragging ? "opacity-40 saturate-50" : node.visible ? "" : "opacity-50",
            ].join(" ")}
            style={{
              paddingLeft: `${8 + indentPx}px`,
              // Visual columns: [eye | role-glyph | name]. The role glyph
              // (children only) sits in col 2; the name in col 3. The badge
              // (linked rows) is appended INLINE to the END of the name cell
              // — no own column. DOM order stays [eye, name(+badge), role];
              // badge is aria-hidden — so the accessibility tree (and the
              // emitter-tree a11y goldens) are unchanged.
              // Eye auto-places into column 1.
              // warned rows append a 16px col 4 for the chain-load
              // glyph; unwarned rows keep the 3-column template.
              gridTemplateColumns: chainWarning !== null
                ? "18px 18px 1fr 16px"
                : "18px 18px 1fr",
              // Spine: override the left-border colour to the group hue on
              // linked, unselected rows. Selected rows keep the accent border
              // (selection wins). Tint: soft full-row wash on the same rows —
              // 20% alpha while the group is hovered, 12% at rest (selected
              // rows fall through to the existing rowBgClass).
              ...(spineTintColor !== null
                ? {
                    borderLeftColor: spineTintColor,
                    backgroundColor: spineTintColor + (linkHover ? "33" : "1f"),
                  }
                : {}),
            }}
          >
            {/* F1: visibility toggle on the LEFT (replaces the old role
                dot). Always rendered so the grid columns stay stable
                during inline rename. */}
            <Tip
              content={node.visible ? "Hide emitter" : "Show emitter"}
              side="right"
            >
              <span
                role="button"
                tabIndex={0}
                data-testid={`emitter-vis-${node.id}`}
                onPointerDown={(e) => e.stopPropagation()}
                onClick={(e) => {
                  e.stopPropagation();
                  void bridge.request({
                    kind: "emitters/set-visible",
                    params: { id: node.id, visible: !node.visible },
                  });
                }}
                onKeyDown={(e) => {
                  if (e.key === "Enter" || e.key === " ") {
                    e.preventDefault();
                    e.stopPropagation();
                    void bridge.request({
                      kind: "emitters/set-visible",
                      params: { id: node.id, visible: !node.visible },
                    });
                  }
                }}
                aria-label={node.visible ? "Hide emitter" : "Show emitter"}
                className="grid place-items-center w-4 h-4 shrink-0 rounded text-text-3 hover:bg-panel-2 hover:text-text cursor-pointer focus-ring"
              >
                {node.visible
                  ? <Eye className="size-3" />
                  : <EyeOff className="size-3" />}
              </span>
            </Tip>
            {/* Name cell (col 3): the label (or inline-rename input) with the
                link dot appended INLINE at its right end on linked rows.
                Wrapping name + dot in one flex cell lets the dot sit at the
                END of the name text and reclaims the old reserved dot column. */}
            <div
              style={{ gridColumn: 3, gridRow: 1, minWidth: 0 }}
              className="flex items-center gap-1.5"
            >
              {isEditing ? (
                // Inline-rename input. Stops click + drag propagation so
                // typing doesn't accidentally re-trigger row selection /
                // drag-start. Commit on Enter, cancel on Esc; blur also
                // commits. Empty value reverts on commit (handled in the
                // parent's `commitEdit`).
                <input
                  ref={inputRef}
                  data-testid={`emitter-rename-input-${node.id}`}
                  value={editing!.value}
                  onChange={(e) => setEditValue(e.target.value)}
                  onKeyDown={(e) => {
                    // Stop the tree-level keyboard handler from snatching
                    // Backspace / Enter / Esc / arrows from the input.
                    e.stopPropagation();
                    if (e.key === "Enter") {
                      e.preventDefault();
                      commitEdit();
                    } else if (e.key === "Escape") {
                      e.preventDefault();
                      cancelEdit();
                    }
                  }}
                  onBlur={() => {
                    // Blur happens AFTER Enter / Esc handlers fire and
                    // toggle `editing` off; the conditional in the parent
                    // makes a second commit a no-op. Safer to always
                    // route through commitEdit on blur so click-outside
                    // works without an explicit click handler.
                    commitEdit();
                  }}
                  onClick={(e) => e.stopPropagation()}
                  onPointerDown={(e) => e.stopPropagation()}
                  onMouseDown={(e) => e.stopPropagation()}
                  onDoubleClick={(e) => e.stopPropagation()}
                  className="min-w-0 flex-1 rounded border border-accent bg-bg px-1 py-0 text-sm text-text outline-none"
                />
              ) : (
                <span
                  className="truncate min-w-0 flex-1"
                  data-emitter-name
                  onDoubleClick={(e) => {
                    // Double-click on the label starts inline rename. The
                    // stopPropagation prevents the click-handler chain
                    // above from re-firing single-select on the second
                    // click.
                    e.stopPropagation();
                    beginEdit(node.id, node.name);
                  }}
                >
                  {node.name}
                </span>
              )}
              {/* Badge: small G{n} chip at the right end of the name cell,
                  coloured to the group. Shrinks-0 so it stays visible even
                  when the name truncates. Clicking selects the whole group. */}
              {isLinked && (
                <Tip
                  content={`Select link group ${node.linkGroup}`}
                  side="right"
                >
                  <span
                    aria-hidden
                    data-testid={`emitter-link-badge-${node.id}`}
                    className="shrink-0 cursor-pointer rounded"
                    style={{
                      fontSize: 10,
                      padding: "2px 5px",
                      border: `1px solid ${linkColor ?? undefined}`,
                      color: linkColor ?? undefined,
                      background: "transparent",
                      pointerEvents: "auto",
                    }}
                    onPointerDown={(e) => e.stopPropagation()}
                    onClick={(e) => {
                      e.stopPropagation();
                      onSelectLinkGroup(node.linkGroup);
                    }}
                  >
                    G{node.linkGroup}
                  </span>
                </Tip>
              )}
            </div>
            {/* Spawn-role glyph for child emitters (lifetime ↻ / on-death ✕),
                placed VISUALLY in column 2 (between the eye and the name) via
                grid-column. Rendered after the name cell in DOM so the
                accessible name stays "…default lifetime child" and the
                goldens are stable. Root rows omit it; column 2 then sits empty
                and the name stays in column 3. */}
            {node.role !== "root" && (
              <span
                aria-label={roleLabel(node.role)}
                style={{ gridColumn: 2, gridRow: 1 }}
                className="inline-block w-full shrink-0 text-center font-mono text-xs text-text-3"
              >
                {roleGlyph(node.role)}
              </span>
            )}
            {/* soft chain-load warning glyph, placed VISUALLY in
                col 5 (right of the name) via grid-column but rendered
                LAST in DOM — same convention as the role glyph / link
                dot, so unwarned rows' accessible names stay byte-
                identical and the a11y goldens hold. The rich
                ChainWarningTip carries the root→offender breakdown for
                sighted users; the aria-label keeps the FULL plain-text
                breakdown for screen readers. side="right" so
                tree tooltips open toward the viewport. */}
            {chainWarning !== null && (
              <Tip
                content={<ChainWarningTip warning={chainWarning} />}
                side="right"
              >
                <span
                  style={{ gridColumn: 4, gridRow: 1 }}
                  data-testid={`emitter-chain-warning-${node.id}`}
                  aria-label={formatChainWarning(chainWarning)}
                  className="grid place-items-center w-4 h-4 shrink-0 justify-self-center text-warning-fg"
                >
                  <TriangleAlert className="size-3" />
                </span>
              </Tip>
            )}
          </button>
        </ContextMenu.Trigger>
        <ContextMenu.Portal>
          <StyledContextMenuContent
            data-testid={`emitter-context-menu-${node.id}`}
          >
            <ContextMenu.Item onSelect={handleRename} className={menuItemClass}>
              Rename
            </ContextMenu.Item>
            <ContextMenu.Item onSelect={handleDuplicate} className={menuItemClass}>
              Duplicate
            </ContextMenu.Item>
            <ContextMenu.Item onSelect={handleDelete} className={menuItemClass}>
              Delete
            </ContextMenu.Item>
            <ContextMenu.Separator className={separatorClass} />
            <ContextMenu.Item onSelect={handleContextCut} className={menuItemClass}>
              Cut
            </ContextMenu.Item>
            <ContextMenu.Item onSelect={handleContextCopy} className={menuItemClass}>
              Copy
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={handleContextPaste}
              disabled={!hasClipboard}
              className={menuItemClass}
            >
              Paste
            </ContextMenu.Item>
            <ContextMenu.Sub>
              <ContextMenu.SubTrigger
                disabled={!hasClipboard}
                className={menuItemClass}
              >
                Paste As
              </ContextMenu.SubTrigger>
              <ContextMenu.Portal>
                <StyledContextSubContent
                >
                  <ContextMenu.Item
                    onSelect={handlePasteAsLifetime}
                    disabled={!hasClipboard || hasLifetimeChild}
                    className={menuItemClass}
                  >
                    Lifetime Child
                  </ContextMenu.Item>
                  <ContextMenu.Item
                    onSelect={handlePasteAsDeath}
                    disabled={!hasClipboard || hasDeathChild}
                    className={menuItemClass}
                  >
                    Death Child
                  </ContextMenu.Item>
                </StyledContextSubContent>
              </ContextMenu.Portal>
            </ContextMenu.Sub>
            <ContextMenu.Separator className={separatorClass} />
            <ContextMenu.Item onSelect={handleIncrement} className={menuItemClass}>
              Increment Index…
            </ContextMenu.Item>
            <ContextMenu.Item onSelect={handleRescale} className={menuItemClass}>
              Rescale Emitter…
            </ContextMenu.Item>
            {/* ─────────────────────────────────────────────────── */}
            <ContextMenu.Separator className={separatorClass} />
            <ContextMenu.Item onSelect={handleNewRoot} className={menuItemClass}>
              New Root Emitter
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={handleAddLifetimeChild}
              disabled={hasLifetimeChild}
              className={menuItemClass}
            >
              Add Lifetime Child
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={handleAddDeathChild}
              disabled={hasDeathChild}
              className={menuItemClass}
            >
              Add Death Child
            </ContextMenu.Item>
            <ContextMenu.Separator className={separatorClass} />
            <ContextMenu.Item
              onSelect={handleMoveUp}
              disabled={!canMoveUp}
              className={menuItemClass}
            >
              Move Up
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={handleMoveDown}
              disabled={!canMoveDown}
              className={menuItemClass}
            >
              Move Down
            </ContextMenu.Item>
            <ContextMenu.Separator className={separatorClass} />
            <ContextMenu.Item
              onSelect={handleSetLinkGroup}
              className={menuItemClass}
            >
              Set Link Group…
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={handleLeaveLinkGroup}
              disabled={leaveLinkGroupDisabled}
              className={menuItemClass}
            >
              Leave Link Group
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={() => onDissolveLinkGroup(node.linkGroup)}
              disabled={!isLinked}
              className={menuItemClass}
            >
              Dissolve Link Group
            </ContextMenu.Item>
            <ContextMenu.Item
              onSelect={handleLinkGroupSettings}
              disabled={!isLinked}
              className={menuItemClass}
            >
              Link Group Settings…
            </ContextMenu.Item>
          </StyledContextMenuContent>
        </ContextMenu.Portal>
      </ContextMenu.Root>
      {/* Spine hit-strip: 8px wide invisible strip over the row's left
          edge, so the 2px coloured border is easy to click. Rendered
          only on linked rows. Sits absolutely inside the relative <li>.
          Width 8px stays inside the gutter (eye toggle begins at
          paddingLeft ≥ 8px) so it never overlaps content. */}
      {isLinked && (
        <Tip
          content={`Select link group ${node.linkGroup}`}
          side="right"
        >
          <span
            aria-hidden
            data-testid={`emitter-link-spine-${node.id}`}
            className="cursor-pointer"
            style={{ position: "absolute", left: 0, top: 0, bottom: 0, width: 8, pointerEvents: "auto" }}
            onPointerDown={(e) => e.stopPropagation()}
            onClick={(e) => {
              e.stopPropagation();
              onSelectLinkGroup(node.linkGroup);
            }}
          />
        </Tip>
      )}
    </li>
  );
}

// ─── Panel-header toolbar ────────────────────────────────────────────
// Restore the legacy panel toolbar from
// src/UI/EmitterList.cpp:3016. Layout matches legacy ordering:
//   [New ▾] [Delete] [▲ Move Up] [▼ Move Down]   (this batch)
//   [👁]    [Show All] [Hide All]                (next batch)
// All four buttons here use bridge calls that already exist in the
// schema — no host-side work needed.

// F2: 28px square to match the main toolbar's `.tb-btn`. F3: `:active`
// pressed state (lighter bg + slight scale) via Tailwind `active:`,
// suppressed while disabled.
const TOOLBAR_BTN =
  "flex h-7 w-7 items-center justify-center rounded text-text-2 transition hover:bg-panel-2 hover:text-text active:bg-panel-3 active:scale-95 disabled:cursor-not-allowed disabled:text-text-3 disabled:hover:bg-transparent disabled:active:scale-100 focus-ring";

const NEW_EMITTER_MENU_ITEM =
  "flex select-none items-center gap-2 rounded px-2 py-1 text-xs text-text hover:bg-panel-2 data-[highlighted]:bg-panel-2 outline-none cursor-pointer data-[disabled]:text-text-3 data-[disabled]:cursor-not-allowed data-[disabled]:hover:bg-transparent";

function findNodeInTree(
  tree: EmitterTreeDto | null,
  id: number | null,
): { node: EmitterTreeNode; siblings: EmitterTreeNode[]; indexInSiblings: number } | null {
  if (tree === null || id === null) return null;
  const walk = (
    siblings: EmitterTreeNode[],
  ): ReturnType<typeof findNodeInTree> => {
    for (let i = 0; i < siblings.length; i++) {
      const n = siblings[i];
      if (n.id === id) return { node: n, siblings, indexInSiblings: i };
      const hit = walk(n.children);
      if (hit !== null) return hit;
    }
    return null;
  };
  return walk(tree.root.children);
}

type ToolbarProps = {
  bridge: Bridge;
  tree: EmitterTreeDto | null;
  primaryId: number | null;
};

function EmitterTreeToolbar({ bridge, tree, primaryId }: ToolbarProps) {
  const primary = findNodeInTree(tree, primaryId);
  const hasPrimary = primary !== null;
  const selIds = useEmitterSelectionStore((s) => s.ids);
  // Lifetime/Death child adds require both a primary AND a free slot
  // (parents can hold at most one of each role).
  const canAddLifetime =
    hasPrimary && !primary!.node.children.some((c) => c.role === "lifetime");
  const canAddDeath =
    hasPrimary && !primary!.node.children.some((c) => c.role === "death");
  // Move is a root-only operation — same gate as the per-row context
  // menu. Sibling reordering at lifetime/death depth is a separate
  // capability not exposed by the legacy panel toolbar either.
  // Move enabled state mirrors move-many's preserve rule, via the shared
  // helper (same predicate the row context-menu Move items use).
  const rootIds = (tree?.root.children ?? []).map((c) => c.id);
  const canMoveUp = canMoveSelection(selIds, rootIds, "up");
  const canMoveDown = canMoveSelection(selIds, rootIds, "down");

  const addRoot = () =>
    void bridge.request({ kind: "emitters/add-root", params: {} });
  const addLifetime = () => {
    if (primaryId === null) return;
    void bridge.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: primaryId },
    });
  };
  const addDeath = () => {
    if (primaryId === null) return;
    void bridge.request({
      kind: "emitters/add-death-child",
      params: { parentId: primaryId },
    });
  };
  const duplicatePrimary = () => {
    // Toolbar Duplicate is selection-aware (like the trash button); the new
    // copies become the selection.
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    void duplicateEmitters(bridge, ids);
  };
  const del = () => {
    // Selection-aware: the trash button deletes the WHOLE multi-selection
    // (matching right-click → Delete and the Delete key), not just the
    // primary row. ids and primary stay in sync, so an empty selection ==
    // no primary == nothing to delete.
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    requestDeleteEmitters(bridge, ids);
  };
  const moveUp = () => {
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    void moveEmitters(bridge, ids, "up");
  };
  const moveDown = () => {
    const ids = useEmitterSelectionStore.getState().ids;
    if (ids.length === 0) return;
    void moveEmitters(bridge, ids, "down");
  };
  const showAll = () =>
    void bridge.request({
      kind: "emitters/set-all-visible",
      params: { visible: true },
    });
  const hideAll = () =>
    void bridge.request({
      kind: "emitters/set-all-visible",
      params: { visible: false },
    });

  return (
    <div
      data-testid="emitter-tree-toolbar"
      className="tree-actions"
    >
      <Menubar.Root>
        <Menubar.Menu>
          <Tip content="New Emitter">
            <Menubar.Trigger
              className={TOOLBAR_BTN}
              aria-label="New Emitter"
            >
              <Plus className="size-4" />
            </Menubar.Trigger>
          </Tip>
          <Menubar.Portal>
            <Menubar.Content
              className="min-w-[160px] rounded-md border border-border bg-bg-2 p-1 shadow-[var(--shadow-soft)] z-50"
              align="start"
              sideOffset={4}
            >
              <Menubar.Item
                onSelect={addRoot}
                className={NEW_EMITTER_MENU_ITEM}
              >
                Root Emitter
              </Menubar.Item>
              <Menubar.Item
                onSelect={addLifetime}
                disabled={!canAddLifetime}
                className={NEW_EMITTER_MENU_ITEM}
              >
                Lifetime Child
              </Menubar.Item>
              <Menubar.Item
                onSelect={addDeath}
                disabled={!canAddDeath}
                className={NEW_EMITTER_MENU_ITEM}
              >
                Death Child
              </Menubar.Item>
            </Menubar.Content>
          </Menubar.Portal>
        </Menubar.Menu>
      </Menubar.Root>
      {/* Span shims: disabled buttons fire no pointer events, so the Tip
          listens on a wrapping span that stays interactive while the button
          inside is disabled. */}
      <Tip content="Duplicate">
        <span className="inline-block">
          <button
            type="button"
            className={TOOLBAR_BTN}
            aria-label="Duplicate emitter"
            disabled={!hasPrimary}
            onClick={duplicatePrimary}
          >
            <Copy className="size-4" />
          </button>
        </span>
      </Tip>
      <Tip content="Delete">
        <span className="inline-block">
          <button
            type="button"
            className={TOOLBAR_BTN}
            aria-label="Delete emitter"
            disabled={!hasPrimary}
            onClick={del}
          >
            <Trash2 className="size-4" />
          </button>
        </span>
      </Tip>
      <Tip content="Move Up">
        <span className="inline-block">
          <button
            type="button"
            className={TOOLBAR_BTN}
            aria-label="Move emitter up"
            disabled={!canMoveUp}
            onClick={moveUp}
          >
            <ChevronUp className="size-4" />
          </button>
        </span>
      </Tip>
      <Tip content="Move Down">
        <span className="inline-block">
          <button
            type="button"
            className={TOOLBAR_BTN}
            aria-label="Move emitter down"
            disabled={!canMoveDown}
            onClick={moveDown}
          >
            <ChevronDown className="size-4" />
          </button>
        </span>
      </Tip>
      <Tip content="Show All Emitters">
        <button
          type="button"
          className={TOOLBAR_BTN}
          aria-label="Show all emitters"
          onClick={showAll}
        >
          <Eye className="size-4" />
        </button>
      </Tip>
      <Tip content="Hide All Emitters">
        <button
          type="button"
          className={TOOLBAR_BTN}
          aria-label="Hide all emitters"
          onClick={hideAll}
        >
          <EyeOff className="size-4" />
        </button>
      </Tip>
    </div>
  );
}

export function EmitterTree({ bridge }: Props) {
  const tree = useEmitterTreeStore((s) => s.tree);
  const setTree = useEmitterTreeStore((s) => s.setTree);
  const selectedIds = useEmitterSelectionIds();
  const primaryId = useEmitterSelectionPrimary();

  // [indicator-consistency] Live guard config: the glyph threshold follows
  // the configurable cap while the guard is enabled (glyph ⟺ gate), and
  // falls back to the advisory 10k when disabled.
  const guard = useOverloadGuardConfig();

  // stableId → soft chain-load warning, recomputed whenever the
  // tree refetches (spawn values ride the tree DTO, so a properties edit
  // that matters lands here via emitters/tree/changed → setTree).
  const chainWarnings = useMemo(
    () =>
      tree !== null
        ? estimateChainLoad(tree.root, guard.enabled ? guard.maxParticles : undefined)
        : new Map<number, ChainWarning>(),
    [tree, guard],
  );

  // [indicator-consistency] System-total estimate for the chip — the same
  // walk useEstimatedLoadPush(bridge, tree) runs for the engine push below;
  // recomputing the pure O(nodes) walk here is cheaper than widening the
  // hook's signature.
  const systemLoad = useMemo(
    () => (tree !== null ? estimateSystemLoad(tree.root) : 0),
    [tree],
  );

  // [hard-guard] Push the system-total alive estimate to the engine on
  // every tree update (same source as the ⚠ glyph — gate and glyph agree
  // by construction). BridgeDispatcher caches the value and reapplies on
  // SetEngine.
  useEstimatedLoadPush(bridge, tree);

  // Drag/drop state. `draggingId` is the source row's id;
  // `indicator` is the row + zone currently displaying a drop hint.
  // Both lifted to the tree level so only one indicator can be active
  // and so rows can read the dragged node's subtree for cycle checks.
  const [draggingId, setDraggingId] = useState<number | null>(null);
  // [multi-drag] ids of every lifted row in the block (all dim while dragging).
  const [draggingIds, setDraggingIds] = useState<number[]>([]);
  const [indicator, setIndicator] = useState<DropIndicator>(null);
  // [multi-drag] Cursor chip following the pointer during a multi-root drag —
  // lists the dragged emitter names vertically, in their emitter-list order.
  // `null` outside a multi-drag (single-drag uses the per-row insertion line).
  const [dragChip, setDragChip] = useState<
    // `exit` set = despawn in flight: the chip transitions to that point
    // (the landing gap / reparent row, or its own spot for a cancel) while
    // fading, then a timeout clears the state.
    { x: number; y: number; names: string[]; exit?: { x: number; y: number } } | null
  >(null);
  // [pointer-drag] Set true when a real drag completes so the synthetic
  // click that follows pointerup (when down+up land on the same row) does
  // NOT also fire row selection. Reset on the next pointerdown and in
  // handleRowClick. (HTML5 DnD is replaced by pointer events because
  // dragstart never fires under composition hosting — WebView2 is a
  // composition visual with no HWND for the OS drag loop.)
  const draggedRef = useRef(false);

  // While a pointer drag is active this holds a function that
  // ABORTS it (tears down dims/gap/chip + listeners, commits nothing). The
  // emitters/tree/changed subscription calls it before refetching, so a host-
  // side structural change mid-drag — undo/redo/paste reach the accelerators
  // focus-independently, or another pane mutates — can't let the gesture commit
  // captured-but-now-stale POSITIONAL ids (which would move the wrong emitters)
  // or render the gap/dim against a reshuffled tree. The drag is cancelled and
  // the user re-drags against the fresh tree.
  const activeDragCancelRef = useRef<(() => void) | null>(null);

  // The pointerId of the in-flight drag (null = none). Re-entrancy
  // guard: a second pointerdown while a drag is live is ignored, so two
  // gestures can't register duplicate listeners over shared controller state.
  const dragPointerRef = useRef<number | null>(null);

  // Inline rename. Local component state because only the
  // tree owns both the focus target (each row's button) and the input
  // (mounted inside the row). One row at a time; null = no edit in
  // progress.
  const [editing, setEditing] = useState<RenameEditingState>(null);
  const editingRef = useRef<RenameEditingState>(null);
  useEffect(() => { editingRef.current = editing; }, [editing]);

  const beginEdit = useCallback((id: number, currentName: string) => {
    setEditing({ id, value: currentName, original: currentName });
  }, []);
  const setEditValue = useCallback((value: string) => {
    setEditing((cur) => (cur === null ? null : { ...cur, value }));
  }, []);
  const cancelEdit = useCallback(() => { setEditing(null); }, []);
  const commitEdit = useCallback(() => {
    const cur = editingRef.current;
    if (cur === null) return;
    const trimmed = cur.value.trim();
    // Empty name → silent revert (no bridge call). Matches legacy: an
    // empty rename was rejected at the TreeView level.
    // Unchanged name → still revert without firing the bridge call;
    // saves a wire round-trip on a no-op commit (e.g. F2 → Enter).
    if (trimmed.length === 0 || trimmed === cur.original) {
      setEditing(null);
      return;
    }
    void bridge.request({
      kind: "emitters/rename",
      params: { id: cur.id, name: trimmed },
    });
    setEditing(null);
  }, [bridge]);

  // Fetch the full tree from the host. Pulled into a callback so the
  // tree-changed subscription can re-trigger it.
  const refreshTree = useCallback(() => {
    let cancelled = false;
    bridge
      .request({ kind: "emitters/list", params: {} })
      .then((t) => {
        // Store invariant: null or a well-formed tree (every consumer here
        // assumes a truthy tree has a `root`). Ignore a malformed/partial
        // response — e.g. a stubbed `{}` — so it can't reach the renderers.
        if (!cancelled && (t as EmitterTreeDto | null)?.root) setTree(t);
      })
      .catch((err) => console.warn("[EmitterTree] emitters/list failed:", err));
    return () => { cancelled = true; };
  }, [bridge]);

  // Initial fetch + tree-changed subscription.
  useEffect(() => {
    const cancelList = refreshTree();
    const offTree = bridge.on("emitters/tree/changed", () => {
      // A structural change while a drag is held invalidates
      // the gesture's pointerdown snapshot (positional ids + geometry). Abort
      // it BEFORE refetching so it can't commit stale ids or paint a stale
      // gap/dim against the reshuffled tree.
      activeDragCancelRef.current?.();
      refreshTree();
    });
    return () => {
      cancelList();
      offTree();
    };
  }, [bridge, refreshTree]);

  // Initial selected-id seed from snapshot + live updates from
  // emitters/selected events. The server tracks only the primary; we
  // sync that into the React-side selection atom whenever a new
  // selection arrives from outside (devtools poke, post-mutation cleanup).
  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (cancelled) return;
        const id = s.selectedEmitterId ?? null;
        const sel = useEmitterSelectionStore.getState();
        if (id === null) {
          if (sel.ids.length === 0 && sel.primary === null) return;
          sel.clear();
        } else if (sel.primary !== id) {
          sel.setSingle(id);
        }
      })
      .catch((err) => console.warn("[EmitterTree] snapshot failed:", err));
    const offSelected = bridge.on("emitters/selected", (e) => {
      const id = e.payload.id;
      const sel = useEmitterSelectionStore.getState();
      if (id === null) {
        sel.clear();
      } else if (sel.primary !== id) {
        // Server says "primary is now id". If the React-side set
        // doesn't include it, replace the selection; if it already
        // contains it, just shift primary without dropping the set.
        if (sel.ids.includes(id)) {
          useEmitterSelectionStore.setState({ primary: id });
        } else {
          sel.setSingle(id);
        }
      }
    });
    return () => {
      cancelled = true;
      offSelected();
    };
  }, [bridge]);

  // Flatten the tree once per change. The flat list drives both
  // render and the shift-click range computation.
  const flatRows  = useMemo(() => flattenTree(tree), [tree]);
  const orderedIds = useMemo(() => flatRows.map((r) => r.node.id), [flatRows]);

  // [glide] FLIP pass: whenever the rendered layout changes — a reorder
  // commit (drag drop, Move Up/Down, delete, paste) OR the make-room gap
  // inserting/moving/clearing DURING a drag — rows that moved glide to their
  // new positions instead of snapping (user call: the glide plays while
  // dragging too, not just on release). Measure offsetTop per stableId
  // BEFORE paint, diff against the previous layout, add any in-flight
  // transform residual (an interrupted glide restarts from the row's current
  // VISUAL position, so rapid gap hops stay continuous), apply the inverted
  // translateY, then transition to zero — snappier mid-drag (FLIP_DRAG_MS)
  // than on settle (FLIP_SETTLE_MS). The dragged block's own rows glide the
  // same way when the gap crosses their footprint. Resolution math is
  // unaffected: gap/onto targets come from the drag-activation geometry
  // snapshot, never from the animated DOM.
  // prefers-reduced-motion: bookkeeping only, no glide.
  // [glide] A row remount (undo/redo rebuilds emitters with FRESH stableIds,
  // so every keyed row remounts) destroys the focused row button and drops
  // keyboard focus to <body> — killing arrow-key nav until a re-click. Track
  // whether focus lives inside the tree (capture handlers on the container;
  // note removal of a focused element fires NO blur, so the flag survives the
  // remount) and restore focus to the primary row after the commit. Only
  // fires when focus was actually dropped (activeElement === body) so it can
  // never steal focus from a modal or another pane.
  const treeHadFocusRef = useRef(false);
  useEffect(() => {
    if (!treeHadFocusRef.current) return;
    if (document.activeElement !== document.body) return;
    const container = treeContainerRef.current;
    if (container === null) return;
    const primaryBtn = primaryId !== null
      ? container.querySelector<HTMLElement>(`button[data-emitter-id="${primaryId}"]`)
      : null;
    (primaryBtn ?? container).focus();
  }, [flatRows, primaryId]);

  const recording = useRecording();
  const flipPositionsRef = useRef<FlipPositions>(new Map());
  useLayoutEffect(() => {
    const sc = treeScrollRef.current;
    const prev = flipPositionsRef.current;
    const next: FlipPositions = new Map();
    const els = new Map<number, HTMLElement>();
    if (sc !== null) {
      sc.querySelectorAll<HTMLElement>("li[data-stable-id]").forEach((li) => {
        const stableId = Number(li.dataset.stableId);
        next.set(stableId, li.offsetTop);
        els.set(stableId, li);
      });
    }
    flipPositionsRef.current = next;
    // prefers-reduced-motion skips the glide — EXCEPT under --record, where the
    // glide must render into the captured clip regardless of the host's setting.
    if (
      !recording &&
      typeof window.matchMedia === "function" &&
      window.matchMedia("(prefers-reduced-motion: reduce)").matches
    ) {
      return;
    }
    // Long wall-clock transition only for the --record active-drag glide (so it
    // spans multiple slow captures); the settle keeps FLIP_SETTLE_MS to stay in
    // step with the drag-chip despawn. See pickFlipDuration.
    const durationMs = pickFlipDuration(recording, draggingId !== null, {
      dragMs: FLIP_DRAG_MS,
      settleMs: FLIP_SETTLE_MS,
      recordDragMs: FLIP_RECORD_MS,
    });
    for (const [stableId, dy] of computeFlipDeltas(prev, next)) {
      const el = els.get(stableId);
      if (el === undefined) continue;
      // In-flight residual: if a previous glide is still running, its
      // computed transform holds the row's current visual offset — start the
      // new glide from there so interruptions don't teleport.
      const m = /matrix\([^,]+,[^,]+,[^,]+,[^,]+,[^,]+,\s*(-?[\d.]+)\)/
        .exec(getComputedStyle(el).transform);
      const total = dy + (m !== null ? parseFloat(m[1]!) : 0);
      if (total === 0) continue;
      el.style.transition = "none";
      el.style.transform = `translateY(${total}px)`;
      void el.offsetHeight; // commit the inverted transform before transitioning
      el.style.transition = `transform ${durationMs}ms ease`;
      el.style.transform = "";
    }
  }, [flatRows, draggingId, indicator, recording]);

  // Subscribe to menu-driven rename
  // requests. The MenuBar's "Rename Emitter" item writes the target
  // id into the tree-action atom; we pick it up, begin inline edit
  // (same path as F2 / context-menu Rename / dbl-click), and consume
  // the request. Silently no-op if the target id doesn't resolve to
  // a current row — matches the defensive guard the F2 handler uses
  // for the same race (mid-mutation, deleted emitter, etc.).
  const renameRequest = useTreeActionStore((s) => s.renameRequest);
  useEffect(() => {
    if (renameRequest === null) return;
    const node = flatRows.find((r) => r.node.id === renameRequest)?.node ?? null;
    if (node !== null) beginEdit(node.id, node.name);
    useTreeActionStore.getState().consumeRenameRequest();
  }, [renameRequest, flatRows, beginEdit]);

  const handleRowClick = useCallback(
    (
      id: number,
      mods: { ctrlKey: boolean; metaKey: boolean; shiftKey: boolean },
    ) => {
      // Suppress the synthetic click that follows a pointer-drag's pointerup
      // (down+up on the same row) so a drag doesn't also re-select the row.
      if (draggedRef.current) { draggedRef.current = false; return; }
      const sel = useEmitterSelectionStore.getState();
      if (mods.shiftKey) {
        sel.range(id, orderedIds);
      } else if (mods.ctrlKey || mods.metaKey) {
        sel.toggle(id);
      } else {
        sel.setSingle(id);
      }
      // Always sync the new primary to the server. After the action
      // above, primary may have shifted (toggle that removed primary,
      // for example).
      const newPrimary = useEmitterSelectionStore.getState().primary;
      void bridge.request({
        kind: "emitters/select",
        params: { id: newPrimary },
      });
    },
    [bridge, orderedIds],
  );

  const rootChildren = tree?.root.children ?? [];

  // [pointer-drag] Pointer-based drag-to-reorder / -reparent. HTML5 DnD
  // never initiates under composition hosting (WebView2
  // is a composition visual with no HWND for the OS drag loop), so the
  // tree drag is driven by pointer events — they deliver like clicks in
  // every hosting mode (and on touch). On pointerdown we arm a drag and
  // attach document-level move/up listeners; once the pointer crosses a
  // small threshold the drag goes "active" (dims the source, shows the
  // drop indicator). The hovered row is found from the move event's
  // target (its `[data-emitter-id]`); on pointerup the resolved
  // `emitters/drop` is dispatched. Closures capture the tree snapshot at
  // drag-start — safe because the tree doesn't mutate mid-gesture.
  const startDrag = (source: EmitterTreeNode, e: React.PointerEvent) => {
    if (e.button !== 0) return;               // primary button only
    if (editingRef.current !== null) return;  // not while inline-renaming
    // One tree drag at a time — a second pointerdown (second mouse,
    // pen) while a drag is live must not arm a duplicate controller over the
    // shared component state.
    if (dragPointerRef.current !== null) return;
    const pointerId = e.pointerId;
    dragPointerRef.current = pointerId;
    // Capture the pointer so losing it (alt-tab / window blur)
    // delivers pointercancel — Chromium synthesises it for captured pointers —
    // and so the gesture only reacts to ITS pointer. jsdom has no
    // setPointerCapture; guard so unit tests are unaffected.
    const captureTarget = e.currentTarget as HTMLElement;
    try { captureTarget.setPointerCapture?.(pointerId); } catch { /* jsdom / lost pointer */ }
    draggedRef.current = false;
    const startX = e.clientX;
    const startY = e.clientY;
    let lastX = startX;
    let lastY = startY;
    const curTree = tree;
    const curRoots = rootChildren;
    const curRows = flatRows;
    // The gesture moves a BLOCK of root subtrees. `blockIds` are the roots it
    // carries, in tree order: a multi-root selection moves the whole selection;
    // a single ROOT drag moves just that root (a size-1 block); a single CHILD
    // drag carries no root block (reparent-only). `blockRootIdxs` are their
    // ascending root indices. Both single-root and multi reorder commit through
    // `reorderManyEmitters(blockIds, lastReorderGap)`; a single drag may instead
    // resolve a reparent into `lastParams`.
    const selIds = useEmitterSelectionStore.getState().ids;
    const multi = isMultiDrag(source.id, selIds, curRoots);
    const sourceIsRoot = curRoots.some((c) => c.id === source.id);
    const blockIds = multi
      ? selectedRootIdsInOrder(selIds, curRoots)
      : sourceIsRoot ? [source.id] : [];
    const blockRootIdxs = blockIds.map((id) => curRoots.findIndex((c) => c.id === id)); // ascending
    const srcRootIdx = sourceIsRoot ? curRoots.findIndex((c) => c.id === source.id) : -1;
    // Chip name list (tree order): one name for a single-root drag, all for a
    // multi; empty for a single CHILD (reparent has no chip).
    const chipNames = blockIds
      .map((id) => curRows.find((r) => r.node.id === id)?.node.name ?? "")
      .filter(Boolean);
    const hasChip = chipNames.length > 0;
    // The dim set: every dragged root's WHOLE subtree (children ride along on
    // a reorder, so they lift visually too). Single-drag dims its subtree for
    // the same reason — whatever you pick up, all of it reads as "in hand".
    const dimIds = multi
      ? blockIds.flatMap((id) => {
          const n = curRoots.find((c) => c.id === id);
          return n ? collectSubtreeIds(n) : [id];
        })
      : collectSubtreeIds(source);
    let lastReorderGap: number | null = null;
    let active = false;
    let lastParams: DropParams | null = null;  // single-drag reparent, when resolved
    let ontoTarget: number | null = null;      // row id currently showing the onto ring
    let rafId: number | null = null;
    const THRESHOLD = 4;
    // Geometry snapshots (captured at activation, resting layout) + the lifted
    // block's measured height. `geom` = per-root-block extents (reorder gaps);
    // `rowGeom` = per-row extents (single-drag onto hit-test). Every later
    // resolve is pure math against these, never live (gap-shifted) DOM.
    let geom: RootBlockGeometry | null = null;
    let rowGeom: RowGeometry | null = null;
    let liftedH = 0;
    // Chip spring state: the chip glides toward its target (the pointer, pulled
    // toward the active gap) instead of teleporting.
    const chipPos = { x: startX + 12, y: startY + 12 };
    const reduceMotion =
      typeof window.matchMedia === "function" &&
      window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    // Pointer client-Y → scroll-content space (the snapshot's frame).
    const toContentY = (clientY: number, sc: HTMLElement): number =>
      clientY - sc.getBoundingClientRect().top + sc.scrollTop;

    // Show / clear the reorder make-room gap at `gapIndex` (idempotent).
    const setReorderGap = (gapIndex: number | null) => {
      if (gapIndex === lastReorderGap) return;
      lastReorderGap = gapIndex;
      lastParams = null;
      setIndicator(gapIndex === null ? null : { kind: "gap", gapIndex, gapHeight: liftedH });
    };

    // [multi-drag] Reorder-only: resolve the gap geometrically. Continuous —
    // every position maps to a gap or the footprint no-op, nothing to flicker.
    const updateMultiTarget = (clientY: number) => {
      const sc = treeScrollRef.current;
      if (sc === null || geom === null) return;
      const res = resolveGapFromGeometry(geom, blockRootIdxs, toContentY(clientY, sc), lastReorderGap, liftedH);
      // "noop" → clear (release leaves the order unchanged; not forced to move).
      setReorderGap(res === "noop" ? null : res.rootIndex);
    };

    // Single-drag: ONE geometric pass yields either the reorder gap (above/below
    // a root) or a reparent onto-target (middle third of a row). `reparentOk`
    // injects the tree-aware reparent validity (slot / cycle / same-parent).
    const updateSingleTarget = (clientY: number) => {
      const sc = treeScrollRef.current;
      if (sc === null || geom === null || rowGeom === null) return;
      const reparentParamsFor = (targetId: number): DropParams | null => {
        const tnode = curRows.find((r) => r.node.id === targetId)?.node ?? null;
        if (tnode === null) return null;
        const tRootIdx = curRoots.findIndex((c) => c.id === targetId);
        return resolveDropIntent(source, tnode, tRootIdx, "onto", curTree, curRoots);
      };
      const res = resolveSingleRootDrop(
        geom, rowGeom, srcRootIdx, source.id,
        (id) => reparentParamsFor(id) !== null,
        toContentY(clientY, sc), lastReorderGap, liftedH,
      );
      if (res !== "noop" && res.kind === "onto") {
        const params = reparentParamsFor(res.targetId);
        lastReorderGap = null;
        lastParams = params;
        const want = params !== null ? res.targetId : null;
        if (want !== ontoTarget) {
          ontoTarget = want;
          setIndicator(want !== null ? { kind: "onto", targetId: want } : null);
        }
        return;
      }
      // Reorder (root source) or nothing (child source / footprint no-op).
      // Leaving the onto zone MUST drop any latched reparent — otherwise a
      // release over the no-op footprint would still commit the stale
      // reparent and the ring would stay painted. setReorderGap's idempotence
      // check can't see the onto state, so clear it explicitly first.
      lastParams = null;
      if (ontoTarget !== null) {
        ontoTarget = null;
        lastReorderGap = null;
        setIndicator(null);
      }
      setReorderGap(res !== "noop" && sourceIsRoot ? res.rootIndex : null);
    };

    const updateTarget = (clientY: number) =>
      multi ? updateMultiTarget(clientY) : updateSingleTarget(clientY);

    // Advance the chip one spring step toward its target and render it. Called
    // per pointermove AND per rAF tick (so it keeps gliding between moves);
    // under prefers-reduced-motion it jumps straight to the target — the pull
    // is information, the glide is decoration. No chip for a reparent-only
    // (single-child) drag.
    const stepChip = () => {
      if (!hasChip || !active) return;
      const sc = treeScrollRef.current;
      let gapCenter: number | null = null;
      if (sc !== null && geom !== null && lastReorderGap !== null) {
        gapCenter =
          gapContentY(geom, lastReorderGap) - sc.scrollTop +
          sc.getBoundingClientRect().top + liftedH / 2;
      }
      const target = computeChipTarget(lastX, lastY, gapCenter, CHIP_PULL);
      if (reduceMotion) {
        chipPos.x = target.x;
        chipPos.y = target.y;
      } else {
        chipPos.x += (target.x - chipPos.x) * CHIP_SPRING;
        chipPos.y += (target.y - chipPos.y) * CHIP_SPRING;
      }
      setDragChip({ x: chipPos.x, y: chipPos.y, names: chipNames });
    };

    // While the pointer sits in an edge zone of the scroll viewport,
    // scroll it each frame (proportional to depth) and re-resolve the drop
    // target so the indicator follows the rows moving under the pointer.
    const tick = () => {
      const sc = treeScrollRef.current;
      if (sc !== null) {
        const delta = computeAutoscrollDelta(lastY, sc.getBoundingClientRect());
        if (delta !== 0) {
          sc.scrollTop += delta;
          updateTarget(lastY);
        }
      }
      stepChip();
      rafId = requestAnimationFrame(tick);
    };

    const onMove = (ev: PointerEvent) => {
      if (ev.pointerId !== pointerId) return; // our pointer only
      lastX = ev.clientX;
      lastY = ev.clientY;
      if (!active) {
        if (Math.abs(ev.clientX - startX) + Math.abs(ev.clientY - startY) < THRESHOLD) {
          return;
        }
        active = true;
        setDraggingId(source.id);
        setDraggingIds(dimIds);
        // Snapshot geometry NOW — but first finish any in-flight reorder
        // glide instantly: the snapshot reads getBoundingClientRect, which
        // INCLUDES live FLIP transforms, so a re-grab within the ~200ms glide
        // window would otherwise capture mid-animation positions and corrupt
        // every gap/onto resolution for the whole gesture.
        treeScrollRef.current
          ?.querySelectorAll<HTMLElement>("li[data-stable-id]")
          .forEach((li) => {
            li.style.transition = "none";
            li.style.transform = "";
          });
        // Both snapshots against the now-resting layout: block extents
        // (reorder gaps) + row extents (single-drag onto hit-test).
        geom = captureRootBlockGeometry(treeScrollRef.current, curRoots);
        rowGeom = captureRowGeometry(treeScrollRef.current, curRows);
        liftedH = geom !== null ? liftedBlockHeight(geom, blockRootIdxs) : 0;
        // Esc / right-click cancel only an ACTIVE drag — attach the
        // listeners on activation so a pre-threshold right-click still opens
        // the row context menu. Start the autoscroll loop.
        document.addEventListener("keydown", onKey, true);
        document.addEventListener("contextmenu", onCtx, true);
        // Expose the abort hook only while active, so a mid-
        // drag tree mutation cancels the gesture before it commits stale ids.
        // Tear down on focus loss (alt-tab / window blur / tab hide)
        // so the drag can't get stranded with no pointerup ever arriving.
        activeDragCancelRef.current = () => finish(false);
        window.addEventListener("blur", onBlur);
        document.addEventListener("visibilitychange", onVis);
        rafId = requestAnimationFrame(tick);
      }
      updateTarget(ev.clientY);
      stepChip(); // also per-move, so the chip exists without waiting on rAF
    };

    const finish = (commit: boolean) => {
      document.removeEventListener("pointermove", onMove);
      document.removeEventListener("pointerup", onUp);
      document.removeEventListener("pointercancel", onCancel);
      document.removeEventListener("keydown", onKey, true);
      document.removeEventListener("contextmenu", onCtx, true);
      window.removeEventListener("blur", onBlur);
      document.removeEventListener("visibilitychange", onVis);
      // Clear the re-entrancy latch + abort hook on EVERY exit
      // path (including a pre-threshold release that returns early below) and
      // release the captured pointer.
      dragPointerRef.current = null;
      activeDragCancelRef.current = null;
      try { captureTarget.releasePointerCapture?.(pointerId); } catch { /* already released */ }
      if (rafId !== null) {
        cancelAnimationFrame(rafId);
        rafId = null;
      }
      if (!active) return;
      active = false; // no straggler tick/move may touch the chip again
      setDraggingId(null);
      setDraggingIds([]);
      setIndicator(null);
      // Chip despawn: on a COMMIT, fly into the landing spot — the reorder
      // gap's center, or the reparent target row — selling "the emitters went
      // in there"; on cancel/no-op, fade where it stands. Reduced motion (or
      // no chip) clears immediately.
      const sc = treeScrollRef.current;
      if (!hasChip || reduceMotion || sc === null || geom === null) {
        setDragChip(null);
      } else {
        const scRect = sc.getBoundingClientRect();
        let exit = { x: chipPos.x, y: chipPos.y }; // default: fade in place
        if (commit && lastParams !== null && lastParams.mode === "reparent" && rowGeom !== null) {
          const i = rowGeom.ids.indexOf(lastParams.targetId);
          if (i >= 0) {
            exit = {
              x: scRect.left + 24,
              y: (rowGeom.tops[i]! + rowGeom.bottoms[i]!) / 2 - sc.scrollTop + scRect.top - 10,
            };
          }
        } else if (commit && lastReorderGap !== null) {
          exit = {
            x: scRect.left + 24,
            y: gapContentY(geom, lastReorderGap) - sc.scrollTop + scRect.top + liftedH / 2 - 10,
          };
        }
        setDragChip((c) => (c === null ? null : { ...c, exit }));
        window.setTimeout(() => setDragChip(null), CHIP_EXIT_MS + 40);
      }
      // Swallow the trailing synthetic click, but only briefly. If
      // the drag ended over a DIFFERENT row (the common reparent/reorder case)
      // or empty space, no synthetic click fires to consume the flag — so
      // clear it on the next macrotask instead of letting it eat the user's
      // next, unrelated click on some other row.
      draggedRef.current = true;
      window.setTimeout(() => { draggedRef.current = false; }, 0);
      if (commit) {
        // A single-drag reparent goes through emitters/drop (the host
        // re-selects the moved emitter so the highlight follows). Every reorder
        // — single root OR multi block — goes through reorder-many, whose
        // newIds re-select the moved roots (the highlight follows them).
        if (lastParams !== null) {
          void bridge.request({ kind: "emitters/drop", params: lastParams });
        } else if (lastReorderGap !== null) {
          void reorderManyEmitters(bridge, blockIds, lastReorderGap);
        }
      }
    };
    const onUp = (ev: PointerEvent) => { if (ev.pointerId !== pointerId) return; finish(true); };
    const onCancel = (ev: PointerEvent) => { if (ev.pointerId !== pointerId) return; finish(false); };
    // Focus loss can swallow the pointerup entirely (the up happens
    // off-window, or the OS steals the pointer). Without these the gesture
    // would stay armed — dims/gap/chip frozen, rAF looping, the next stray
    // click committing the abandoned drop. visibilitychange covers tab hide.
    const onBlur = () => finish(false);
    const onVis = () => { if (document.visibilityState === "hidden") finish(false); };
    // Capture-phase so we win over the row's Radix context menu and
    // the tree's own key handler; stopPropagation keeps the menu from opening.
    const onKey = (ev: KeyboardEvent) => {
      if (ev.key !== "Escape") return;
      ev.preventDefault();
      ev.stopPropagation();
      finish(false);
    };
    const onCtx = (ev: MouseEvent) => {
      ev.preventDefault();
      ev.stopPropagation();
      finish(false);
    };

    document.addEventListener("pointermove", onMove);
    document.addEventListener("pointerup", onUp);
    document.addEventListener("pointercancel", onCancel);
  };

  // Hovering a LINKED row lights up its whole group — member rows
  // tint when `linkHover` is true. `hoveredLinkGroup` is the group currently
  // hovered (null = none). The hover signal comes from member rows via
  // onHoverLinkGroup.
  const [hoveredLinkGroup, setHoveredLinkGroup] = useState<number | null>(null);

  // Dissolve a whole link group in one action. Gather every member
  // of `groupId` from the live flat list and unlink them all with a single
  // `set-membership {groupId:null}` — the host's per-target LeaveLinkGroup
  // (+ auto-dissolve of the last pair) unwinds the group under one
  // captureUndo, so a single Ctrl+Z restores it. Reads the live flatRows
  // at call time, never a cached id list.
  const handleDissolveLinkGroup = useCallback(
    (groupId: number) => {
      if (groupId === 0) return;
      const ids = flatRows
        .filter((r) => r.node.linkGroup === groupId)
        .map((r) => r.node.id);
      if (ids.length === 0) return;
      void bridge.request({
        kind: "linkGroups/set-membership",
        params: { ids, groupId: null },
      });
    },
    [flatRows, bridge],
  );

  // Clicking a link-group bracket selects every member of that group
  // (replace, primary = the top-most member). Mirrors the dissolve
  // enumeration; reads live flatRows. Syncs the new primary to the host.
  const handleSelectLinkGroup = useCallback(
    (groupId: number) => {
      if (groupId === 0) return;
      const ids = flatRows
        .filter((r) => r.node.linkGroup === groupId)
        .map((r) => r.node.id);
      if (ids.length === 0) return;
      useEmitterSelectionStore.getState().setIds(ids, ids[0]);
      void bridge.request({ kind: "emitters/select", params: { id: ids[0] } });
    },
    [flatRows, bridge],
  );

  const treeScrollRef = useRef<HTMLDivElement | null>(null);

  // ── Marquee (rubber-band) selection ──────────────────────
  // A primary-button drag starting on EMPTY space inside the scroll
  // viewport (not on a row) sweeps a rectangle; every emitter row it
  // intersects becomes the selection. Ctrl/Cmd makes it additive (union
  // with the prior selection); Esc cancels and restores. Mirrors the
  // legacy EmitterList marquee; uses live intersection rather than
  // the legacy sticky-hit accumulation (the more predictable behaviour).
  // Document-level move/up listeners (no pointer capture needed) so a drag
  // that leaves the viewport still tracks.
  const [marqueeBox, setMarqueeBox] = useState<
    { left: number; top: number; width: number; height: number } | null
  >(null);
  // `mergeBase` is what swept rows union with (the prior selection only when
  // additive); `prior` is always the pre-marquee selection, restored on Esc.
  const marqueeRef = useRef<
    { mergeBase: number[]; prior: number[]; startX: number; startY: number } | null
  >(null);

  const handleScrollPointerDown = useCallback(
    (e: React.PointerEvent<HTMLDivElement>) => {
      if (e.button !== 0) return;
      const target = e.target as HTMLElement;
      // A press on a row (or any interactive control) belongs to that row's
      // click-select / drag-reorder, not the marquee.
      if (target.closest("[data-emitter-id]") !== null) return;
      if (target.closest("input,button,[role='button']") !== null) return;
      e.preventDefault();
      const prior = [...useEmitterSelectionStore.getState().ids];
      const additive = e.ctrlKey || e.metaKey;
      const mergeBase = additive ? prior : [];
      marqueeRef.current = { mergeBase, prior, startX: e.clientX, startY: e.clientY };

      const onMove = (ev: PointerEvent) => {
        const m = marqueeRef.current;
        if (m === null) return;
        const mq = rectFromPoints(m.startX, m.startY, ev.clientX, ev.clientY);
        const rows = [...document.querySelectorAll("[data-emitter-id]")].map((el) => {
          const r = el.getBoundingClientRect();
          return {
            id: Number((el as HTMLElement).dataset.emitterId),
            rect: { left: r.left, top: r.top, right: r.right, bottom: r.bottom },
          };
        });
        const swept = emittersInMarquee(rows, mq);
        const { ids, primary } = mergeMarqueeSelection(m.mergeBase, swept);
        useEmitterSelectionStore.getState().setIds(ids, primary);
        const sc = treeScrollRef.current;
        if (sc !== null) {
          const cr = sc.getBoundingClientRect();
          setMarqueeBox({
            left: mq.left - cr.left + sc.scrollLeft,
            top: mq.top - cr.top + sc.scrollTop,
            width: mq.right - mq.left,
            height: mq.bottom - mq.top,
          });
        }
      };
      const cleanup = () => {
        document.removeEventListener("pointermove", onMove);
        document.removeEventListener("pointerup", onUp);
        document.removeEventListener("keydown", onKey, true);
        marqueeRef.current = null;
        setMarqueeBox(null);
      };
      const onUp = () => cleanup();
      const onKey = (ev: KeyboardEvent) => {
        if (ev.key !== "Escape") return;
        const m = marqueeRef.current;
        if (m !== null) {
          const primary = m.prior.length > 0 ? m.prior[m.prior.length - 1]! : null;
          useEmitterSelectionStore.getState().setIds(m.prior, primary);
        }
        ev.preventDefault();
        ev.stopPropagation();
        cleanup();
      };
      document.addEventListener("pointermove", onMove);
      document.addEventListener("pointerup", onUp);
      document.addEventListener("keydown", onKey, true);
    },
    [],
  );

  // ── keyboard handler ─────────────────────────────────
  //
  // Routes via the focused row's `data-emitter-id`. The tree container
  // is `tabIndex={0}` so it can hold focus when no row is focused
  // (initial-load case). Arrow/Home/End shift focus; Enter/F2/Delete/
  // Ctrl+C/X/V fire actions. Keystrokes targeting an `<input>` are
  // never intercepted — the inline-rename input stops propagation on
  // its own onKeyDown anyway, but the tagName guard is the safety net.
  //
  // Focus is shifted by querying the rendered DOM for the target row's
  // button and calling `.focus()` on it. The button is the actual
  // focus target (the container's tabIndex just lets users tab INTO
  // the tree); arrow nav within the tree always lands on a row button.
  const treeContainerRef = useRef<HTMLDivElement | null>(null);

  const focusRowById = useCallback((id: number) => {
    if (treeContainerRef.current === null) return;
    const btn = treeContainerRef.current.querySelector(
      `button[data-emitter-id="${id}"]`,
    ) as HTMLButtonElement | null;
    btn?.focus();
  }, []);

  const handleTreeKeyDown = useCallback(
    (e: React.KeyboardEvent<HTMLDivElement>) => {
      // Never steal keystrokes when the focus is in a text input
      // (inline-rename, spinners, modal text fields that might bubble).
      const target = e.target as HTMLElement | null;
      if (target !== null && target.tagName === "INPUT") return;
      // Editing mode disables the global keyboard nav so the input
      // owns all keys. (The input's onKeyDown stops propagation too;
      // belt + braces.)
      if (editingRef.current !== null) return;

      // Resolve the focused row id from the active element's
      // `data-emitter-id`, falling back to the React-side primary so
      // the first keypress lands somewhere sensible.
      const active = document.activeElement as HTMLElement | null;
      const activeIdStr = active?.getAttribute("data-emitter-id") ?? null;
      const focusedId = activeIdStr !== null ? Number.parseInt(activeIdStr, 10) : primaryId;
      const focusedIdx = focusedId !== null ? orderedIds.indexOf(focusedId) : -1;

      // Helpers — used by multiple branches below.
      const moveFocus = (nextIdx: number) => {
        if (nextIdx < 0 || nextIdx >= orderedIds.length) return;
        const nextId = orderedIds[nextIdx]!;
        e.preventDefault();
        useEmitterSelectionStore.getState().setSingle(nextId);
        void bridge.request({
          kind: "emitters/select",
          params: { id: nextId },
        });
        focusRowById(nextId);
      };

      if (e.key === "ArrowDown") {
        moveFocus(focusedIdx + 1);
        return;
      }
      if (e.key === "ArrowUp") {
        moveFocus(focusedIdx - 1);
        return;
      }
      if (e.key === "Home") {
        moveFocus(0);
        return;
      }
      if (e.key === "End") {
        moveFocus(orderedIds.length - 1);
        return;
      }
      if (e.key === "F2") {
        if (focusedId === null) return;
        const node = flatRows.find((r) => r.node.id === focusedId)?.node ?? null;
        if (node === null) return;
        e.preventDefault();
        beginEdit(focusedId, node.name);
        return;
      }
      if (e.key === "Delete") {
        const cur = useEmitterSelectionStore.getState().ids;
        if (cur.length === 0) return;
        e.preventDefault();
        // Descending-order delete + the destructive-confirm gate both live in
        // requestDeleteEmitters → performDelete now.
        requestDeleteEmitters(bridge, [...cur]);
        return;
      }
      // Ctrl+C / Ctrl+X / Ctrl+V on the focused tree. Cmd+* on macOS
      // routes through metaKey, same handler.
      const mod = e.ctrlKey || e.metaKey;
      if (mod && (e.key === "c" || e.key === "C")) {
        const cur = useEmitterSelectionStore.getState().ids;
        if (cur.length === 0) return;
        e.preventDefault();
        void bridge.request({ kind: "emitters/copy", params: { ids: cur } });
        markEmittersCopied();
        return;
      }
      if (mod && (e.key === "x" || e.key === "X")) {
        const cur = useEmitterSelectionStore.getState().ids;
        if (cur.length === 0) return;
        e.preventDefault();
        void bridge.request({ kind: "emitters/cut", params: { ids: cur } });
        markEmittersCopied();
        return;
      }
      if (mod && (e.key === "v" || e.key === "V")) {
        e.preventDefault();
        void bridge.request({ kind: "emitters/paste", params: {} });
        return;
      }
    },
    [bridge, beginEdit, flatRows, focusRowById, orderedIds, primaryId],
  );

  return (
    <div
      ref={treeContainerRef}
      data-testid="emitter-tree"
      data-selected-count={selectedIds.length}
      data-primary-id={primaryId ?? ""}
      data-dragging-id={draggingId ?? ""}
      data-editing-id={editing?.id ?? ""}
      tabIndex={0}
      onKeyDown={handleTreeKeyDown}
      // [glide] focus-restore bookkeeping (see treeHadFocusRef): removal of a
      // focused element fires no blur, so this flag is the only record that
      // the tree owned focus when a remount dropped it.
      onFocusCapture={() => { treeHadFocusRef.current = true; }}
      onBlurCapture={(e) => {
        if (!e.currentTarget.contains(e.relatedTarget as Node | null)) {
          treeHadFocusRef.current = false;
        }
      }}
      className="flex h-full flex-col outline-none"
    >
      {tree !== null && <SystemLoadChip bridge={bridge} systemLoad={systemLoad} />}
      {tree === null ? (
        <div className="flex-1 min-h-0 text-text-3 text-sm">(loading…)</div>
      ) : rootChildren.length === 0 ? (
        <div className="flex-1 min-h-0 text-text-3 text-sm">(no emitters)</div>
      ) : (
        // Wrap the <ul> in a relative-positioned container so the
        // bracket gutter (absolute, right-aligned) can stack alongside.
        // This container is the scroll viewport now —
        // `flex-1 min-h-0 overflow-y-auto` so long emitter lists scroll
        // inside it while EmitterTreeToolbar (sibling below) stays
        // pinned at the pane's bottom.
        <div
          ref={treeScrollRef}
          onPointerDown={handleScrollPointerDown}
          className="emitter-tree-scroll relative flex flex-1 min-h-0 overflow-y-auto"
        >
          <ul
            role="tree"
            aria-label="Emitters"
            className="m-0 flex-1 list-none p-0"
          >
          {flatRows.map((row) => {
            // "make room" gap: a flow spacer (the lifted block's measured
            // height) at the resolved root gap, so the rows shift to reveal
            // where the dragged emitter(s) will land. Used by BOTH single-root
            // and multi reorder. Gap g renders before root g's row; the end gap
            // (g = N) renders after the whole list (below this map).
            const showGap =
              indicator?.kind === "gap" &&
              indicator.gapIndex < rootChildren.length &&
              rootChildren[indicator.gapIndex]!.id === row.node.id;
            const gap = showGap ? (
              <li
                aria-hidden
                role="presentation"
                data-testid={`drop-gap-at-${indicator.gapIndex}`}
                // ring-inset: render the ring INSIDE the element so it isn't
                // clipped by the scroll container's overflow at the very top /
                // bottom edge of the list.
                className="pointer-events-none mx-0.5 rounded bg-accent-soft ring-1 ring-inset ring-accent"
                style={{ height: `${indicator.gapHeight}px` }}
              />
            ) : null;
            return (
              // [glide] keyed by stableId so a reorder MOVES row elements
              // (FLIP can animate them) instead of remounting per-position.
              <Fragment key={row.node.stableId}>
                {gap}
                <EmitterRow
                  row={row}
                  primaryId={primaryId}
                  selectedIds={selectedIds}
                  orderedIds={orderedIds}
                  onRowClick={handleRowClick}
                  bridge={bridge}
                  draggingId={draggingId}
                  draggingIds={draggingIds}
                  indicator={indicator}
                  startDrag={startDrag}
                  editing={editing}
                  beginEdit={beginEdit}
                  setEditValue={setEditValue}
                  commitEdit={commitEdit}
                  cancelEdit={cancelEdit}
                  linkHover={
                    hoveredLinkGroup !== null &&
                    row.node.linkGroup === hoveredLinkGroup
                  }
                  onHoverLinkGroup={setHoveredLinkGroup}
                  onDissolveLinkGroup={handleDissolveLinkGroup}
                  onSelectLinkGroup={handleSelectLinkGroup}
                  chainWarning={chainWarnings.get(row.node.stableId) ?? null}
                />
              </Fragment>
            );
          })}
          {/* end gap (g = N): after the LAST root's whole subtree — the very
              bottom of the list. */}
          {indicator?.kind === "gap" && indicator.gapIndex === rootChildren.length && (
            <li
              aria-hidden
              role="presentation"
              data-testid={`drop-gap-at-${indicator.gapIndex}`}
              className="pointer-events-none mx-0.5 rounded bg-accent-soft ring-1 ring-inset ring-accent"
              style={{ height: `${indicator.gapHeight}px` }}
            />
          )}
          </ul>
          {/* Marquee (rubber-band) selection rectangle. */}
          {marqueeBox !== null && (
            <div
              data-testid="emitter-marquee"
              aria-hidden
              className="pointer-events-none absolute border border-accent bg-accent/15"
              style={{
                left: marqueeBox.left,
                top: marqueeBox.top,
                width: marqueeBox.width,
                height: marqueeBox.height,
              }}
            />
          )}
        </div>
      )}
      <EmitterTreeToolbar bridge={bridge} tree={tree} primaryId={primaryId} />
      {/* [multi-drag] Cursor chip — a small fixed-position card during a
          multi-root drag: up to 4 dragged names (tree order) + a "+k more"
          line. Its position is the magnetized spring state (computeChipTarget
          + per-frame easing): anchored at the pointer, pulled toward the
          active gap so the emitters read as flowing into it. Uses this file's
          floating-surface vocabulary (bg-bg-2 + shadow-xl) with the sky-400
          drag accent to match the gap affordance. */}
      {dragChip && (
        <div
          data-testid="drag-chip"
          data-exiting={dragChip.exit ? "true" : "false"}
          aria-hidden
          // drag-chip-enter: pop-in on spawn (components.css; reduced-motion
          // disables it). Exit mode overrides position with the landing spot
          // + fades/shrinks via an inline transition — see finish().
          className="drag-chip-enter pointer-events-none fixed z-50 rounded-md border border-accent bg-bg-2/95 px-2 py-1 text-xs text-accent shadow-[var(--shadow-soft)]"
          style={
            dragChip.exit
              ? {
                  left: dragChip.exit.x,
                  top: dragChip.exit.y,
                  opacity: 0,
                  transform: "scale(0.85)",
                  transition:
                    `left ${CHIP_EXIT_MS}ms ease-in, top ${CHIP_EXIT_MS}ms ease-in, ` +
                    `opacity ${CHIP_EXIT_MS}ms ease-in, transform ${CHIP_EXIT_MS}ms ease-in`,
                }
              : { left: dragChip.x, top: dragChip.y }
          }
        >
          {dragChip.names.slice(0, 4).map((name, i) => (
            <div key={i} className="truncate px-2 leading-5">
              {name}
            </div>
          ))}
          {dragChip.names.length > 4 && (
            <div className="px-2 leading-5 text-text-3">
              +{dragChip.names.length - 4} more
            </div>
          )}
        </div>
      )}
    </div>
  );
}
