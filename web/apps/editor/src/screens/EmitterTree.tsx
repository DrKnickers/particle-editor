// EmitterTree — sidebar tree of the live ParticleSystem's emitters.
//
// Phase 3 Screen 4 Batch A: read-only render + single-select.
// Phase 3 Screen 4 Batch B1: right-click context menu + 4 modal dialogs
//                            for Rename/Duplicate/Delete/Increment/
//                            Rescale/LinkGroupSettings.
// Phase 3 Screen 4 Batch B2: Add Lifetime/Death Child, Move Up/Down,
//                            Set Link Group… / Leave Link Group, plus
//                            React-side multi-select (Ctrl/Cmd + Shift
//                            + plain click).
// Phase 3 Screen 4 Batch B3: HTML5 drag/drop reorder + reparent.
// Phase 3 Screen 4 Batch C : Link-group bracket gutter + inline rename
//                            (F2 / dbl-click / context-menu Rename;
//                            replaces B1's modal — `RenameEmitterDialog`
//                            is deleted) + keyboard nav (arrows / Home
//                            / End / Enter / F2 / Delete / Ctrl+C/X/V).
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
// `linkGroup !== 0`. The full coloured-bracket visualization (
// port) renders in the right gutter (Batch C); the dot itself stays
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
  useState,
  type ComponentProps,
} from "react";
import * as ContextMenu from "@radix-ui/react-context-menu";
import * as Menubar from "@radix-ui/react-menubar";
import { ChevronDown, ChevronUp, Copy, Eye, EyeOff, Plus, Trash2 } from "lucide-react";
import { useViewportOcclusion } from "@/lib/viewport-occlusion";
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
import {
  computeDropZone,
  computeRootGapIndex,
  isDescendant,
  resolveReparentSlot,
  type DropZone,
} from "@/lib/drop-zone";
import { computeLinkGroupBrackets, colorForGroup } from "@/lib/link-group-colors";
import { useEmitterTreeStore } from "@/lib/emitter-tree";
import { requestDeleteEmitters } from "@/lib/delete-emitters";
import { moveEmitters, duplicateEmitters } from "@/lib/emitter-reorder";
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
// being hovered; `zone` is which third of the row's rect the cursor is
// in. `null` means no active drag-over.
type DropIndicator = { targetId: number; zone: DropZone } | null;

// Validated parameters for the `emitters/drop` bridge call — the output
// of resolveDropIntent. `null` means the drop is refused.
type DropParams =
  | { mode: "reparent"; id: number; targetId: number; slot: "lifetime" | "death" }
  | { mode: "reorder"; id: number; rootIndex: number };

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
 *  This was Batch B3's inline `resolveDropIntent`; lifted to a pure fn so
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

// Inline-rename state (Batch C). `editing.id` is the row currently in
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
  indicator: DropIndicator;
  startDrag: (node: EmitterTreeNode, e: React.PointerEvent) => void;
  // Batch C — inline rename. `editing.id === node.id` means this row
  // renders an `<input>` instead of the label span. `beginEdit` starts
  // a new rename session against this row; `setEditValue` updates the
  // live value; `commitEdit` / `cancelEdit` end the session.
  editing: RenameEditingState;
  beginEdit: (id: number, currentName: string) => void;
  setEditValue: (value: string) => void;
  commitEdit: () => void;
  cancelEdit: () => void;
  //: true when this row's link group is the one currently hovered —
  // the row paints a subtle tint so the user sees the whole group light up
  // together. `onHoverLinkGroup` reports this row's group on pointer enter
  // (null on leave) so the parent can drive the tint + bracket highlight.
  linkHover: boolean;
  onHoverLinkGroup: (groupId: number | null) => void;
  //: dissolve the entire link group this row belongs to.
  onDissolveLinkGroup: (groupId: number) => void;
};

