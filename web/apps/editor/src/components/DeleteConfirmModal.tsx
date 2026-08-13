// DeleteConfirmModal — confirmation for destructive emitter deletes. Driven by
// useDeleteConfirmStore; mounted once in App.tsx with the bridge prop (the
// store holds only data, this component runs the actual delete on confirm).
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { useDeleteConfirmStore, confirmPendingDelete, type DeleteImpact } from "@/lib/delete-emitters";

function bodyText(ids: number[], impact: DeleteImpact): string {
  const n = ids.length;
  const total = impact.affectedCount;
  if (n === 1) {
    if (total <= 1) return `Delete "${impact.primaryName}"?`; // (defensive; non-destructive never reaches here)
    const kids = total - 1;
    return `Delete "${impact.primaryName}" and its ${kids} child emitter${kids === 1 ? "" : "s"}?`;
  }
  if (total === n) return `Delete ${n} emitters?`;
  return `Delete ${n} selected emitters and their children (${total} total)?`;
}

export function DeleteConfirmModal({ bridge }: { bridge: Bridge }) {
  const pending = useDeleteConfirmStore((s) => s.pending);
  const clear = useDeleteConfirmStore((s) => s.clear);

  const onDelete = () => {
    // confirmPendingDelete re-validates against the live tree (abort if it changed)
    // and clears the pending state itself.
    confirmPendingDelete(bridge);
  };

  return (
    <Modal
      open={pending !== null}
      onOpenChange={(o) => { if (!o) clear(); }}
      title="Delete emitters?"
      size="sm"
    >
      <Modal.Body>
        <p className="text-sm text-text-2">{pending ? bodyText(pending.ids, pending.impact) : ""}</p>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton autoFocus onClick={clear}>Cancel</Modal.CancelButton>
        <Modal.OkButton variant="danger" onClick={onDelete}>Delete</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
