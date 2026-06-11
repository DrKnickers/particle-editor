import { useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { applyMode, readStoredMode, type ThemeMode } from "@/lib/theme";
import { readConfirmDelete, writeConfirmDelete } from "@/lib/delete-emitters";
import {
  applyOverloadGuard,
  clampMaxParticles,
  readOverloadGuard,
  writeOverloadGuard,
  MIN_MAX_PARTICLES,
  MAX_MAX_PARTICLES,
  type OverloadGuardConfig,
} from "@/lib/overload-guard";

type Props = { bridge: Bridge; open: boolean; onOpenChange: (open: boolean) => void };

const MODES: { value: ThemeMode; label: string }[] = [
  { value: "dark", label: "Dark" },
  { value: "light", label: "Light" },
  { value: "system", label: "System" },
];

export function PreferencesDialog({ bridge, open, onOpenChange }: Props) {
  const [mode, setMode] = useState<ThemeMode>(() => readStoredMode());
  const [confirmDelete, setConfirmDelete] = useState<boolean>(() => readConfirmDelete());
  const choose = (m: ThemeMode) => { setMode(m); applyMode(m); };

  const [guard, setGuard] = useState<OverloadGuardConfig>(() => readOverloadGuard());
  // Draft string for the number field so partial typing ("2", "25") isn't
  // clamped/sent per keystroke — commit on blur/Enter only.
  const [capDraft, setCapDraft] = useState<string>(() => String(readOverloadGuard().maxParticles));

  const commitGuard = (next: OverloadGuardConfig) => {
    const clamped = { ...next, maxParticles: clampMaxParticles(next.maxParticles) };
    setGuard(clamped);
    setCapDraft(String(clamped.maxParticles));
    writeOverloadGuard(clamped);
    applyOverloadGuard(bridge, clamped);
  };
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
          {/* [guard-config] Preview overload guard. OFF is fully uncapped —
              the pre-#121 behavior that CAN OOM the editor; the warning
              line states the trade (autosave #41 is the backstop). */}
          <div className="flex flex-col gap-2 border-t border-border pt-3">
            <div className="text-text-2">Preview</div>
            <div className="flex items-center justify-between">
              <label htmlFor="pref-overload-guard" className="text-text-2">
                Limit preview particle count
              </label>
              <input
                id="pref-overload-guard"
                type="checkbox"
                checked={guard.enabled}
                onChange={(e) => commitGuard({ ...guard, enabled: e.target.checked })}
                className="accent-[var(--accent)]"
              />
            </div>
            <div className="flex items-center justify-between">
              <label
                htmlFor="pref-overload-max"
                className={guard.enabled ? "text-text-2" : "text-text-3"}
              >
                Max preview particles
              </label>
              <input
                id="pref-overload-max"
                type="number"
                aria-label="Max preview particles"
                disabled={!guard.enabled}
                value={capDraft}
                min={MIN_MAX_PARTICLES}
                max={MAX_MAX_PARTICLES}
                onChange={(e) => setCapDraft(e.target.value)}
                onBlur={() => commitGuard({ ...guard, maxParticles: Number(capDraft) })}
                onKeyDown={(e) => {
                  if (e.key === "Enter") commitGuard({ ...guard, maxParticles: Number(capDraft) });
                }}
                className="w-28 rounded border border-border-2 bg-bg px-2 py-1 text-right text-xs text-text disabled:opacity-50"
              />
            </div>
            {!guard.enabled && (
              <div className="text-[11px] text-warning">
                Unlimited spawning can crash the editor on extreme effects —
                unsaved changes are at risk.
              </div>
            )}
          </div>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.OkButton onClick={() => onOpenChange(false)}>Close</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