// (Group A): mirror of MenuBar's OccludingMenubarContent for
// ContextMenu. When the row's context menu mounts, we register its
// bounding rect as a viewport occlusion so the AlphaCompositor's
// alpha stamp opens a hole in the layered viewport popup at that
// location — without it the right side of the menu gets overpainted
// by the popup wherever it crosses the viewport rect.
function OccludingContextMenuContent({
  bridge,
  occlusionId,
  children,
  ...rest
}: ComponentProps<typeof ContextMenu.Content> & {
  bridge: Bridge;
  occlusionId: string;
}) {
  const ref = useRef<HTMLDivElement | null>(null);
  // pad=24, feather=24 — matches the menubar dropdown's shadow-xl ring
  // so the popup alpha smoothly transitions from full-viewport at the
  // padded outer edge to full-cut at the menu's actual outline.
  useViewportOcclusion(bridge, occlusionId, ref, 24, 24);
  return (
    <ContextMenu.Content
      className="z-50 min-w-[220px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-xl"
      {...rest}
    >
      <div ref={ref}>{children}</div>
    </ContextMenu.Content>
  );
}

// A submenu (e.g. "Paste As ▸") renders in its OWN portal at a different
// screen location than its parent menu, so it needs its OWN viewport
// occlusion rect — otherwise the layered D3D viewport popup overpaints the
// part of the submenu that crosses the viewport, exactly as it would the
// top-level menu without OccludingContextMenuContent. Same pad/feather.
function OccludingContextSubContent({
  bridge,
  occlusionId,
  children,
  ...rest
}: ComponentProps<typeof ContextMenu.SubContent> & {
  bridge: Bridge;
  occlusionId: string;
}) {
  const ref = useRef<HTMLDivElement | null>(null);
  useViewportOcclusion(bridge, occlusionId, ref, 24, 24);
  return (
    <ContextMenu.SubContent
      className="z-50 min-w-[200px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-xl"
      {...rest}
    >
      <div ref={ref}>{children}</div>
    </ContextMenu.SubContent>
  );
}

