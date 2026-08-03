// ImportEmittersDialog — Modal-based file picker → tree preview →
// branch-select checkboxes → import.
//
// Flow:
//   1. Modal opens; only "Browse…" is enabled.
//   2. Browse → bridge.request("file/pick-open") (NON-MUTATING picker — never
//      loads the file as the active document) → if ok:true, fire
//      "emitters/preview-from-file" with the resolved path.
//   3. Preview success → render the selection card. Selection is a
//      conventional file-tree BRANCH model: a parent checkbox owns its
//      whole subtree (none / partial / all), so ticking a parent pulls
//      its descendants in and a manually-unticked child leaves the
//      parent in the indeterminate ("partial") state.
//   4. OK ("Import N selected") → emitters/import-from-file. Close only
//      on a full import; a 0 / partial count keeps the modal open with
//      an inline message (see Errors).
//   5. Cancel anytime → close, discard state.
//
// Errors. file/pick-open ok:false (user cancelled) leaves the modal open
// with the prompt still empty so the user can retry. preview ok:false
// surfaces an inline error message inside the body (no tree). import
// resolves `{ imported }` (a count, not an ok-discriminant): imported
// === 0 or a partial (< requested) result shows an inline message
// ABOVE the still-loaded tree and stays open so the user can re-pick;
// a rejected request (native hard failure / mock not-implemented) is
// caught and shown the same way. Only a full import closes the modal.

import { useEffect, useMemo, useRef, useState, type KeyboardEvent } from "react";
import type {
  Bridge,
  EmitterTreeNode,
} from "@particle-editor/bridge-schema";
import { Check, ChevronRight, File, Minus } from "lucide-react";
import { Modal } from "@/components/Modal";

type Props = {
  bridge: Bridge;
  open: boolean;
  onOpenChange: (open: boolean) => void;
};

/** Collect descendant ids (not including the node itself). Used by the
 *  branch-select cascade and the branch-state computation. */
function descendantIds(node: EmitterTreeNode): number[] {
  const out: number[] = [];
  for (const child of node.children) {
    out.push(child.id);
    out.push(...descendantIds(child));
  }
  return out;
}

function basename(path: string): string {
  const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return idx >= 0 ? path.slice(idx + 1) : path;
}

type BranchState = "none" | "partial" | "all";

/** Selection state of a node's whole branch (self ∪ descendants) against
 *  `picks`. Drives both the checkbox glyph and the click behaviour:
 *  `all` → checked, `partial` → indeterminate dash, `none` → empty. */
function branchState(node: EmitterTreeNode, picks: Set<number>): BranchState {
  const ids = [node.id, ...descendantIds(node)];
  let sel = 0;
  for (const id of ids) if (picks.has(id)) sel++;
  if (sel === 0) return "none";
  if (sel === ids.length) return "all";
  return "partial";
}

type VisibleRow = { node: EmitterTreeNode; depth: number; parentId: number | null };

/** Flatten the tree to the currently-visible rows (collapsed subtrees skipped),
 *  in DOM order, with depth + parent id. This is the index space the roving
 *  tabindex / arrow-key engine moves over (↑/↓ by index, ← to parentId). */
function visibleRows(tree: EmitterTreeNode | null, collapsed: Set<number>): VisibleRow[] {
  if (!tree) return [];
  const out: VisibleRow[] = [];
  const walk = (nodes: EmitterTreeNode[], depth: number, parentId: number | null) => {
    for (const n of nodes) {
      out.push({ node: n, depth, parentId });
      if (n.children.length > 0 && !collapsed.has(n.id)) walk(n.children, depth + 1, n.id);
    }
  };
  walk(tree.children, 0, null);
  return out;
}

const ARROW_KEYS = new Set([
  "ArrowDown", "ArrowUp", "ArrowLeft", "ArrowRight", "Home", "End", " ", "Enter",
]);

