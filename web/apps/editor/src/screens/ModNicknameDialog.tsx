// ModNicknameDialog — single-text-input modal asking the user to name
// a mod's data directory. Phase 3 Screen 8 Batch 4.
//
// No menu trigger in this batch. Real auto-trigger (on file-load with
// an unknown mod-data path) is deferred to the file-load batch. The
// dialog is exposed via `usePromptModNickname()` from
// `lib/mod-nickname.ts` and a `?demo=mod-nickname` route gate in
// App.tsx so design checkpoints + Playwright can drive it standalone.
//
// Layout: title + explanation paragraph + labelled text input +
// Cancel/OK footer. OK disabled when the input is empty (trimmed).

import { useEffect, useState } from "react";
import { Modal } from "@/components/Modal";
import { useModNicknameStore } from "@/lib/mod-nickname";

export function ModNicknameDialog() {
  const open = useModNicknameStore((s) => s.open);
  const [value, setValue] = useState("");

  // Reset the input on every fresh open so a previous nickname doesn't
  // bleed in. Mirrors the ImportEmittersDialog pattern.
  useEffect(() => {
    if (open) setValue("");
  }, [open]);

  const dismiss = (result: string | null) => {
    const { resolver, setOpen, setResolver } = useModNicknameStore.getState();
    setOpen(false);
    setResolver(null);
    if (resolver) resolver(result);
  };

  const handleOk = () => {
    const trimmed = value.trim();
    if (!trimmed) return;
    dismiss(trimmed);
  };

  return (
    <Modal
      open={open}
      onOpenChange={(o) => {
        // Esc / overlay click / close glyph → Cancel.
        if (!o) dismiss(null);
      }}
      title="Set mod nickname"
      size="sm"
    >
      <Modal.Body>
        <p className="mb-3 text-xs text-text-2">
          Give this mod&apos;s data directory a human-readable name.
        </p>
        <label className="block text-xs text-text">
          <span className="mb-1 block text-[11px] text-text-2">
            Nickname:
          </span>
          <input
            type="text"
            value={value}
            onChange={(e) => setValue(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === "Enter") handleOk();
            }}
            aria-label="Mod nickname"
            className="w-full rounded border border-border-2 bg-bg-2 px-2 py-1 text-xs text-text outline-none focus:border-accent"
            autoFocus
            spellCheck={false}
          />
        </label>
      </Modal.Body>
      <Modal.Footer>
        <Modal.CancelButton onClick={() => dismiss(null)}>
          Cancel
        </Modal.CancelButton>
        <Modal.OkButton onClick={handleOk} disabled={value.trim() === ""}>
          OK
        </Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
