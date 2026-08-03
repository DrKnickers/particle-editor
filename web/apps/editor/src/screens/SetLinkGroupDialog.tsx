// SetLinkGroupDialog.
//
// Modal opened from the EmitterTree context-menu "Set Link Group…"
// item. Two radio choices:
//
//   1. Create new group (default). Submits with `groupId: -1`; the
//      host picks the smallest unused positive uint32_t and assigns
//      every selected emitter to it.
//   2. Join existing group. Submits with the chosen `groupId`. The
//      <select> below the radio lists every distinct linkGroup value
//      currently present in the tree (>0, sorted ascending). The
//      radio + select are disabled when no existing groups exist.
//
// Operates on the React-side multi-selection — the bridge call's
// `ids` is the current `useEmitterSelectionStore` ids array. The
// dialog stays driven by the `tree-context` atom (extended with
// `"set-link-group"`); the right-click handler in
// EmitterTree.tsx promotes a non-multi-selected right-clicked row to
// a single-select before opening, so the selection-at-OK matches the
// row the user clicked when no Ctrl/Shift gesture preceded.

import { useEffect, useMemo, useState } from "react";
import type { Bridge, EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { useTreeContextStore } from "@/lib/tree-context";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";

type Props = {
  bridge: Bridge;
};

/** Walk the tree, gather every distinct linkGroup > 0, sort ascending. */
function collectExistingGroups(tree: EmitterTreeDto | null): number[] {
  if (tree === null) return [];
  const groups = new Set<number>();
  const visit = (n: EmitterTreeNode) => {
    if (n.linkGroup > 0) groups.add(n.linkGroup);
    n.children.forEach(visit);
  };
  visit(tree.root);
  return Array.from(groups).sort((a, b) => a - b);
}

export function SetLinkGroupDialog({ bridge }: Props) {
  const open = useTreeContextStore((s) => s.open === "set-link-group");
  const close = useTreeContextStore((s) => s.close);

  // The dialog operates on the React-side multi-selection. Snapshot it
  // at open time so a mid-dialog toggle doesn't surprise the user.
  const [selectedIds, setSelectedIds] = useState<number[]>([]);
  const [mode, setMode] = useState<"new" | "existing">("new");
  const [existingGroups, setExistingGroups] = useState<number[]>([]);
  const [chosenGroup, setChosenGroup] = useState<number | null>(null);
  // The non-exempt fields the chosen join would overwrite, shown
  // INLINE so the user sees them before a single OK commits — no separate
  // confirm step. Fetched reactively as the target (mode / group) changes;
  // read-only, never blocks the join.
  const [conflicts, setConflicts] = useState<{ id: number; fields: string[] }[]>([]);

  useEffect(() => {
    if (!open) return;
    // Capture selection at open.
    const ids = [...useEmitterSelectionStore.getState().ids];
    setSelectedIds(ids);
    setMode("new");
    setConflicts([]);
    // Fetch the live tree to extract existing groups.
    let cancelled = false;
    bridge
      .request({ kind: "emitters/list", params: {} })
      .then((tree) => {
        if (cancelled) return;
        const groups = collectExistingGroups(tree);
        setExistingGroups(groups);
        setChosenGroup(groups.length > 0 ? groups[0]! : null);
      })
      .catch(() => {
        setExistingGroups([]);
        setChosenGroup(null);
      });
    return () => {
      cancelled = true;
    };
  }, [open, bridge]);

  const hasExisting = existingGroups.length > 0;
  // Disable OK only when we'd submit "existing" but no groups exist
  // (defensive — the radio is also disabled in that case).
  const okDisabled = useMemo(() => {
    if (selectedIds.length === 0) return true;
    // A new group needs >= 2 members — the host's CreateLinkGroup silently
    // no-ops below 2, which read as "OK did nothing". Block it here so the
    // requirement is visible instead of failing silently.
    if (mode === "new" && selectedIds.length < 2) return true;
    if (mode === "existing" && chosenGroup === null) return true;
    return false;
  }, [mode, chosenGroup, selectedIds]);

  // Reactively preview which non-exempt fields the current target
  // (a new group, or the chosen existing group) would overwrite, so the
  // form can list them BEFORE the user commits. Read-only; the result only
  // drives the inline note — OK always proceeds. Re-runs when the target
  // changes (mode / chosen group / selection).
  useEffect(() => {
    if (!open || selectedIds.length === 0) { setConflicts([]); return; }
    if (mode === "existing" && chosenGroup === null) { setConflicts([]); return; }
    const groupId = mode === "new" ? -1 : chosenGroup!;
    let cancelled = false;
    bridge
      .request({ kind: "linkGroups/diff-membership", params: { ids: selectedIds, groupId } })
      .then((r) => { if (!cancelled) setConflicts(r?.conflicts ?? []); })
      .catch(() => { if (!cancelled) setConflicts([]); });
    return () => { cancelled = true; };
  }, [open, bridge, selectedIds, mode, chosenGroup]);

  // OK joins in ONE click. The inline note above already showed any field
  // disagreements, so there's no separate confirm step (legacy listed the
  // differing fields in the same dialog). Synchronous — no async gap that
  // could swallow the first click.
  const handleOk = () => {
    if (okDisabled) return;
    const groupId = mode === "new" ? -1 : chosenGroup!;
    void bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids: selectedIds, groupId },
    });
    close();
  };

  // Differing fields across all joiners (deduped) + how many emitters
  // would be overwritten — drives the inline note copy.
  const conflictFields = Array.from(new Set(conflicts.flatMap((c) => c.fields)));
  const conflictEmitterCount = conflicts.length;

  return (
    <Modal
      open={open}
      onOpenChange={(o) => { if (!o) close(); }}
      title="Set Link Group"
      size="sm"
    >
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm">
          <label className="flex items-center gap-2 text-text">
            <input
              type="radio"
              name="set-link-group-mode"
              value="new"
              checked={mode === "new"}
              onChange={() => setMode("new")}
              data-testid="set-link-group-radio-new"
            />
            <span>Create new group</span>
          </label>
          <label
            className={[
              "flex items-center gap-2",
              hasExisting ? "text-text" : "text-text-3",
            ].join(" ")}
          >
            <input
              type="radio"
              name="set-link-group-mode"
              value="existing"
              checked={mode === "existing"}
              onChange={() => setMode("existing")}
              disabled={!hasExisting}
              data-testid="set-link-group-radio-existing"
              className="disabled:opacity-40 disabled:cursor-not-allowed"
            />
            <span>Join existing group</span>
          </label>
          <select
            value={chosenGroup ?? ""}
            onChange={(e) => setChosenGroup(Number.parseInt(e.target.value, 10))}
            disabled={!hasExisting || mode !== "existing"}
            aria-label="Existing group to join"
            data-testid="set-link-group-select"
            className="ml-6 w-32 rounded border border-border-2 bg-bg px-2 py-1 text-sm text-text focus-ring disabled:cursor-not-allowed disabled:opacity-40"
          >
            {existingGroups.map((g) => (
              <option key={g} value={g}>
                Group {g}
              </option>
            ))}
            {!hasExisting && <option value="">(none)</option>}
          </select>
          {mode === "new" && selectedIds.length < 2 ? (
            <p className="text-[11px] font-medium leading-relaxed text-text-2">
              Select at least 2 emitters to create a group.
            </p>
          ) : (
            <p className="text-[11px] leading-relaxed text-text-3">
              All {selectedIds.length} selected
              {selectedIds.length === 1 ? " emitter" : " emitters"} will be linked.
            </p>
          )}
          {/* Inline disagreement note. Shows which shared fields the
              join would overwrite, so the user decides BEFORE the single OK
              — no separate confirm step. */}
          {conflictFields.length > 0 && (
            <div
              data-testid="link-conflict-inline"
              className="rounded border border-warning/60 bg-warning/15 px-2 py-1.5 text-[11px] leading-relaxed text-warning-fg"
            >
              <p className="font-medium">
                Joining overwrites {conflictFields.length}{" "}
                {conflictFields.length === 1 ? "field" : "fields"} on{" "}
                {conflictEmitterCount}{" "}
                {conflictEmitterCount === 1 ? "emitter" : "emitters"} with the
                group's values:
              </p>
              <p className="mt-0.5 font-medium text-warning-fg">
                {conflictFields.join(", ")}
              </p>
            </div>
          )}
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton>Cancel</Modal.CancelButton>
        <Modal.OkButton onClick={handleOk} disabled={okDisabled}>
          OK
        </Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