function EmitterRow({
  row, primaryId, selectedIds, orderedIds, onRowClick, bridge,
  draggingId, indicator, startDrag,
  editing, beginEdit, setEditValue, commitEdit, cancelEdit,
  linkHover, onHoverLinkGroup, onDissolveLinkGroup,
}: RowProps) {
  const { node, depth, siblings } = row;
  const isPrimary = primaryId === node.id;
  const isSelected = selectedIds.includes(node.id);
  const isLinked = node.linkGroup !== 0;
  const isEditing = editing !== null && editing.id === node.id;
  // Context-menu Paste gates on session clipboard content ().
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
    // Batch C: context-menu Rename starts inline edit instead of
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
  // Context-menu clipboard () + New Root () — reuse the same
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

  // ── Batch B3 — drag/drop handlers ────────────────────────────────
  //
  // The row is both a drag source and a drop target. Drop semantics:
  //   - drop on a root row, upper third  → reorder above target root
  //   - drop on a root row, lower third  → reorder below target root
  //   - drop on any row,  middle third  → reparent under target
  // The drag itself is driven by the parent's pointer-drag controller
  // (startDrag, wired to this row's button below); the row only renders
  // the drop indicator from `indicator` and initiates on pointerdown.

  const isThisRowIndicator = indicator?.targetId === node.id;
  const indicatorZone = isThisRowIndicator ? indicator!.zone : null;

  // Reparent target visual: tint the row + ring.
  const reparentTintClass = indicatorZone === "onto"
    ? "bg-accent-soft ring-1 ring-sky-400"
    : "";

  const menuItemClass =
    "flex cursor-pointer items-center rounded px-2 py-1 text-xs text-text outline-none data-[disabled]:cursor-not-allowed data-[disabled]:text-text-3 data-[highlighted]:bg-panel-2";
  const separatorClass =
    "my-1 h-px bg-panel-2";

  // Selected-row styling (Batch B2):
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
    <li role="treeitem" aria-selected={isSelected} className="relative">
      {/* Insertion line: 2px sky-400 bar at the top or bottom of the row
          during dragover for the reorder zones. Absolute-positioned so
          it doesn't perturb the row's layout. */}
      {indicatorZone === "above" && (
        <div
          data-testid={`drop-indicator-above-${node.id}`}
          className="pointer-events-none absolute left-0 right-0 top-0 z-10 h-0.5 bg-accent"
        />
      )}
      {indicatorZone === "below" && (
        <div
          data-testid={`drop-indicator-below-${node.id}`}
          className="pointer-events-none absolute left-0 right-0 bottom-0 z-10 h-0.5 bg-accent"
        />
      )}
      <ContextMenu.Root>
        <ContextMenu.Trigger asChild>
          <button
            type="button"
            onPointerDown={(e) => startDrag(node, e)}
            //: hovering a linked row lights up its whole group.
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
            data-link-group={node.linkGroup}
            data-link-hover={linkHover ? "true" : "false"}
            data-selected={isSelected ? "true" : "false"}
            data-primary={isPrimary ? "true" : "false"}
            data-drop-zone={indicatorZone ?? ""}
            data-dragging={draggingId === node.id ? "true" : "false"}
            className={[
              "grid w-full items-center gap-1.5 py-0.5 pr-2 text-left text-sm transition-colors",
              "border-l-2",
              borderClass,
              rowBgClass,
              reparentTintClass,
              fontClass,
              //: hovering a group's bracket tints its member rows.
              linkHover ? "bg-accent/10" : "",
              node.visible ? "" : "opacity-50",
              draggingId === node.id ? "opacity-50" : "",
            ].join(" ")}
            style={{
              paddingLeft: `${8 + indentPx}px`,
              // Visual columns: [eye | role-glyph | link-dot | name]. The
              // role glyph (children only) sits in col 2; the link dot
              // (linked rows only) reserves col 3 on EVERY row so names stay
              // left-aligned whether linked or not. DOM order stays
              // [eye, label, role, dot] — glyph/label/dot are placed VISUALLY
              // via grid-column below, and the dot is aria-hidden — so the
              // accessibility tree (and the emitter-tree a11y goldens, which
              // capture "default ↻" + the row's accessible name) are
              // unchanged. Eye auto-places into column 1.
              gridTemplateColumns: "18px 18px 10px 1fr",
            }}
          >
            {/* F1: visibility toggle on the LEFT (replaces the old role
                dot). Always rendered so the grid columns stay stable
                during inline rename. */}
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
              title={node.visible ? "Hide emitter" : "Show emitter"}
              aria-label={node.visible ? "Hide emitter" : "Show emitter"}
              className="grid place-items-center w-4 h-4 shrink-0 rounded text-text-3 hover:bg-panel-2 hover:text-text cursor-pointer"
            >
              {node.visible
                ? <Eye className="size-3" />
                : <EyeOff className="size-3" />}
            </span>
            {isEditing ? (
              // Inline-rename input. Stops click + drag propagation so
              // typing doesn't accidentally re-trigger row selection /
              // drag-start. Commit on Enter, cancel on Esc; blur also
              // commits. Empty value reverts on commit (handled in the
              // parent's `commitEdit`).
              <input
                ref={inputRef}
                style={{ gridColumn: 4, gridRow: 1 }}
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
                className="truncate"
                data-emitter-name
                style={{ gridColumn: 4, gridRow: 1 }}
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
            {/* Spawn-role glyph for child emitters (lifetime ↻ / on-death ✕),
                placed VISUALLY in column 2 (between the eye and the label)
                via grid-column. Rendered last in DOM so the accessible name
                stays "…default lifetime child" and the goldens are stable.
                Root rows omit it; column 2 then sits empty and the label
                stays in column 3. */}
            {node.role !== "root" && (
              <span
                aria-label={roleLabel(node.role)}
                style={{ gridColumn: 2, gridRow: 1 }}
                className="inline-block w-full shrink-0 text-center font-mono text-xs text-text-3"
              >
                {roleGlyph(node.role)}
              </span>
            )}
            {/*: per-row "is-linked" dot, placed in col 3 (left of the
                name). Decorative (aria-hidden) so the accessible name — and
                the emitter-tree a11y goldens — stay unchanged. Coloured to
                MATCH this row's bracket (colorForGroup), so the dot and the
                gutter bracket read as the same group at a glance. */}
            {isLinked && (
              <span
                aria-hidden
                data-testid={`emitter-link-dot-${node.id}`}
                style={{
                  gridColumn: 3,
                  gridRow: 1,
                  background: colorForGroup(node.linkGroup) ?? undefined,
                }}
                className="pointer-events-none size-1.5 justify-self-center rounded-full"
              />
            )}
          </button>
        </ContextMenu.Trigger>
        <ContextMenu.Portal>
          <OccludingContextMenuContent
            bridge={bridge}
            occlusionId="context-menu:emitter-tree"
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
                <OccludingContextSubContent
                  bridge={bridge}
                  occlusionId="context-menu:emitter-tree:paste-as"
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
                </OccludingContextSubContent>
              </ContextMenu.Portal>
            </ContextMenu.Sub>
            <ContextMenu.Separator className={separatorClass} />
            <ContextMenu.Item onSelect={handleIncrement} className={menuItemClass}>
              Increment Index…
            </ContextMenu.Item>
            <ContextMenu.Item onSelect={handleRescale} className={menuItemClass}>
              Rescale Emitter…
            </ContextMenu.Item>
            {/* ─── Batch B2 additions ───────────────────────────── */}
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
          </OccludingContextMenuContent>
        </ContextMenu.Portal>
      </ContextMenu.Root>
    </li>
  );
}

// Row-height for the bracket gutter math. Matches the `py-1`+`text-sm`
// row styling — empirically ~24px in the current theme. Static so the
// bracket layer can lay out absolutely without per-render measurement
// (a ResizeObserver pass adds complexity for a polish detail; if the
// tree theme changes this constant moves with it).
const ROW_HEIGHT_PX     = 20;
const LANE_WIDTH_PX     = 10;  // 2px bracket + 8px gap to next lane
// Gap between the longest emitter name's right edge and the first bracket
// lane. The bracket layer is absolutely positioned at (measured longest-name
// right + this gap) so the brackets hug the names instead of sitting at the
// panel's far-right edge. See the measure effect in EmitterTree.
const BRACKET_NAME_GAP_PX = 16;

// ─── Panel-header toolbar ────────────────────────────────────────────
// (Group A polish): restore the legacy panel toolbar from
// src/UI/EmitterList.cpp:3016. Layout matches legacy ordering:
//   [New ▾] [Delete] [▲ Move Up] [▼ Move Down]   (this batch)
//   [👁]    [Show All] [Hide All]                (next batch, T2/T3)
// All four buttons here use bridge calls that already exist in the
// schema — no host-side work needed.

// F2: 28px square to match the main toolbar's `.tb-btn`. F3: `:active`
// pressed state (lighter bg + slight scale) via Tailwind `active:`,
// suppressed while disabled.
const TOOLBAR_BTN =
  "flex h-7 w-7 items-center justify-center rounded text-text-2 transition hover:bg-panel-2 hover:text-text active:bg-panel-3 active:scale-95 disabled:cursor-not-allowed disabled:text-text-3 disabled:hover:bg-transparent disabled:active:scale-100 outline-none";

const NEW_EMITTER_MENU_ITEM =
  "flex select-none items-center gap-2 rounded px-2 py-1 text-xs text-text hover:bg-panel-2 focus:bg-panel-2 outline-none cursor-pointer data-[disabled]:text-text-3 data-[disabled]:cursor-not-allowed data-[disabled]:hover:bg-transparent";

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
          <Menubar.Trigger
            className={TOOLBAR_BTN}
            title="New Emitter"
            aria-label="New Emitter"
          >
            <Plus className="size-4" />
          </Menubar.Trigger>
          <Menubar.Portal>
            <Menubar.Content
              className="min-w-[160px] rounded-md border border-border bg-bg-2 p-1 shadow-xl z-50"
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
      <button
        type="button"
        className={TOOLBAR_BTN}
        title="Duplicate"
        aria-label="Duplicate emitter"
        disabled={!hasPrimary}
        onClick={duplicatePrimary}
      >
        <Copy className="size-4" />
      </button>
      <button
        type="button"
        className={TOOLBAR_BTN}
        title="Delete"
        aria-label="Delete emitter"
        disabled={!hasPrimary}
        onClick={del}
      >
        <Trash2 className="size-4" />
      </button>
      <button
        type="button"
        className={TOOLBAR_BTN}
        title="Move Up"
        aria-label="Move emitter up"
        disabled={!canMoveUp}
        onClick={moveUp}
      >
        <ChevronUp className="size-4" />
      </button>
      <button
        type="button"
        className={TOOLBAR_BTN}
        title="Move Down"
        aria-label="Move emitter down"
        disabled={!canMoveDown}
        onClick={moveDown}
      >
        <ChevronDown className="size-4" />
      </button>
      <button
        type="button"
        className={TOOLBAR_BTN}
        title="Show All Emitters"
        aria-label="Show all emitters"
        onClick={showAll}
      >
        <Eye className="size-4" />
      </button>
      <button
        type="button"
        className={TOOLBAR_BTN}
        title="Hide All Emitters"
        aria-label="Hide all emitters"
        onClick={hideAll}
      >
        <EyeOff className="size-4" />
      </button>
    </div>
  );
}

