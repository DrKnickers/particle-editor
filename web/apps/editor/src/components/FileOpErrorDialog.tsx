// FileOpErrorDialog — single App-level modal that shows a file-op failure
// message from useFileOpErrorStore. Mounted once in App.tsx.
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { useFileOpErrorStore } from "@/lib/file-op";

export function FileOpErrorDialog({ bridge }: { bridge: Bridge }) {
  const message = useFileOpErrorStore((s) => s.message);
  const title = useFileOpErrorStore((s) => s.title);
  const clear = useFileOpErrorStore((s) => s.clear);
  return (
    <Modal
      bridge={bridge}
      open={message !== null}
      onOpenChange={(o) => { if (!o) clear(); }}
      title={title ?? "Couldn't complete that"}
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
