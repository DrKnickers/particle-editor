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
// `primary` atom on top (see `lib/tree/emitter-selection.ts`). Plain click
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
} from "react";
import type {
  Bridge,
  EmitterTreeDto,
  EmitterTreeNode,
} from "@particle-editor/bridge-schema";
import { useTreeActionStore } from "@/lib/tree/tree-action";
import {
  useEmitterSelectionIds,
  useEmitterSelectionPrimary,
  useEmitterSelectionStore,
} from "@/lib/tree/emitter-selection";
import { computeAutoscrollDelta } from "@/lib/drag-autoscroll";
import { computeFlipDeltas, DRAG_FEEL, pickFlipDuration, type FlipPositions } from "@/lib/flip";
import { useRecording } from "@/lib/record/record-mode";
import {
  computeRootGapIndex,
  isDescendant,
  resolveReparentSlot,
  type DropZone,
} from "@/lib/tree/drop-zone";
import { estimateChainLoad, estimateSystemLoad, type ChainWarning } from "@/lib/chain-load";
import { useOverloadGuardConfig } from "@/lib/overload-guard";
import { useEstimatedLoadPush } from "@/lib/use-estimated-load-push";
import { SystemLoadChip } from "@/components/SystemLoadChip";
import { useEmitterTree, useEmitterTreeStore } from "@/lib/tree/emitter-tree";
import { reorderManyEmitters } from "@/lib/tree/emitter-reorder";
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
import { useEmitterMarquee } from "./emitter-tree/useEmitterMarquee";
import { useEmitterRename } from "./emitter-tree/useEmitterRename";
import { useEmitterTreeKeyboard } from "./emitter-tree/useEmitterTreeKeyboard";
import { EmitterRow } from "./emitter-tree/EmitterRow";
import { EmitterTreeToolbar } from "./emitter-tree/EmitterTreeToolbar";
import type { FlatRow, DropIndicator } from "./emitter-tree/types";

export type { RenameEditingState } from "./emitter-tree/useEmitterRename";

