// FileOpErrorModal — single App-level modal that shows a file-op failure
// message from useFileOpErrorStore. Mounted once in App.tsx.
import { Modal } from "@/components/Modal";
import { useFileOpErrorStore } from "@/lib/file-op";

export function FileOpErrorModal() {
  const message = useFileOpErrorStore((s) => s.message);
  const clear = useFileOpErrorStore((s) => s.clear);
  return (
    <Modal
      open={message !== null}
      onOpenChange={(o) => { if (!o) clear(); }}
      title="Couldn't complete that"
      size="sm"
    >
      <Modal.Body>
        <p className="whitespace-pre-line text-sm text-text-2">{message}</p>
      </Modal.Body>
      <Modal.Footer>
        <Modal.OkButton onClick={clear}>OK</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
