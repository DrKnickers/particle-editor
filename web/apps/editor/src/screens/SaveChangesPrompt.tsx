// SaveChangesPrompt — three-button modal that gates destructive ops
// (New / Open / Recent) when the in-memory particle system is dirty.
//
// Mirrors the legacy `DoCheckChanges`
// (`MessageBox MB_YESNOCANCEL`) in the legacy main.cpp:
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
import { runFileOp } from "@/lib/file-op";
import { basename } from "@/lib/paths";

type Props = {
  bridge: Bridge;
};

/** Extract the basename from a full path. Cheap implementation: splits
 *  on the last `/` or `\\`. Falls back to the whole string for paths
 *  without a separator. Used in the body copy ("Save changes to
 *  foo.alo?"). */
export function SaveChangesPrompt({ bridge }: Props) {
  const pendingAction = useFileStateStore((s) => s.pendingAction);
  const currentFilePath = useFileStateStore((s) => s.currentFilePath);
  const setPendingAction = useFileStateStore((s) => s.setPendingAction);

  const open = pendingAction !== null;
  const fileLabel = currentFilePath ? basename(currentFilePath) : "this particle system";

  /** Run the pending closure and clear the slot. */
  const runPending = async () => {
    const action = useFileStateStore.getState().pendingAction;
    setPendingAction(null);
    if (action) await action();
  };

  const handleSave = async () => {
    // Attempt to save via runFileOp, which surfaces a non-cancel failure
    // (disk full / read-only / locked) in the error modal. On success, run the
    // pending New/Open and close. On ANY failure OR user-cancel, KEEP this prompt
    // open (do NOT clear pendingAction) and do NOT run the destructive pending op:
    // the unsaved work must survive, and the user can retry Save, Don't Save, or
    // Cancel from the still-open prompt (release-audit #11 — previously a failed
    // save silently closed the prompt and abandoned the pending op).
    try {
      const r = await runFileOp(bridge, { kind: "file/save", params: {} });
      if (r.ok) {
        await runPending();
      }
      // else: leave the prompt open; runFileOp already surfaced any real error.
    } catch {
      // Rejected save — runFileOp already populated the error store. Keep the
      // prompt open so the pending op never runs and the user can retry.
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
        <Modal.CancelButton onClick={handleCancel} aria-label="Cancel">Cancel</Modal.CancelButton>
        <Modal.OkButton
          variant="secondary"
          onClick={() =>
            // "Don't Save" runs the parked action (Ctrl+O/Ctrl+N), which may
            // reject; runFileOp already surfaces file failures, so swallow to
            // avoid an unhandled promise rejection (#489).
            void handleDiscard().catch((err) =>
              console.warn("[save-prompt] discard action failed:", err),
            )
          }
          aria-label="Don't Save"
        >
          Don&apos;t Save
        </Modal.OkButton>
        <Modal.OkButton onClick={() => void handleSave()} aria-label="Save">
          Save
        </Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
