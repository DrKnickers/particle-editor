// ImportEmittersDialog — Modal-based file picker → tree preview →
// branch-select checkboxes → import. Phase 3 Screen 8 Batch 4.
//
// Flow:
//   1. Modal opens; only "Browse…" is enabled.
//   2. Browse → bridge.request("file/open") → if ok:true, fire
//      "emitters/preview-from-file" with the resolved path.
//   3. Preview success → render the selection card. Selection is a
//      conventional file-tree BRANCH model: a parent checkbox owns its
//      whole subtree (none / partial / all), so ticking a parent pulls
//      its descendants in and a manually-unticked child leaves the
//      parent in the indeterminate ("partial") state.
//   4. OK ("Import N selected") → emitters/import-from-file → close.
//   5. Cancel anytime → close, discard state.
//
// Errors. file/open ok:false (user cancelled) leaves the modal open
// with the prompt still empty so the user can retry. preview ok:false
// surfaces an inline error message inside the body.

import { useEffect, useMemo, useRef, useState } from "react";
import type {
  Bridge,
  EmitterTreeNode,
} from "@particle-editor/bridge-schema";
import { ChevronRight, File } from "lucide-react";
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

/** A native checkbox that also reflects the indeterminate state. React has
 *  no JSX attribute for `indeterminate` (it's a DOM property), so we set it
 *  via a ref in an effect keyed on the value. */
function TriCheckbox({
  checked,
  indeterminate,
  onChange,
  "aria-label": ariaLabel,
  className,
}: {
  checked: boolean;
  indeterminate: boolean;
  onChange: () => void;
  "aria-label": string;
  className?: string;
}) {
  const ref = useRef<HTMLInputElement>(null);
  useEffect(() => {
    if (ref.current) ref.current.indeterminate = indeterminate;
  }, [indeterminate]);
  return (
    <input
      ref={ref}
      type="checkbox"
      checked={checked}
      onChange={onChange}
      aria-label={ariaLabel}
      className={className}
    />
  );
}

export function ImportEmittersDialog({ bridge, open, onOpenChange }: Props) {
  const [sourcePath, setSourcePath] = useState<string | null>(null);
  const [tree, setTree] = useState<EmitterTreeNode | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [picks, setPicks] = useState<Set<number>>(() => new Set());
  const [collapsed, setCollapsed] = useState<Set<number>>(() => new Set());

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
      const r = await bridge.request({ kind: "file/open", params: {} });
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
          // expanded; reset both so stale ids from a prior file (ids
          // overlap — both start at 1) can't mis-collapse this one.
          setPicks(new Set());
          setCollapsed(new Set());
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
    if (!sourcePath || picks.size === 0) return;
    try {
      await bridge.request({
        kind: "emitters/import-from-file",
        params: { path: sourcePath, selected: Array.from(picks) },
      });
      onOpenChange(false);
    } catch (err) {
      // emitters/import-from-file isn't implemented in the mock yet;
      // surface the error inline and keep the modal open.
      console.warn("[ImportEmitters] import failed:", err);
      setError(String(err));
    }
  };

  // ── Render helpers ───────────────────────────────────────────────

  const renderNode = (node: EmitterTreeNode, depth: number) => {
    const state = branchState(node, picks);
    const hasChildren = node.children.length > 0;
    const isCollapsed = collapsed.has(node.id);
    return (
      <div key={node.id}>
        {/* Selection fill is the app's `bg-accent/20` (ReferenceObjectPicker
            selection) and applies to FULLY-selected rows only; a partial
            branch is carried by the indeterminate dash alone, so its row
            stays neutral (no "selected" fill on a not-fully-selected node).
            `mb-0.5` separates rows into discrete pills (matching the picker's
            gap-0.5) so adjacent selections don't fuse into one band. */}
        <div
          data-selstate={state}
          className={
            "mb-0.5 flex h-[26px] items-center gap-1.5 rounded pr-1.5 text-xs " +
            (state === "all" ? "bg-accent/20" : "hover:bg-hover")
          }
          style={{ paddingLeft: `${4 + depth * 16}px` }}
        >
          {hasChildren ? (
            <button
              type="button"
              onClick={() => toggleCollapse(node.id)}
              aria-expanded={!isCollapsed}
              aria-label={`${isCollapsed ? "Expand" : "Collapse"} ${node.name}`}
              className="flex size-4 shrink-0 items-center justify-center rounded text-text-3 hover:text-text focus-ring"
            >
              <ChevronRight
                className={
                  "size-3.5 transition-transform " +
                  (isCollapsed ? "" : "rotate-90")
                }
              />
            </button>
          ) : (
            <span className="size-4 shrink-0" aria-hidden />
          )}
          <label className="flex min-w-0 flex-1 items-center gap-2 py-1 text-text">
            <TriCheckbox
              checked={state === "all"}
              indeterminate={state === "partial"}
              onChange={() => toggleNode(node)}
              aria-label={`Select ${node.name}`}
              className="size-3 shrink-0 accent-[var(--accent)]"
            />
            <span className="truncate" title={node.name}>
              {node.name}
            </span>
          </label>
        </div>
        {hasChildren &&
          !isCollapsed &&
          node.children.map((c) => renderNode(c, depth + 1))}
      </div>
    );
  };

  const sourceLabel = sourcePath ? basename(sourcePath) : "(no file selected)";

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
          {!loading && error && (
            <div className="min-h-[160px] rounded border border-border bg-bg p-3 text-xs text-danger-fg">
              {error}
            </div>
          )}
          {!loading && !error && !tree && (
            <div className="flex min-h-[160px] items-center justify-center rounded border border-border bg-bg p-3 text-center text-xs text-text-3">
              Click Browse… to select a source .alo file.
            </div>
          )}
          {!loading && !error && tree && (
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
              {/* Tree body */}
              <div
                role="group"
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
          disabled={picks.size === 0}
        >
          {picks.size > 0 ? `Import ${picks.size} selected` : "Import"}
        </Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