export function ImportEmittersDialog({ bridge, open, onOpenChange }: Props) {
  const [sourcePath, setSourcePath] = useState<string | null>(null);
  const [tree, setTree] = useState<EmitterTreeNode | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [picks, setPicks] = useState<Set<number>>(() => new Set());
  const [collapsed, setCollapsed] = useState<Set<number>>(() => new Set());
  // True while an import request is in flight. Blocks a second OK click
  // (the native import has no timeout and can block on a slow disk read),
  // which would otherwise duplicate-import and push a second undo entry.
  const [importing, setImporting] = useState(false);
  // Roving-tabindex focus target (the treeitem the arrow keys act on). One row
  // is tabbable at a time; null falls back to the first visible row.
  const [activeId, setActiveId] = useState<number | null>(null);
  const rowRefs = useRef(new Map<number, HTMLDivElement>());
  const setRowRef = (id: number) => (el: HTMLDivElement | null) => {
    if (el) rowRefs.current.set(id, el);
    else rowRefs.current.delete(id);
  };

  // Reset state whenever the modal opens fresh so a previous session
  // doesn't bleed in (a closed-then-reopened modal should start empty).
  useEffect(() => {
    if (open) {
      setSourcePath(null);
      setTree(null);
      setError(null);
      setPicks(new Set());
      setCollapsed(new Set());
      setLoading(false);
      setImporting(false);
      setActiveId(null);
    }
  }, [open]);

  // All selectable ids (everything except the synthetic root id 0).
  const allIds = useMemo(() => {
    if (!tree) return [] as number[];
    const ids: number[] = [];
    for (const c of tree.children) {
      ids.push(c.id);
      ids.push(...descendantIds(c));
    }
    return ids;
  }, [tree]);

  const handleBrowse = async () => {
    setError(null);
    try {
      // Non-mutating picker: returns the chosen path WITHOUT loading it as the
      // active document, so browsing for an import source can never replace the
      // current (possibly dirty) document (release-audit #2).
      const r = await bridge.request({ kind: "file/pick-open", params: {} });
      if (!r.ok) {
        // User cancelled the picker or browser-mode rejected — leave
        // the modal open so a retry is one click away.
        return;
      }
      const path = r.path ?? "";
      if (!path) return;
      setSourcePath(path);
      setLoading(true);
      try {
        const preview = await bridge.request({
          kind: "emitters/preview-from-file",
          params: { path },
        });
        if (preview.ok) {
          setTree(preview.tree);
          // A fresh tree starts with nothing picked and everything
          // expanded; reset all three so stale ids from a prior file (ids
          // overlap — both start at 1) can't mis-collapse or mis-focus this one.
          setPicks(new Set());
          setCollapsed(new Set());
          setActiveId(null);
        } else {
          setError(preview.error);
          setTree(null);
        }
      } finally {
        setLoading(false);
      }
    } catch (err) {
      console.warn("[ImportEmitters] browse failed:", err);
      setError(String(err));
      setLoading(false);
    }
  };

  /** Branch toggle. Clicking a node that isn't fully selected selects its
   *  whole branch (self + descendants); clicking a fully-selected one clears
   *  the branch. A leaf has no descendants, so this is a plain toggle. */
  const toggleNode = (node: EmitterTreeNode) => {
    const want = branchState(node, picks) !== "all";
    setPicks((prev) => {
      const next = new Set(prev);
      for (const id of [node.id, ...descendantIds(node)]) {
        if (want) next.add(id);
        else next.delete(id);
      }
      return next;
    });
  };

  const toggleCollapse = (id: number) =>
    setCollapsed((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  const handleSelectAll = () => setPicks(new Set(allIds));
  // Legacy IDC_IMPORT_CLEAR: deselect every node.
  const handleClear = () => setPicks(new Set());

  const handleOk = async () => {
    if (!sourcePath || picks.size === 0 || importing) return;
    const requested = picks.size;
    setError(null); // clear any prior attempt's message before retrying
    setImporting(true);
    try {
      const { imported } = await bridge.request({
        kind: "emitters/import-from-file",
        params: { path: sourcePath, selected: Array.from(picks) },
      });
      // A resolved request only means the host didn't throw — the real
      // outcome is the `imported` count. Closing on resolve alone would
      // report a fake success when 0 (or only some) actually landed.
      // `>=` (not `===`) is defensive: the host can never return more than
      // requested, but treating an impossible over-count as full success
      // is safer than mislabelling it a partial.
      if (imported >= requested) {
        // Full success — every selected emitter was imported.
        onOpenChange(false);
      } else if (imported === 0) {
        // Resolved with 0 imported. The host rejects (→ catch) for every
        // pre-flight failure — missing/out-of-range picks, unreadable
        // file — so the only way here is: the source loaded but every
        // clone threw (corrupt emitter data). Keep the modal open with
        // the tree intact so the user can pick a different source.
        console.warn(`[ImportEmitters] imported 0 of ${requested}`);
        setError(
          "No emitters were imported. The source file may contain unreadable emitter data.",
        );
      } else {
        // Partial import — some landed, some didn't. The host re-loads the
        // same source the preview read and imports every in-range pick, so a
        // shortfall here means some emitters failed to clone (corrupt emitter
        // data); name that rather than a vague "some couldn't be added".
        console.warn(`[ImportEmitters] imported ${imported} of ${requested}`);
        setError(
          `Imported ${imported} of ${requested} selected. The rest had unreadable data — the source file may be corrupt.`,
        );
      }
    } catch (err) {
      // The request rejected outright: the native host rejects on any
      // hard failure (no system bound, missing/out-of-range picks,
      // unreadable file), and the browser mock throws because import
      // isn't implemented there. Either way, surface it and stay open.
      console.warn("[ImportEmitters] import failed:", err);
      setError(String(err));
    } finally {
      // Re-enable OK whether we imported, errored, or stayed open. (On a
      // full import the modal is closing, but the open-reset effect also
      // clears this, so a reopen always starts ready.)
      setImporting(false);
    }
  };

  // ── Keyboard tree (roving tabindex + WAI-ARIA Tree View) ─────────
  const rows = useMemo(() => visibleRows(tree, collapsed), [tree, collapsed]);
  // The single tabbable row: the active one if still visible, else the first.
  const activeRowId =
    rows.length === 0
      ? null
      : activeId !== null && rows.some((r) => r.node.id === activeId)
        ? activeId
        : rows[0]!.node.id;

  const focusRow = (id: number) => {
    setActiveId(id);
    rowRefs.current.get(id)?.focus();
  };

  /** APG Tree View keyboard map. Single tab stop; ↑/↓ move focus, →/← expand/
   *  collapse or step to child/parent, Home/End jump, Space/Enter toggle
   *  selection. Range-select (Shift/Ctrl) is intentionally out of scope. */
  const onTreeKeyDown = (e: KeyboardEvent<HTMLDivElement>, node: EmitterTreeNode) => {
    if (!ARROW_KEYS.has(e.key)) return; // let Tab / Esc bubble to the modal
    e.preventDefault();
    e.stopPropagation(); // a focused child must not let an ancestor re-handle
    const idx = rows.findIndex((r) => r.node.id === node.id);
    if (idx < 0) return;
    const hasKids = node.children.length > 0;
    switch (e.key) {
      case "ArrowDown":
        if (idx < rows.length - 1) focusRow(rows[idx + 1]!.node.id);
        break;
      case "ArrowUp":
        if (idx > 0) focusRow(rows[idx - 1]!.node.id);
        break;
      case "ArrowRight":
        if (hasKids && collapsed.has(node.id)) toggleCollapse(node.id); // expand
        else if (hasKids) focusRow(node.children[0]!.id); // → first child
        break;
      case "ArrowLeft":
        if (hasKids && !collapsed.has(node.id)) toggleCollapse(node.id); // collapse
        else if (rows[idx]!.parentId !== null) focusRow(rows[idx]!.parentId!); // → parent
        break;
      case "Home":
        focusRow(rows[0]!.node.id);
        break;
      case "End":
        focusRow(rows[rows.length - 1]!.node.id);
        break;
      case " ":
      case "Enter":
        setActiveId(node.id);
        toggleNode(node);
        break;
    }
  };

  // ── Render helpers ───────────────────────────────────────────────

  // A multi-select treeitem. Selection lives on the treeitem's `aria-checked`
  // (true / false / "mixed"); the checkbox is a presentational graphic. The
  // chevron is a presentational click target — NOT a focusable button, which
  // would add a second tab stop inside the single-tab-stop tree.
  const renderNode = (node: EmitterTreeNode, depth: number) => {
    const state = branchState(node, picks);
    const hasChildren = node.children.length > 0;
    const isCollapsed = collapsed.has(node.id);
    const ariaChecked: boolean | "mixed" =
      state === "all" ? true : state === "partial" ? "mixed" : false;
    return (
      <div
        key={node.id}
        role="treeitem"
        aria-label={node.name}
        aria-checked={ariaChecked}
        aria-level={depth + 1}
        aria-expanded={hasChildren ? !isCollapsed : undefined}
        data-selstate={state}
        ref={setRowRef(node.id)}
        tabIndex={node.id === activeRowId ? 0 : -1}
        onClick={(e) => { e.stopPropagation(); setActiveId(node.id); toggleNode(node); }}
        onKeyDown={(e) => onTreeKeyDown(e, node)}
        className="flex flex-col rounded outline-none focus-ring"
      >
        {/* Visual row. `bg-accent/20` only on a FULLY-selected row; a partial
            branch is carried by the dash glyph alone. `mb-0.5` keeps adjacent
            selections as discrete pills. */}
        <div
          className={
            "mb-0.5 flex h-[26px] cursor-pointer items-center gap-1.5 rounded pr-1.5 text-xs " +
            (state === "all" ? "bg-accent/20" : "hover:bg-hover")
          }
          style={{ paddingLeft: `${4 + depth * 16}px` }}
        >
          {hasChildren ? (
            <span
              aria-hidden
              data-testid="row-chevron"
              onClick={(e) => {
                e.stopPropagation();
                // APG: collapsing a node that contains focus moves focus to
                // that node, so the focused descendant doesn't unmount out
                // from under the user (mouse-collapse while a child is focused).
                if (!isCollapsed && activeRowId !== null && descendantIds(node).includes(activeRowId)) {
                  focusRow(node.id);
                }
                toggleCollapse(node.id);
              }}
              className="flex size-4 shrink-0 cursor-pointer items-center justify-center rounded text-text-3 hover:text-text"
            >
              <ChevronRight
                className={"size-3.5 transition-transform motion-reduce:transition-none " + (isCollapsed ? "" : "rotate-90")}
              />
            </span>
          ) : (
            <span className="size-4 shrink-0" aria-hidden />
          )}
          <span
            aria-hidden
            className={
              "flex size-3 shrink-0 items-center justify-center rounded-[3px] " +
              (state === "none" ? "border border-border-2" : "bg-accent-strong text-white")
            }
          >
            {state === "all" && <Check className="size-2.5" strokeWidth={3} />}
            {state === "partial" && <Minus className="size-2.5" strokeWidth={3} />}
          </span>
          <span className="min-w-0 flex-1 truncate text-text" title={node.name}>
            {node.name}
          </span>
        </div>
        {hasChildren && !isCollapsed && (
          <div role="group">
            {node.children.map((c) => renderNode(c, depth + 1))}
          </div>
        )}
      </div>
    );
  };

  const sourceLabel = sourcePath ? basename(sourcePath) : "(no file selected)";
  // A successfully-previewed .alo with no root emitters is a model file, not a
  // particle effect — there's nothing to import. Signal that instead of showing
  // an empty selection card.
  const emptyTree = tree !== null && tree.children.length === 0;

  return (
    <Modal
      open={open}
      onOpenChange={onOpenChange}
      title="Import Emitters"
      size="md"
    >
      <Modal.Body>
        <div className="space-y-3">
          {/* Source file row */}
          <div className="flex items-center gap-2">
            <span className="text-[11px] text-text-2">Source</span>
            <span
              title={sourcePath ?? undefined}
              className="flex min-w-0 flex-1 items-center gap-1.5 rounded bg-bg-3 px-2 py-1 text-xs text-text-2"
            >
              <File className="size-3.5 shrink-0 text-text-3" aria-hidden />
              <span className="truncate">{sourceLabel}</span>
            </span>
            <button
              type="button"
              onClick={() => void handleBrowse()}
              aria-label="Browse for source file"
              className="shrink-0 rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 focus-ring"
            >
              Browse…
            </button>
          </div>

          {/* Selection card (only once a tree is loaded) / states */}
          {loading && (
            <div className="min-h-[160px] rounded border border-border bg-bg p-3 text-xs text-text-3">
              Loading preview…
            </div>
          )}
          {/* A preview failure sets `error` with NO tree → show it alone. An
              import outcome (0 / partial / reject) sets `error` while the tree
              is still loaded; that message renders as a banner INSIDE the card
              below (not here) so the selection stays editable for a retry. */}
          {!loading && error && !tree && (
            <div className="min-h-[160px] rounded border border-border bg-bg p-3 text-xs text-danger-fg">
              {error}
            </div>
          )}
          {!loading && !error && !tree && (
            <div className="flex min-h-[160px] items-center justify-center rounded border border-border bg-bg p-3 text-center text-xs text-text-3">
              Click Browse… to select a source .alo file.
            </div>
          )}
          {!loading && !error && emptyTree && (
            <div className="flex min-h-[160px] flex-col items-center justify-center gap-1 rounded border border-border bg-bg p-3 text-center text-xs text-text-3">
              <span className="text-text-2">No particle emitters in this file</span>
              <span>This .alo looks like a model — there's nothing to import. Browse for a particle effect.</span>
            </div>
          )}
          {!loading && tree && !emptyTree && (
            <div className="overflow-hidden rounded border border-border">
              {/* Card header: count + bulk controls */}
              <div className="flex items-center justify-between border-b border-border bg-bg-2 px-3 py-2">
                <span className="text-[11px] font-semibold uppercase tracking-[0.04em] text-text-2">
                  Emitters{" "}
                  <span className="font-normal text-text-3">
                    · {picks.size} of {allIds.length} selected
                  </span>
                </span>
                <span className="flex gap-3">
                  <button
                    type="button"
                    onClick={handleSelectAll}
                    disabled={allIds.length === 0}
                    aria-label="Select all emitters"
                    className="text-[11px] text-accent hover:underline focus-ring disabled:cursor-not-allowed disabled:text-text-3 disabled:no-underline"
                  >
                    Select all
                  </button>
                  <button
                    type="button"
                    onClick={handleClear}
                    disabled={picks.size === 0}
                    aria-label="Clear selection"
                    className="text-[11px] text-accent hover:underline focus-ring disabled:cursor-not-allowed disabled:text-text-3 disabled:no-underline"
                  >
                    Clear
                  </button>
                </span>
              </div>
              {/* Import outcome (0 / partial / reject) — a banner above the
                  tree body so the selection stays visible for a retry. */}
              {error && (
                <div className="border-b border-border bg-bg px-3 py-2 text-xs text-warning-fg">
                  {error}
                </div>
              )}
              {/* Tree body — WAI-ARIA multi-select tree (roving tabindex). */}
              <div
                role="tree"
                aria-multiselectable="true"
                aria-label="Emitters"
                className="max-h-[260px] overflow-y-auto bg-bg p-1"
              >

                {/* Skip the synthetic root; render its children directly. */}
                {tree.children.map((c) => renderNode(c, 0))}
              </div>
            </div>
          )}
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton>Cancel</Modal.CancelButton>
        <Modal.OkButton
          onClick={() => void handleOk()}
          disabled={picks.size === 0 || importing}
        >
          {importing
            ? "Importing…"
            : picks.size > 0
              ? `Import ${picks.size} selected`
              : "Import"}
        </Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
