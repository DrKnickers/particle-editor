import { useState } from "react";
import { Modal } from "@/components/Modal";
import { applyMode, readStoredMode, type ThemeMode } from "@/lib/theme";
import { readConfirmDelete, writeConfirmDelete } from "@/lib/delete-emitters";

type Props = { open: boolean; onOpenChange: (open: boolean) => void };

const MODES: { value: ThemeMode; label: string }[] = [
  { value: "dark", label: "Dark" },
  { value: "light", label: "Light" },
  { value: "system", label: "System" },
];

export function PreferencesDialog({ open, onOpenChange }: Props) {
  const [mode, setMode] = useState<ThemeMode>(() => readStoredMode());
  const [confirmDelete, setConfirmDelete] = useState<boolean>(() => readConfirmDelete());
  const choose = (m: ThemeMode) => { setMode(m); applyMode(m); };
  return (
    <Modal open={open} onOpenChange={onOpenChange} title="Preferences" size="sm">
      <Modal.Body>
        <div className="flex flex-col gap-3 text-sm">
          <div className="text-text-2">Theme</div>
          <div role="radiogroup" aria-label="Theme" className="inline-flex rounded border border-border-2 bg-bg-2 p-0.5">
            {MODES.map((m) => (
              <button
                key={m.value}
                role="radio"
                aria-checked={mode === m.value}
                aria-label={m.label}
                onClick={() => choose(m.value)}
                className={`px-3 py-1 rounded text-xs ${mode === m.value ? "bg-accent-soft text-accent" : "text-text-3"}`}
              >
                {m.label}
              </button>
            ))}
          </div>
          <div className="flex items-center justify-between pt-1">
            <label htmlFor="pref-confirm-delete" className="text-text-2">
              Confirm before deleting emitters
            </label>
            <input
              id="pref-confirm-delete"
              type="checkbox"
              checked={confirmDelete}
              onChange={(e) => { setConfirmDelete(e.target.checked); writeConfirmDelete(e.target.checked); }}
              className="accent-[var(--accent)]"
            />
          </div>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.OkButton onClick={() => onOpenChange(false)}>Close</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
