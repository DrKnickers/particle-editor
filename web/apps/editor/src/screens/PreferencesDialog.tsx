import { useEffect, useState } from "react";
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
import {
  applyMsaaLevel,
  queryMsaaLevels,
  readMsaaLevel,
  writeMsaaLevel,
  type MsaaLevel,
} from "@/lib/msaa-quality";
import { applySkydomeSeamFix, readSkydomeSeamFix, writeSkydomeSeamFix } from "@/lib/skydome-seam-fix";
import { applyModelShadows, readModelShadows, writeModelShadows } from "@/lib/model-shadows";

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

  // Antialiasing: hardware-gated level list from the engine; persisted
  // selection in localStorage. Seeded with null until the query resolves
  // so we only show what the GPU actually supports.
  const [msaaLevels, setMsaaLevels] = useState<number[] | null>(null);
  const [msaaLevel, setMsaaLevel] = useState<MsaaLevel>(() => readMsaaLevel());

  useEffect(() => {
    let cancelled = false;
    void queryMsaaLevels(bridge).then(({ levels, current }) => {
      if (cancelled) return;
      // Unknown/failed query (levels === []) — leave everything as-is.
      // Do NOT collapse to "Off-only", do NOT persist, do NOT send.
      if (levels.length === 0) return;
      // Filter to only the valid set to avoid unvalidated casts downstream.
      const validLevels = levels.filter((l): l is MsaaLevel =>
        l === 0 || l === 2 || l === 4 || l === 8,
      );
      if (validLevels.length === 0) return;
      setMsaaLevels(validLevels);
      // Reconcile displayed value — READ-ONLY: never writeMsaaLevel or
      // applyMsaaLevel here.  Priority: saved (if still offered) → engine's
      // authoritative current (if known and offered) → Off (floor).
      const saved = readMsaaLevel();
      const displayed: MsaaLevel = validLevels.includes(saved)
        ? saved
        : current >= 0 && validLevels.includes(current as MsaaLevel)
          ? (current as MsaaLevel)
          : 0;
      setMsaaLevel(displayed);
    });
    return () => { cancelled = true; };
  }, [bridge]);

  const commitMsaaLevel = (level: MsaaLevel) => {
    setMsaaLevel(level);
    writeMsaaLevel(level);
    applyMsaaLevel(bridge, level);
  };

  // "Smooth skydome seams": re-maps the dome UVs to hide the asset's closure
  // seam (default on). Persisted in localStorage; applied to the engine live.
  const [seamFix, setSeamFix] = useState<boolean>(() => readSkydomeSeamFix());
  const commitSeamFix = (enabled: boolean) => {
    setSeamFix(enabled);
    writeSkydomeSeamFix(enabled);
    applySkydomeSeamFix(bridge, enabled);
  };

  // "Model shadows": casts the game's stencil shadow for the reference model
  // onto the ground (and self-shadows it) (default on). Persisted in
  // localStorage; applied to the engine live.
  const [modelShadows, setModelShadows] = useState<boolean>(() => readModelShadows());
  const commitModelShadows = (enabled: boolean) => {
    setModelShadows(enabled);
    writeModelShadows(enabled);
    applyModelShadows(bridge, enabled);
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
          {/* Antialiasing: hardware-gated MSAA level. The select is
              disabled while the query is in-flight (msaaLevels===null). */}
          <div className="flex flex-col gap-2 border-t border-border pt-3">
            <div className="text-text-2">Rendering</div>
            <div className="flex items-center justify-between">
              <label htmlFor="pref-msaa-level" className="text-text-2">
                Antialiasing
              </label>
              <select
                id="pref-msaa-level"
                aria-label="Antialiasing"
                value={msaaLevel}
                disabled={msaaLevels === null}
                onChange={(e) => commitMsaaLevel(Number(e.target.value) as MsaaLevel)}
                className="rounded border border-border-2 bg-bg px-2 py-1 text-xs text-text disabled:opacity-50"
              >
                {(msaaLevels ?? [msaaLevel]).map((lvl) => (
                  <option key={lvl} value={lvl}>
                    {lvl === 0 ? "Off" : `${lvl}× MSAA`}
                  </option>
                ))}
              </select>
            </div>
            <div className="flex items-center justify-between">
              <label htmlFor="pref-skydome-seam-fix" className="text-text-2">
                Smooth skydome seams
              </label>
              <input
                id="pref-skydome-seam-fix"
                type="checkbox"
                checked={seamFix}
                onChange={(e) => commitSeamFix(e.target.checked)}
                className="accent-[var(--accent)]"
              />
            </div>
            <div className="text-[11px] text-text-3">
              Hides the seam baked into stock skydome textures by re-mapping the dome.
              Off shows the dome exactly as the game does (with the seam).
            </div>
            <div className="flex items-center justify-between">
              <label htmlFor="pref-model-shadows" className="text-text-2">
                Model shadows
              </label>
              <input
                id="pref-model-shadows"
                type="checkbox"
                checked={modelShadows}
                onChange={(e) => commitModelShadows(e.target.checked)}
                className="accent-[var(--accent)]"
              />
            </div>
            <div className="text-[11px] text-text-3">
              Casts the game's stencil shadow for the reference model onto the ground (and self-shadows it).
            </div>
          </div>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.OkButton onClick={() => onOpenChange(false)}>Close</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
