// AtlasConfirmModal — confirmation dialog for setting a new frame index
// when multiple selected keys have differing current values (Task 9).
//
// Uses the shared Modal compound-component API:
//   <Modal open onOpenChange title size="sm">
//     <Modal.Body>…</Modal.Body>
//     <Modal.Footer>
//       <Modal.CancelButton />
//       <Modal.OkButton>Set all</Modal.OkButton>
//     </Modal.Footer>
//   </Modal>
//
// Modal.CancelButton wraps Dialog.Close (auto-closes on click).
// Modal.OkButton does NOT auto-close — onConfirm must close externally.

import { Modal } from "@/components/Modal";

export function AtlasConfirmModal({
  open,
  count,
  frame,
  onConfirm,
  onCancel,
}: {
  open: boolean;
  count: number;
  frame: number;
  onConfirm: () => void;
  onCancel: () => void;
}) {
  return (
    <Modal
      open={open}
      onOpenChange={(o) => { if (!o) onCancel(); }}
      title="Set frame for all selected keys"
      size="sm"
    >
      <Modal.Body>
        <p className="text-sm text-text-2">
          These {count} keys have different frames. Set all to frame {frame}?
        </p>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton onClick={onCancel} />
        <Modal.OkButton onClick={onConfirm}>Set all</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
