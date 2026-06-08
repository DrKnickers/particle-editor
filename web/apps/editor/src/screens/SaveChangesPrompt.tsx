// SaveChangesPrompt — three-button modal that gates destructive ops
// (New / Open / Recent) when the in-memory particle system is dirty.
//
// Phase 3 Screen 8 Batch 3. Mirrors the legacy `DoCheckChanges`
// (`MessageBox MB_YESNOCANCEL`) at [src/main.cpp:1395-1409]:
//   - Save (Yes) → call file/save; if it succeeds, run the pending
//     action. If save was cancelled (ok:false), abort.
//   - Don't Save (No) → run the pending action immediately.
//   - Cancel → discard the pending action, close the prompt.
//
// The pending action is a closure stored in the file-state atom — see
// `usePromptSaveChanges()` in `lib/file-state.ts`. The prompt's open
// state is `pendingAction != null` so a caller anywhere in the tree
// can pop the prompt by setting a pending action.

import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { useFileStateStore } from "@/lib/file-state";

type Props = {
  bridge: Bridge;
};

/** Extract the basename from a full path. Cheap implementation: splits
 *  on the last `/` or `\\`. Falls back to the whole string for paths
 *  without a separator. Used in the body copy ("Save changes to
 *  foo.alo?"). */
function basename(path: string | null): string {
  if (!path) return "this particle system";
  const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return idx >= 0 ? path.slice(idx + 1) : path;
}

export function SaveChangesPrompt({ bridge }: Props) {
  const pendingAction = useFileStateStore((s) => s.pendingAction);
  const currentFilePath = useFileStateStore((s) => s.currentFilePath);
  const setPendingAction = useFileStateStore((s) => s.setPendingAction);

  const open = pendingAction !== null;
  const fileLabel = basename(currentFilePath);

  /** Run the pending closure and clear the slot. */
  const runPending = async () => {
    const action = useFileStateStore.getState().pendingAction;
    setPendingAction(null);
    if (action) await action();
  };

  const handleSave = async () => {
    // Attempt to save the current document. If the user cancels the
    // native save picker (ok:false), abort — don't run the pending
    // action. Matches legacy DoCheckChanges → DoSaveFile semantics.
    try {
      const r = await bridge.request({ kind: "file/save", params: {} });
      if (r.ok) {
        await runPending();
      } else {
        // Save cancelled / failed — keep the prompt closed but discard
        // the pending action so the user can decide afresh.
        setPendingAction(null);
      }
    } catch (err) {
      console.warn("[SaveChangesPrompt] file/save failed:", err);
      setPendingAction(null);
    }
  };

  const handleDiscard = async () => {
    await runPending();
  };

  const handleCancel = () => {
    setPendingAction(null);
  };

  return (
    <Modal
      open={open}
      onOpenChange={(o) => {
        // Esc / overlay click → treat as Cancel (discard pending).
        if (!o) setPendingAction(null);
      }}
      title="Save changes?"
      size="sm"
    >
      <Modal.Body>
        <p className="text-sm text-text-2">
          Do you want to save changes to{" "}
          <span className="font-medium text-text">{fileLabel}</span>?
        </p>
      </Modal.Body>
      <Modal.Footer>
        <button
          type="button"
          onClick={handleCancel}
          aria-label="Cancel"
          className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 outline-none focus:border-accent"
        >
          Cancel
        </button>
        <button
          type="button"
          onClick={() => void handleDiscard()}
          aria-label="Don't Save"
          className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-xs text-text hover:bg-panel-3 outline-none focus:border-accent"
        >
          Don&apos;t Save
        </button>
        <button
          type="button"
          onClick={() => void handleSave()}
          aria-label="Save"
          className="rounded bg-accent px-3 py-1 text-xs font-medium text-white hover:bg-accent outline-none focus:ring-2 focus:ring-accent"
        >
          Save
        </button>
      </Modal.Footer>
    </Modal>
  );
}