export function EmitterTree({ bridge }: Props) {
  const tree = useEmitterTreeStore((s) => s.tree);
  const setTree = useEmitterTreeStore((s) => s.setTree);
  const selectedIds = useEmitterSelectionIds();
  const primaryId = useEmitterSelectionPrimary();

  // Batch B3 — drag/drop state. `draggingId` is the source row's id;
  // `indicator` is the row + zone currently displaying a drop hint.
  // Both lifted to the tree level so only one indicator can be active
  // and so rows can read the dragged node's subtree for cycle checks.
  const [draggingId, setDraggingId] = useState<number | null>(null);
  const [indicator, setIndicator] = useState<DropIndicator>(null);
  // [pointer-drag] Set true when a real drag completes so the synthetic
  // click that follows pointerup (when down+up land on the same row) does
  // NOT also fire row selection. Reset on the next pointerdown and in
  // handleRowClick. (B3's HTML5 DnD is replaced by pointer events because
  // dragstart never fires under composition hosting — WebView2 is a
  // composition visual with no HWND for the OS drag loop.)
  const draggedRef = useRef(false);

  // Batch C — inline rename. Local component state because only the
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
  // selection arrives from outside (legacy --legacy-ui edit, devtools
  // poke, post-mutation cleanup).
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

  // Phase 4.1 Fix dispatch 5 — subscribe to menu-driven rename
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
  // (Batch B3) never initiates under composition hosting (WebView2
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
    draggedRef.current = false;
    const startX = e.clientX;
    const startY = e.clientY;
    let lastX = startX;
    let lastY = startY;
    const curTree = tree;
    const curRoots = rootChildren;
    const curRows = flatRows;
    let active = false;
    let lastParams: DropParams | null = null;
    let rafId: number | null = null;
    const THRESHOLD = 4;

    // Resolve the drop intent for the given row at vertical position `clientY`
    // and reflect it in the indicator. Shared by the pointermove path (row
    // from the event target) and the autoscroll loop (row from
    // elementFromPoint — a held, stationary pointer fires no move while the
    // content scrolls under it).
    const updateDropTarget = (rowEl: HTMLElement | null, clientY: number) => {
      if (rowEl === null) {
        lastParams = null;
        setIndicator(null);
        return;
      }
      const targetId = Number(rowEl.getAttribute("data-emitter-id"));
      const target = curRows.find((r) => r.node.id === targetId)?.node ?? null;
      if (target === null) {
        lastParams = null;
        setIndicator(null);
        return;
      }
      const rect = rowEl.getBoundingClientRect();
      const zone = computeDropZone(clientY - rect.top, rect.height);
      const targetRootIdx = curRoots.findIndex((c) => c.id === targetId);
      const params = resolveDropIntent(source, target, targetRootIdx, zone, curTree, curRoots);
      lastParams = params;
      setIndicator(params !== null ? { targetId, zone } : null);
    };

    // [] While the pointer sits in an edge zone of the scroll viewport,
    // scroll it each frame (proportional to depth) and re-resolve the drop
    // target so the indicator follows the rows moving under the pointer.
    const tick = () => {
      const sc = treeScrollRef.current;
      if (sc !== null) {
        const delta = computeAutoscrollDelta(lastY, sc.getBoundingClientRect());
        if (delta !== 0) {
          sc.scrollTop += delta;
          const rowEl =
            (document.elementFromPoint(lastX, lastY)?.closest?.("[data-emitter-id]") as
              | HTMLElement
              | null) ?? null;
          updateDropTarget(rowEl, lastY);
        }
      }
      rafId = requestAnimationFrame(tick);
    };

    const onMove = (ev: PointerEvent) => {
      lastX = ev.clientX;
      lastY = ev.clientY;
      if (!active) {
        if (Math.abs(ev.clientX - startX) + Math.abs(ev.clientY - startY) < THRESHOLD) {
          return;
        }
        active = true;
        setDraggingId(source.id);
        // [] Esc / right-click cancel only an ACTIVE drag — attach the
        // listeners on activation so a pre-threshold right-click still opens
        // the row context menu. [] start the autoscroll loop.
        document.addEventListener("keydown", onKey, true);
        document.addEventListener("contextmenu", onCtx, true);
        rafId = requestAnimationFrame(tick);
      }
      const rowEl =
        ((ev.target as Element | null)?.closest?.("[data-emitter-id]") as HTMLElement | null) ??
        null;
      updateDropTarget(rowEl, ev.clientY);
    };

    const finish = (commit: boolean) => {
      document.removeEventListener("pointermove", onMove);
      document.removeEventListener("pointerup", onUp);
      document.removeEventListener("pointercancel", onCancel);
      document.removeEventListener("keydown", onKey, true);
      document.removeEventListener("contextmenu", onCtx, true);
      if (rafId !== null) {
        cancelAnimationFrame(rafId);
        rafId = null;
      }
      if (!active) return;
      setDraggingId(null);
      setIndicator(null);
      draggedRef.current = true; // swallow the trailing click
      if (commit && lastParams !== null) {
        void bridge.request({ kind: "emitters/drop", params: lastParams });
      }
    };
    const onUp = () => finish(true);
    const onCancel = () => finish(false);
    // [] Capture-phase so we win over the row's Radix context menu and
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

  // Bracket descriptors for the link-group gutter. One entry per group
  // with ≥2 members; each carries a dedicated lane + every member row
  // index. The renderer absolute-positions the layer so the brackets
  // hug the names (measure effect below) and draws a stub at each member.
  const brackets = useMemo(
    () => computeLinkGroupBrackets(flatRows.map((r) => r.node)),
    [flatRows],
  );

  //: hovering a LINKED row lights up its whole group — the member
  // rows tint and the gutter bracket thickens/brightens. `hoveredLinkGroup`
  // is the group currently hovered (null = none). The hover signal comes from
  // both member rows AND the bracket's own hit-zone (which is now clickable to
  // select the whole group — see the bracket render + handleSelectLinkGroup).
  const [hoveredLinkGroup, setHoveredLinkGroup] = useState<number | null>(null);

  //: dissolve a whole link group in one action. Gather every member
  // of `groupId` from the live flat list and unlink them all with a single
  // `set-membership {groupId:null}` — the host's per-target LeaveLinkGroup
  // (+ auto-dissolve of the last pair) unwinds the group under one
  // captureUndo, so a single Ctrl+Z restores it. Reads the live flatRows
  // at call time, never a cached id list (R4 mitigation).
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

  // [link-group polish] "Hug the longest name": position the bracket
  // layer at (longest visible name's right edge + gap) instead of the
  // panel's far-right edge. The 1fr name column FILLS the row, so the
  // column edge ≠ the text edge — measure each name's text node (Range),
  // cap at the column edge for truncated names, take the max. Re-measure
  // on tree change, container resize, and after web fonts settle.
  const treeScrollRef = useRef<HTMLDivElement | null>(null);
  const [bracketLeft, setBracketLeft] = useState<number | null>(null);
  useLayoutEffect(() => {
    const container = treeScrollRef.current;
    if (container === null) return;
    const measure = () => {
      const names = container.querySelectorAll<HTMLElement>("[data-emitter-name]");
      if (names.length === 0) {
        setBracketLeft(null);
        return;
      }
      const cRect = container.getBoundingClientRect();
      const range = document.createRange();
      // jsdom (vitest) doesn't implement Range.getBoundingClientRect; fall
      // back to the element rect there so the effect degrades gracefully
      // instead of throwing. Real browsers measure the text node.
      const canMeasureText = typeof range.getBoundingClientRect === "function";
      let maxRight = 0;
      names.forEach((el) => {
        let right = el.getBoundingClientRect().right;
        if (canMeasureText) {
          range.selectNodeContents(el);
          // Cap at the column's right edge so a truncated (overflowing)
          // name doesn't push the bracket past the visible text.
          right = Math.min(range.getBoundingClientRect().right, right);
        }
        const rel = right - cRect.left + container.scrollLeft;
        if (rel > maxRight) maxRight = rel;
      });
      setBracketLeft(Math.round(maxRight) + BRACKET_NAME_GAP_PX);
    };
    measure();
    const ro = new ResizeObserver(measure);
    ro.observe(container);
    void document.fonts?.ready?.then(measure).catch(() => {});
    return () => ro.disconnect();
  }, [flatRows]);

  // ── Marquee (rubber-band) selection () ──────────────────────
  // A primary-button drag starting on EMPTY space inside the scroll
  // viewport (not on a row) sweeps a rectangle; every emitter row it
  // intersects becomes the selection. Ctrl/Cmd makes it additive (union
  // with the prior selection); Esc cancels and restores. Mirrors the
  // legacy EmitterList marquee (); uses live intersection rather than
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

  // ── Batch C — keyboard handler ─────────────────────────────────
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
      className="flex h-full flex-col outline-none"
    >
      {tree === null ? (
        <div className="flex-1 min-h-0 text-text-3 text-sm">(loading…)</div>
      ) : rootChildren.length === 0 ? (
        <div className="flex-1 min-h-0 text-text-3 text-sm">(no emitters)</div>
      ) : (
        // Wrap the <ul> in a relative-positioned container so the
        // bracket gutter (absolute, right-aligned) can stack alongside.
        // B1.3.1 polish: this container is the scroll viewport now —
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
          {flatRows.map((row) => (
            <EmitterRow
              key={row.node.id}
              row={row}
              primaryId={primaryId}
              selectedIds={selectedIds}
              orderedIds={orderedIds}
              onRowClick={handleRowClick}
              bridge={bridge}
              draggingId={draggingId}
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
            />
          ))}
          </ul>
          {/* Link-group bracket layer. Absolutely positioned at the
              measured longest-name right edge (bracketLeft) so the
              brackets HUG the names rather than sitting at the panel's
              far-right edge. Each group has its own DEDICATED lane (one
              per group, by groupId); a STUB is drawn at every member row
              (including first + last), not just top/bottom caps. The
              layer scrolls with the rows (absolute inside the relative
              scroll container). The gutter container stays
              pointer-events-none so the gaps between lanes click through;
              each bracket re-enables pointer events for (click =
              select the group, hover = tint members). Brackets stay
              aria-hidden — they're a mouse convenience over the already
              keyboard-accessible row selection, so the a11y tree (and the
              goldens) are unchanged. */}
          {bracketLeft !== null && brackets.length > 0 && (
            <div
              data-testid="link-group-bracket-gutter"
              aria-hidden
              className="pointer-events-none absolute top-0"
              style={{ left: bracketLeft }}
            >
              {brackets.map((b) => {
                const top    = b.firstRowIndex * ROW_HEIGHT_PX + ROW_HEIGHT_PX / 2;
                const height = (b.lastRowIndex - b.firstRowIndex) * ROW_HEIGHT_PX;
                const left   = b.lane * LANE_WIDTH_PX;
                const hovered = hoveredLinkGroup === b.groupId;
                // The clickable hit-zone is one lane wide (LANE_WIDTH_PX),
                // ~5× the visible 2px line, so it's easy to hit yet never
                // overlaps an adjacent lane. It sits inset 4px so the visible
                // line keeps its original x. (clicking the bracket wiped
                // a row selection) is guarded by: (a) pointer-events live ONLY
                // on this hit-zone, not the whole gutter; (b) stopPropagation
                // on click + pointerdown so the press never reaches the row
                // button beneath nor starts a marquee. Hovering the hit-zone
                // also lights the group (the click affordance).
                const HITZONE_INSET = 4;
                return (
                  <div
                    key={b.groupId}
                    data-testid={`link-group-bracket-${b.groupId}`}
                    data-link-group={b.groupId}
                    data-lane={b.lane}
                    role="button"
                    tabIndex={0}
                    aria-label={`Select link group ${b.groupId}`}
                    title={`Select link group ${b.groupId}`}
                    className="pointer-events-auto absolute cursor-pointer"
                    style={{ top, left: left - HITZONE_INSET, width: LANE_WIDTH_PX, height }}
                    onPointerEnter={() => setHoveredLinkGroup(b.groupId)}
                    onPointerLeave={() => setHoveredLinkGroup(null)}
                    onPointerDown={(e) => e.stopPropagation()}
                    onClick={(e) => {
                      e.stopPropagation();
                      handleSelectLinkGroup(b.groupId);
                    }}
                    onKeyDown={(e) => {
                      if (e.key === "Enter" || e.key === " ") {
                        e.preventDefault();
                        e.stopPropagation();
                        handleSelectLinkGroup(b.groupId);
                      }
                    }}
                  >
                    {/* The visible coloured line. */}
                    <div
                      aria-hidden
                      className="absolute"
                      style={{
                        left: HITZONE_INSET,
                        top: 0,
                        width: hovered ? 3 : 2,
                        height,
                        background: b.color,
                        opacity: hovered ? 1 : 0.85,
                      }}
                    />
                    {b.memberRowIndices.map((rowIdx) => (
                      <div
                        key={rowIdx}
                        aria-hidden
                        data-testid={`link-group-stub-${b.groupId}-${rowIdx}`}
                        className="absolute"
                        style={{
                          top: (rowIdx - b.firstRowIndex) * ROW_HEIGHT_PX - 1,
                          left: 0,
                          width: 5,
                          height: 2,
                          background: b.color,
                        }}
                      />
                    ))}
                  </div>
                );
              })}
            </div>
          )}
          {/* Marquee (rubber-band) selection rectangle (). */}
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
    </div>
  );
}