type Props = {
  bridge: Bridge;
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

// [multi-drag] Chip-magnetize tuning: how far the chip's Y leans toward the
// active gap's center (0 = stays at the pointer, 1 = docks onto the gap), and
// the per-frame spring factor for the glide between targets.
const CHIP_PULL = 0.6;
const CHIP_SPRING = 0.25;

// --record only, active-drag glide (see pickFlipDuration in lib/flip.ts): the FLIP
// glide is a wall-clock CSS transition, but the capture loop grabs each frame only
// after a slow paint+PrintWindow (tens of ms of REAL time per frame). At
// At the short live-drag duration, the transition finishes between captures, so the reorder is never
// caught mid-glide and the rows snap in the recorded clip. This value is
// EMPIRICALLY TUNED to the capture cadence (≈800ms spans ~8 captured frames) — not
// an animation-feel choice; re-verify against a --record clip before changing it.

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
    const firstEl = sc.querySelector(`[data-emitter-id="${ids[0]}"]`);
    const lastEl = sc.querySelector(`[data-emitter-id="${ids[ids.length - 1]}"]`);
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
    const el = sc.querySelector(`[data-emitter-id="${r.node.id}"]`);
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

export function EmitterTree({ bridge }: Props) {
  const tree = useEmitterTree(bridge);
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
    // Exit set = despawn in flight: the chip transitions to that point
    // (the landing gap / reparent row, or its own spot for a cancel) while
    // fading, then a timeout clears the state.
    { names: string[]; exit?: { x: number; y: number } } | null
  >(null);
  const dragChipRef = useRef<HTMLDivElement | null>(null);
  const dragChipPositionRef = useRef({ x: 0, y: 0 });
  const writeDragChipTransform = useCallback((x: number, y: number) => {
    dragChipPositionRef.current = { x, y };
    const el = dragChipRef.current;
    if (el !== null) el.style.transform = "translate3d(" + x + "px, " + y + "px, 0)";
  }, []);
  const setDragChipElement = useCallback((el: HTMLDivElement | null) => {
    dragChipRef.current = el;
    if (el === null) return;
    const { x, y } = dragChipPositionRef.current;
    el.style.transform = "translate3d(" + x + "px, " + y + "px, 0)";
  }, []);
  // [pointer-drag] Set true when a real drag completes so the synthetic
  // click that follows pointerup (when down+up land on the same row) does
  // NOT also fire row selection. Reset on the next pointerdown and in
  // handleRowClick. (HTML5 DnD is replaced by pointer events because
  // dragstart never fires under composition hosting — WebView2 is a
  // composition visual with no HWND for the OS drag loop.)
  const draggedRef = useRef(false);

  // While a pointer drag is armed or active this holds a function that
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
  const {
    editing,
    editingRef,
    beginEdit,
    setEditValue,
    handleRenameInputKeyDown,
    handleRenameInputBlur,
  } = useEmitterRename({
    bridge,
    restoreFocus: (id) => {
      treeContainerRef.current
        ?.querySelector<HTMLElement>(`[data-emitter-id="${id}"]`)
        ?.focus();
    },
  });

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
        if (!cancelled && (t as EmitterTreeDto | null)?.root) setTree(t, bridge);
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
  // translateY, then transition to zero — snappier mid-drag than on settle.
  // The dragged block's own rows glide the
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
      ? container.querySelector<HTMLElement>(`[data-emitter-id="${primaryId}"]`)
      : null;
    (primaryBtn ?? container).focus();
  }, [flatRows, primaryId]);

  const recording = useRecording();

  // Entrance fade for newly ADDED rows (add/paste/duplicate).
  // Sibling reflow is the FLIP glide below; this covers the new row itself,
  // which used to pop in. Track every stableId ever rendered; a row whose id
  // is unseen this render wears .row-fade-in for its mount. Exempt: the
  // initial mount (ref still null), full-tree rebuilds (undo/redo mints
  // FRESH stableIds for EVERY row — fading the whole tree would read as a
  // flash, per the remount note above), and --record runs (frame
  // determinism; CSS side is also killed via [data-recording]).
  const seenStableIdsRef = useRef<Set<number> | null>(null);
  const enteringIds = useMemo<ReadonlySet<number>>(() => {
    const seen = seenStableIdsRef.current;
    if (seen === null || recording) return new Set();
    const fresh = flatRows.filter((r) => !seen.has(r.node.stableId));
    if (fresh.length === 0 || fresh.length === flatRows.length) return new Set();
    return new Set(fresh.map((r) => r.node.stableId));
  }, [flatRows, recording]);
  useEffect(() => {
    // Replace (not accumulate): the set is "the PREVIOUS render's ids", so it
    // can't grow unboundedly across a session. A stableId
    // deleted and re-added later fades in again — correct, it IS an add.
    seenStableIdsRef.current = new Set(flatRows.map((r) => r.node.stableId));
  }, [flatRows]);

  // Exit ghosts: a DELETED row fades out at its last
  // position (absolute overlay in scroll-content space) while its siblings
  // FLIP-glide to close the gap — the removal used to pop. Metadata for
  // vanished rows comes from the PREVIOUS render's flatRows (the FLIP
  // position map only holds offsets); coordinates
  // from the position snapshot taken in the same layout effect. Skipped
  // while a drag/indicator is active (the gap spacer corrupts tops), under
  // reduced-motion and --record, and on bulk rebuilds (undo/redo mints
  // fresh ids for every row — no ghost storms).
  type ExitGhost = { stableId: number; top: number; name: string; depth: number };
  const [exitGhosts, setExitGhosts] = useState<readonly ExitGhost[]>([]);
  const prevRowMetaRef = useRef<Map<number, { name: string; depth: number }>>(new Map());
  const EXIT_GHOST_MS = 200; // popover-pop-out (110ms, `both`) + removal slack
  // Removal timers, cleared on unmount so a late timeout can't setState on a
  // dead component.
  const ghostTimersRef = useRef<Set<number>>(new Set());
  useEffect(() => {
    const timers = ghostTimersRef.current;
    return () => { for (const t of timers) window.clearTimeout(t); };
  }, []);

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

    // Exit ghosts: detect removals against the snapshot we just replaced.
    const reduceMotion =
      typeof window.matchMedia === "function" &&
      window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (!recording && !reduceMotion && draggingId === null && indicator === null) {
      let surviving = 0;
      for (const id of next.keys()) if (prev.has(id)) surviving += 1;
      // A REBUILD replaces rows with fresh ids (undo/redo) — skip. An EMPTY
      // next is a mass DELETE, not a rebuild: those rows should ghost (the
      // ≤8 cap below still bounds a storm).
      const fullRebuild = next.size > 0 && prev.size > 0 && surviving === 0;
      if (!fullRebuild) {
        const removed: ExitGhost[] = [];
        for (const [id, top] of prev) {
          if (next.has(id)) continue;
          const meta = prevRowMetaRef.current.get(id);
          if (meta !== undefined) removed.push({ stableId: id, top, name: meta.name, depth: meta.depth });
        }
        if (removed.length > 0 && removed.length <= 8) {
          const ids = removed.map((g) => g.stableId);
          setExitGhosts((gs) => [
            ...gs.filter((g) => !ids.includes(g.stableId)),
            ...removed,
          ]);
          ghostTimersRef.current.add(
            window.setTimeout(() => {
              setExitGhosts((gs) => gs.filter((g) => !ids.includes(g.stableId)));
            }, EXIT_GHOST_MS),
          );
        }
      }
    }
    prevRowMetaRef.current = new Map(
      flatRows.map((r) => [r.node.stableId, { name: r.node.name, depth: r.depth }]),
    );
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
    // spans multiple slow captures); the settle keeps DRAG_FEEL.settleMs to stay in
    // step with the drag-chip despawn. See pickFlipDuration.
    const durationMs = pickFlipDuration(recording, draggingId !== null, {
      ...DRAG_FEEL,
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
  // drag-start — safe because a tree/changed aborts the gesture (armed or
  // active) via activeDragCancelRef.
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
      writeDragChipTransform(chipPos.x, chipPos.y);
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
        if (hasChip) {
          setDragChip({ names: chipNames });
          writeDragChipTransform(chipPos.x, chipPos.y);
        }
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
        // Tear down on focus loss (alt-tab / window blur / tab hide)
        // so the drag can't get stranded with no pointerup ever arriving.
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
    // Expose the abort hook from ARMING, not activation: the closures above
    // captured the tree at pointerdown, so a tree/changed that lands before
    // the 4px threshold must disarm the gesture too — otherwise the later
    // activation resolves (and commits) positional ids against a stale tree.
    activeDragCancelRef.current = () => finish(false);
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

  const { treeScrollRef, marqueeBox, handleScrollPointerDown } = useEmitterMarquee({ orderedIds });

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

  const { handleTreeKeyDown } = useEmitterTreeKeyboard({
    bridge,
    flatRows,
    orderedIds,
    primaryId,
    editingRef,
    beginEdit,
    treeContainerRef,
  });

  // Exit-ghost nodes, rendered by whichever branch owns the tree area right
  // now: the relative scroll container (normal case) or the emptied-tree
  // state (a mass delete removes the container in the same commit — the
  // ghosts must not vanish with it). Tops are scroll-content
  // coordinates; the empty branch occupies the same slot, close enough for
  // a 200ms decorative fade.
  const exitGhostNodes = exitGhosts.map((g) => (
    <div
      key={`exit-ghost-${g.stableId}`}
      aria-hidden
      data-testid="emitter-exit-ghost"
      data-state="closed"
      className="fade-animate-fast pointer-events-none absolute inset-x-0"
      style={{ top: `${g.top}px` }}
    >
      <div
        className="truncate py-0.5 pr-2 text-left text-sm text-text-3"
        style={{ paddingLeft: `${8 + 18 + g.depth * 12}px` }}
      >
        {g.name}
      </div>
    </div>
  ));

  return (
    <div
      ref={treeContainerRef}
      data-testid="emitter-tree"
      data-selected-count={selectedIds.length}
      data-primary-id={primaryId ?? ""}
      data-dragging-id={draggingId ?? ""}
      data-editing-id={editing?.id ?? ""}
      // -1: rows rove (the primary row is the tree's Tab
      // stop), so the container no longer takes its own stop — but it stays
      // programmatically focusable for the initial-load / remount-restore
      // focus paths above.
      tabIndex={-1}
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
        // Shape-matched static skeleton with an
        // sr-only status so AT still hears the loading→loaded transition.
        <div role="status" className="flex-1 min-h-0 px-1 pt-1">
          <span className="sr-only">Loading emitters…</span>
          <div aria-hidden className="flex flex-col gap-1">
            <div className="skeleton-row" />
            <div className="skeleton-row w-4/5" />
            <div className="skeleton-row w-3/5" />
          </div>
        </div>
      ) : rootChildren.length === 0 ? (
        // Teaching empty state: say what belongs here and
        // point at the next action instead of the old bare "(no emitters)".
        <div
          role="status"
          data-testid="emitter-tree-empty"
          className="relative flex flex-1 min-h-0 flex-col items-center justify-center gap-1 px-4 text-center"
        >
          <span className="text-sm text-text-2">No emitters yet</span>
          <span className="text-xs text-text-3">
            Add one with the + button below, or right-click a row later to
            duplicate, link, and reorder.
          </span>
          {exitGhostNodes}
        </div>
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
                  handleRenameInputKeyDown={handleRenameInputKeyDown}
                  handleRenameInputBlur={handleRenameInputBlur}
                  linkHover={
                    hoveredLinkGroup !== null &&
                    row.node.linkGroup === hoveredLinkGroup
                  }
                  onHoverLinkGroup={setHoveredLinkGroup}
                  onDissolveLinkGroup={handleDissolveLinkGroup}
                  onSelectLinkGroup={handleSelectLinkGroup}
                  chainWarning={chainWarnings.get(row.node.stableId) ?? null}
                  entering={enteringIds.has(row.node.stableId)}
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
          {/* Exit ghosts (shared nodes — also rendered by the emptied-
              tree branch below, whose deletes have no scroll container). */}
          {exitGhostNodes}
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
          ref={setDragChipElement}
          data-testid="drag-chip"
          data-exiting={dragChip.exit ? "true" : "false"}
          aria-hidden
          // drag-chip-enter: pop-in on spawn (components.css; reduced-motion
          // disables it). Exit mode transitions transform to the landing spot
          // + fades/shrinks via an inline transition - see finish().
          className="drag-chip-enter pointer-events-none fixed z-50 rounded-md border border-accent bg-bg-2/95 px-2 py-1 text-xs text-accent shadow-[var(--shadow-soft)]"
          style={
            dragChip.exit
              ? {
                  left: 0,
                  top: 0,
                  opacity: 0,
                  transform: "translate3d(" + dragChip.exit.x + "px, " + dragChip.exit.y + "px, 0) scale(0.85)",
                  transition:
                    "transform " + CHIP_EXIT_MS + "ms ease-in, opacity " + CHIP_EXIT_MS + "ms ease-in",
                }
              : { left: 0, top: 0 }
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
