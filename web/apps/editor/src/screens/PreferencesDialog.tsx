import { useEffect, useState, type ReactNode } from "react";
import { Check, ChevronDown, ChevronUp, AlertTriangle } from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { Modal } from "@/components/Modal";
import { cn } from "@/lib/utils";
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
import { applySoftShadows, readSoftShadows, writeSoftShadows } from "@/lib/soft-shadows";

type Props = { bridge: Bridge; open: boolean; onOpenChange: (open: boolean) => void };

const MODES: { value: ThemeMode; label: string }[] = [
  { value: "dark", label: "Dark" },
  { value: "light", label: "Light" },
  { value: "system", label: "System" },
];

// ── Design-handoff section card (2026-06-19 modal redesign) ──────────────
// A grouped settings panel: a `bg` card with a `bg-2` uppercase header bar —
// real hierarchy instead of a hairline top-border. Rows live inside.
function SectionCard({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="overflow-hidden rounded-lg border border-border bg-bg">
      <div className="border-b border-border bg-bg-2 px-3 py-2 text-[11px] font-semibold uppercase tracking-[0.04em] text-text-2">
        {title}
      </div>
      {children}
    </div>
  );
}

// A real <input type="checkbox"> (kept for role/label/keyboard + tests) made
// to look like the mockup's 14px box: the native input is visually hidden but
// still the accessible control; a sibling <span> draws the box + tick from the
// `checked` prop. The wrapping <label> is the click target; `aria-label` (not
// the row title element) is the accessible name so getByLabelText resolves to
// exactly the title.
function CheckToggle({
  id,
  label,
  checked,
  onChange,
  disabled,
}: {
  id: string;
  label: string;
  checked: boolean;
  onChange: (e: React.ChangeEvent<HTMLInputElement>) => void;
  disabled?: boolean;
}) {
  return (
    <label className={cn("relative inline-flex size-[14px] shrink-0", disabled ? "cursor-not-allowed" : "cursor-pointer")}>
      <input
        id={id}
        type="checkbox"
        aria-label={label}
        checked={checked}
        disabled={disabled}
        onChange={onChange}
        className="peer sr-only"
      />
      <span
        aria-hidden
        className={cn(
          "flex size-[14px] items-center justify-center rounded-[3px] border transition-colors peer-focus-visible:outline peer-focus-visible:outline-2 peer-focus-visible:outline-offset-1 peer-focus-visible:outline-[var(--accent)]",
          checked ? "border-accent bg-accent" : "border-border-2 bg-bg-3",
        )}
      >
        {checked && <Check className="size-2.5 text-white" strokeWidth={3} />}
      </span>
    </label>
  );
}

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
  // Stepper ▲/▼ base = the value currently VISIBLE in the field (the draft),
  // falling back to the committed value only when the draft is blank/non-numeric.
  // Stepping off `guard.maxParticles` would silently discard a typed-but-
  // uncommitted edit (clampMaxParticles still bounds the result).
  const stepCap = (delta: number) => {
    const parsed = Number(capDraft);
    const base = capDraft.trim() === "" || !Number.isFinite(parsed) ? guard.maxParticles : parsed;
    commitGuard({ ...guard, maxParticles: base + delta });
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

  // "Soft shadows": blurs the stencil shadow edges to match the game's
  // soft-shadow rendering (default on). Persisted in localStorage; applied
  // to the engine live. Depends on Model shadows (the dependent row dims +
  // disables when Model shadows is off — but the stored soft preference is
  // never overwritten by toggling the parent).
  const [softShadows, setSoftShadows] = useState<boolean>(() => readSoftShadows());
  const commitSoftShadows = (enabled: boolean) => {
    setSoftShadows(enabled);
    writeSoftShadows(enabled);
    applySoftShadows(bridge, enabled);
  };

  return (
    <Modal open={open} onOpenChange={onOpenChange} title="Preferences" size="md">
      <Modal.Body>
        <div className="flex flex-col gap-2.5">

          {/* APPEARANCE */}
          <SectionCard title="Appearance">
            <div className="flex items-center justify-between gap-3 px-3 py-[9px]">
              <span className="text-xs text-text">Theme</span>
              <div
                role="radiogroup"
                aria-label="Theme"
                className="inline-flex gap-0.5 rounded-[5px] border border-border-2 bg-bg-2 p-0.5"
              >
                {MODES.map((m) => (
                  <button
                    key={m.value}
                    role="radio"
                    aria-checked={mode === m.value}
                    aria-label={m.label}
                    onClick={() => choose(m.value)}
                    className={cn(
                      "rounded-[3px] px-[11px] py-[3px] text-[11px] transition-colors focus-ring-inset",
                      mode === m.value
                        ? "bg-accent-soft font-semibold text-accent"
                        : "text-text-3 hover:text-text-2",
                    )}
                  >
                    {m.label}
                  </button>
                ))}
              </div>
            </div>
          </SectionCard>

          {/* EDITING */}
          <SectionCard title="Editing">
            <div className="flex items-start justify-between gap-3 px-3 py-[9px]">
              <div className="flex min-w-0 flex-col gap-0.5">
                <span className="text-xs text-text">Confirm before deleting emitters</span>
                <span className="text-[11px] leading-snug text-text-3">
                  Shows a confirmation dialog before an emitter is removed.
                </span>
              </div>
              <CheckToggle
                id="pref-confirm-delete"
                label="Confirm before deleting emitters"
                checked={confirmDelete}
                onChange={(e) => { setConfirmDelete(e.target.checked); writeConfirmDelete(e.target.checked); }}
              />
            </div>
          </SectionCard>

          {/* PREVIEW */}
          {/* [guard-config] Preview overload guard. OFF is fully uncapped —
              the pre-#121 behavior that CAN OOM the editor; the warning
              line states the trade (autosave #41 is the backstop). */}
          <SectionCard title="Preview">
            <div className="flex items-center justify-between gap-3 px-3 py-[9px]">
              <span className="text-xs text-text">Limit preview particle count</span>
              <CheckToggle
                id="pref-overload-guard"
                label="Limit preview particle count"
                checked={guard.enabled}
                onChange={(e) => commitGuard({ ...guard, enabled: e.target.checked })}
              />
            </div>
            {/* dependent: nested under the toggle, dimmed + disabled when off */}
            <div
              className={cn(
                "mb-[9px] ml-[26px] mr-3 flex items-center justify-between gap-3 border-l border-border-2 pl-3",
                guard.enabled ? "" : "opacity-50",
              )}
            >
              <label
                htmlFor="pref-overload-max"
                className={guard.enabled ? "text-xs text-text" : "text-xs text-text-3"}
              >
                Max preview particles
              </label>
              <div className="flex h-[22px] w-28 items-center overflow-hidden rounded-[5px] border border-border-2 bg-bg-3">
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
                  className="min-w-0 flex-1 border-none bg-transparent px-2 text-right text-xs tabular-nums text-text outline-none [appearance:textfield] disabled:cursor-not-allowed [&::-webkit-inner-spin-button]:appearance-none [&::-webkit-outer-spin-button]:appearance-none"
                />
                <div className="flex flex-col self-stretch border-l border-border-2">
                  <button
                    type="button"
                    aria-label="Increase max preview particles"
                    tabIndex={-1}
                    disabled={!guard.enabled}
                    onClick={() => stepCap(1000)}
                    className="flex h-[11px] w-[17px] items-center justify-center text-text-3 hover:bg-hover hover:text-text disabled:pointer-events-none"
                  >
                    <ChevronUp className="size-2" strokeWidth={2} />
                  </button>
                  <button
                    type="button"
                    aria-label="Decrease max preview particles"
                    tabIndex={-1}
                    disabled={!guard.enabled}
                    onClick={() => stepCap(-1000)}
                    className="flex h-[11px] w-[17px] items-center justify-center border-t border-border-2 text-text-3 hover:bg-hover hover:text-text disabled:pointer-events-none"
                  >
                    <ChevronDown className="size-2" strokeWidth={2} />
                  </button>
                </div>
              </div>
            </div>
            {!guard.enabled && (
              <div className="mx-3 mb-[11px] flex items-start gap-2 rounded-md border border-warning/30 bg-warning/10 px-[9px] py-[7px]">
                <AlertTriangle className="mt-px size-3 shrink-0 text-warning-fg" strokeWidth={1.5} />
                <span className="text-[11px] leading-snug text-warning-fg">
                  Unlimited spawning can crash the editor on extreme effects —
                  unsaved changes are at risk.
                </span>
              </div>
            )}
          </SectionCard>

          {/* RENDERING */}
          {/* Antialiasing: hardware-gated MSAA level. The select is
              disabled while the query is in-flight (msaaLevels===null). */}
          <SectionCard title="Rendering">
            <div className="flex items-center justify-between gap-3 px-3 py-[9px]">
              <label htmlFor="pref-msaa-level" className="text-xs text-text">Antialiasing</label>
              <div className="relative h-[22px]">
                <select
                  id="pref-msaa-level"
                  aria-label="Antialiasing"
                  value={msaaLevel}
                  disabled={msaaLevels === null}
                  onChange={(e) => commitMsaaLevel(Number(e.target.value) as MsaaLevel)}
                  className="h-[22px] min-w-[104px] cursor-pointer appearance-none rounded-[5px] border border-border-2 bg-bg-3 pl-2 pr-6 text-xs text-text focus-ring disabled:cursor-not-allowed disabled:opacity-40"
                >
                  {(msaaLevels ?? [msaaLevel]).map((lvl) => (
                    <option key={lvl} value={lvl}>
                      {lvl === 0 ? "Off" : `${lvl}× MSAA`}
                    </option>
                  ))}
                </select>
                <ChevronDown
                  aria-hidden
                  className="pointer-events-none absolute right-[7px] top-1/2 size-2.5 -translate-y-1/2 text-text-3"
                  strokeWidth={1.6}
                />
              </div>
            </div>
            <div className="flex items-start justify-between gap-3 border-t border-border px-3 py-[9px]">
              <div className="flex min-w-0 flex-col gap-0.5">
                <span className="text-xs text-text">Smooth skydome seams</span>
                <span className="text-[11px] leading-snug text-text-3">
                  Hides the seam baked into stock skydome textures by re-mapping the dome.
                  Off shows the dome exactly as the game does (with the seam).
                </span>
              </div>
              <CheckToggle
                id="pref-skydome-seam-fix"
                label="Smooth skydome seams"
                checked={seamFix}
                onChange={(e) => commitSeamFix(e.target.checked)}
              />
            </div>
            <div className="flex items-start justify-between gap-3 border-t border-border px-3 py-[9px]">
              <div className="flex min-w-0 flex-col gap-0.5">
                <span className="text-xs text-text">Model shadows</span>
                <span className="text-[11px] leading-snug text-text-3">
                  Casts the game's stencil shadow for the reference model onto the ground (and self-shadows it).
                </span>
              </div>
              <CheckToggle
                id="pref-model-shadows"
                label="Model shadows"
                checked={modelShadows}
                onChange={(e) => commitModelShadows(e.target.checked)}
              />
            </div>
            {/* dependent: Soft shadows nests under Model shadows */}
            <div
              className={cn(
                "mb-[10px] ml-[26px] mr-3 flex items-start justify-between gap-3 border-l border-border-2 pl-3 pt-[9px]",
                modelShadows ? "" : "opacity-45",
              )}
            >
              <div className="flex min-w-0 flex-col gap-0.5">
                <span className="text-xs text-text">Soft shadows</span>
                <span className="text-[11px] leading-snug text-text-3">
                  Softens (blurs) the model shadow&apos;s edges, matching the game; off = hard-edged.
                </span>
              </div>
              <CheckToggle
                id="pref-soft-shadows"
                label="Soft shadows"
                checked={softShadows}
                disabled={!modelShadows}
                onChange={(e) => commitSoftShadows(e.target.checked)}
              />
            </div>
          </SectionCard>
        </div>
      </Modal.Body>
      <Modal.Footer>
        <Modal.OkButton onClick={() => onOpenChange(false)}>Close</Modal.OkButton>
      </Modal.Footer>
    </Modal>
  );
}
