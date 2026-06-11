// ImportEmittersDialog — Modal-based file picker → tree preview →
// checkbox selection → import. Phase 3 Screen 8 Batch 4.
//
// Flow:
//   1. Modal opens; only "Browse…" is enabled.
//   2. Browse → bridge.request("file/open") → if ok:true, fire
//      "emitters/preview-from-file" with the resolved path.
//   3. Preview success → render tree-with-checkboxes. User picks.
//      "Auto-include children" default-on: ticking a parent cascades.
//   4. OK ("Import N selected") → emitters/import-from-file → close.
//   5. Cancel anytime → close, discard state.
//
// Errors. file/open ok:false (user cancelled) leaves the modal open
// with the prompt still empty so the user can retry. preview ok:false
// surfaces an inline error message inside the body.

import { useEffect, useMemo, useState } from "react";
import type {
  Bridge,
  EmitterTreeNode,
} from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";

type Props = {
  bridge: Bridge;
  open: boolean;
  onOpenChange: (open: boolean) => void;
};

/** Collect descendant ids (not including the node itself). Used by the
 *  "Auto-include children" cascade. */
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

export function ImportEmittersDialog({ bridge, open, onOpenChange }: Props) {
  const [sourcePath, setSourcePath] = useState<string | null>(null);
  const [tree, setTree] = useState<EmitterTreeNode | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [picks, setPicks] = useState<Set<number>>(() => new Set());
  const [autoChildren, setAutoChildren] = useState(true);

  // Reset state whenever the modal opens fresh so a previous session
  // doesn't bleed in (a closed-then-reopened modal should start empty).
  useEffect(() => {
    if (open) {
      setSourcePath(null);
      setTree(null);
      setError(null);
      setPicks(new Set());
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
          setPicks(new Set());
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

  /** Toggle a single id. If `autoChildren` is on AND we're checking
   *  (not unchecking), also tick every descendant of the toggled node. */
  const togglePick = (node: EmitterTreeNode, checked: boolean) => {
    setPicks((prev) => {
      const next = new Set(prev);
      if (checked) {
        next.add(node.id);
        if (autoChildren) {
          for (const id of descendantIds(node)) next.add(id);
        }
      } else {
        next.delete(node.id);
        if (autoChildren) {
          for (const id of descendantIds(node)) next.delete(id);
        }
      }
      return next;
    });
  };

  const handleSelectAll = () => setPicks(new Set(allIds));
  // Legacy IDC_IMPORT_CLEAR (main.cpp:7326): deselect every node.
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
    const checked = picks.has(node.id);
    return (
      <div key={node.id}>
        <label
          className="flex items-center gap-2 py-1 text-xs text-text"
          style={{ paddingLeft: `${depth * 16}px` }}
        >
          <input
            type="checkbox"
            checked={checked}
            onChange={(e) => togglePick(node, e.target.checked)}
            aria-label={`Select ${node.name}`}
            className="size-3 accent-sky-500"
          />
          <span>{node.name}</span>
        </label>
        {node.children.map((c) => renderNode(c, depth + 1))}
      </div>
    );
  };

  return (
    <Modal
      open={open}
      onOpenChange={onOpenChange}
      title="Import Emitters"
      size="lg"
    >
      <Modal.Body>
        <div className="space-y-3">
          {/* Source file row */}
          <div className="flex items-center gap-2">
            <span className="text-[11px] text-text-2">Source file:</span>
            <span className="flex-1 truncate rounded border border-border bg-bg px-2 py-1 text-xs text-text-2">
              {sourcePath ? basename(sourcePath) : "(not selected)"}
            </span>
            <button
              type="button"
              onClick={() => void handleBrowse()}
              aria-label="Browse for source file"
              className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 outline-none focus:border-accent"
            >
              Browse…
            </button>
          </div>

          {/* Tree / loading / empty / error states */}
          <div
            className="min-h-[160px] max-h-[280px] overflow-y-auto rounded border border-border bg-bg p-2"
            aria-label="Emitter tree"
          >
            {loading && (
              <div className="text-xs text-text-3">Loading preview…</div>
            )}
            {!loading && error && (
              <div className="text-xs text-amber-400">{error}</div>
            )}
            {!loading && !error && !tree && (
              <div className="text-xs text-text-3">
                Click Browse… to select a source .alo file.
              </div>
            )}
            {!loading && !error && tree && (
              <div>
                {/* Skip the synthetic root; render its children directly. */}
                {tree.children.map((c) => renderNode(c, 0))}
              </div>
            )}
          </div>

          {/* Auto-include children */}
          <label className="flex items-center gap-2 text-xs text-text">
            <input
              type="checkbox"
              checked={autoChildren}
              onChange={(e) => setAutoChildren(e.target.checked)}
              aria-label="Auto-include children"
              className="size-3 accent-sky-500"
            />
            <span>Auto-include children</span>
          </label>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <button
          type="button"
          onClick={handleSelectAll}
          disabled={!tree || allIds.length === 0}
          aria-label="Select all emitters"
          className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 outline-none focus:border-accent disabled:cursor-not-allowed disabled:opacity-50"
        >
          Select All
        </button>
        <button
          type="button"
          onClick={handleClear}
          disabled={picks.size === 0}
          aria-label="Clear selection"
          className="mr-auto rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 outline-none focus:border-accent disabled:cursor-not-allowed disabled:opacity-50"
        >
          Clear
        </button>
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
