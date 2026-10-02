// CurveEditorToolbar — the curve editor's edit-affordance row (presentational):
// Select / Insert mode, Linear / Smooth / Step interpolation, snap-to-grid,
// Lock-to, the read-only lock cue, Delete, and the Time / Value spinners for
// the selected key(s). CurveEditorPanel owns every piece of state and passes
// it in; this component only lays it out.
//
// The toggles are IconButton's bordered "toggle" variant (aria-pressed →
// accent), Lock-to is the shared Select. Disabled buttons fire no pointer
// events, so the buttons whose tooltip matters while disabled (mode tools,
// Delete) ride `tipWhenDisabled`.

import type { ReactElement } from "react";
import { Lock, Magnet, MousePointer2, Plus, Trash2 } from "lucide-react";
import type { InterpolationType } from "@particle-editor/bridge-schema";
import { IconButton } from "@/primitives/IconButton";
import { Select } from "@/primitives/Select";
import { Spinner } from "@/primitives/Spinner";
import { Tip } from "@/primitives/Tip";

export type CurveEditMode = "select" | "insert";

const INTERP_KINDS: readonly InterpolationType[] = Object.freeze([
  "linear", "smooth", "step",
]);

/** Tiny inline glyphs for the three interpolation modes. Each is a
 *  16×16 SVG showing the curve shape between two endpoints — clearer
 *  than text at icon size, no lucide icon matches the semantics
 *  closely enough (lucide has `Spline` but not `Linear` / `Step`
 *  curve-editor glyphs). */
const INTERP_ICONS: Record<InterpolationType, ReactElement> = {
  linear: (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.75" aria-hidden="true">
      <line x1="2" y1="12" x2="14" y2="4" strokeLinecap="round" />
      <circle cx="2" cy="12" r="1.5" fill="currentColor" />
      <circle cx="14" cy="4" r="1.5" fill="currentColor" />
    </svg>
  ),
  smooth: (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.75" aria-hidden="true">
      <path d="M 2 12 C 5 12, 7 4, 14 4" strokeLinecap="round" />
      <circle cx="2" cy="12" r="1.5" fill="currentColor" />
      <circle cx="14" cy="4" r="1.5" fill="currentColor" />
    </svg>
  ),
  step: (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.75" aria-hidden="true">
      <polyline points="2,12 8,12 8,4 14,4" strokeLinecap="round" strokeLinejoin="round" />
      <circle cx="2" cy="12" r="1.5" fill="currentColor" />
      <circle cx="14" cy="4" r="1.5" fill="currentColor" />
    </svg>
  ),
};

const LOCKED_TIP = "Channel is locked — unlock to edit";

function Divider() {
  return <span className="mx-1 h-4 w-px bg-panel-2" aria-hidden />;
}

type SpinnerField = {
  /** Remount key — binds to track + selection size ONLY (see the panel). */
  resetKey: string;
  value: number;
  onChange: (value: number) => void;
  disabled: boolean;
};

type Props = {
  mode: CurveEditMode;
  onModeChange: (mode: CurveEditMode) => void;
  /** The focus channel is a read-only mirror of its Lock-to target. */
  focusLocked: boolean;
  focusLabel: string;
  interpolation: InterpolationType | null;
  interpDisabled: boolean;
  onInterpolation: (kind: InterpolationType) => void;
  snapEnabled: boolean;
  onToggleSnap: () => void;
  lockToValue: string;
  lockToOptions: readonly string[];
  lockToDisabled: boolean;
  onLockToChange: (value: string) => void;
  deleteDisabled: boolean;
  onDelete: () => void;
  time: SpinnerField;
  value: SpinnerField & { min: number; max: number; step: number };
};

export function CurveEditorToolbar({
  mode,
  onModeChange,
  focusLocked,
  focusLabel,
  interpolation,
  interpDisabled,
  onInterpolation,
  snapEnabled,
  onToggleSnap,
  lockToValue,
  lockToOptions,
  lockToDisabled,
  onLockToChange,
  deleteDisabled,
  onDelete,
  time,
  value,
}: Props) {
  return (
    // Lives above .ce-body so the `.ce-toolbar` rule in components.css (36px
    // row, padded, border-bottom) gives it the design's slot. With no emitter
    // selected the toolbar still renders but each control disables — the user
    // sees the affordance surface without it doing anything until a selection
    // lands.
    <div data-testid="curve-editor-toolbar" className="ce-toolbar">
      {/* Mode toggle (Select / Insert). Copy flips to the lock hint while the
          channel is locked, mirroring the Delete button pattern. */}
      <IconButton
        variant="toggle"
        label="Select tool"
        tip={focusLocked ? LOCKED_TIP : "Select (click a key to select; click empty area to clear)"}
        tipWhenDisabled
        pressed={mode === "select"}
        data-state={mode === "select" ? "on" : "off"}
        data-testid="ce-tool-select"
        disabled={focusLocked}
        onClick={() => onModeChange("select")}
      >
        <MousePointer2 className="size-3.5" aria-hidden="true" />
      </IconButton>
      <IconButton
        variant="toggle"
        label="Insert tool"
        tip={focusLocked ? LOCKED_TIP : "Insert (click empty canvas to add a key)"}
        tipWhenDisabled
        pressed={mode === "insert"}
        data-state={mode === "insert" ? "on" : "off"}
        data-testid="ce-tool-insert"
        disabled={focusLocked}
        onClick={() => onModeChange("insert")}
      >
        <Plus className="size-3.5" aria-hidden="true" />
      </IconButton>

      <Divider />

      {/* Interpolation toggle — applies to the focus channel's underlying
          track. */}
      {INTERP_KINDS.map((kind) => {
        const isActive = interpolation === kind;
        const label = kind[0]!.toUpperCase() + kind.slice(1);
        return (
          <IconButton
            key={kind}
            variant="toggle"
            label={`Interpolation ${kind}`}
            tip={`${label} interpolation`}
            pressed={isActive}
            data-state={isActive ? "on" : "off"}
            data-testid={`ce-interp-${kind}`}
            disabled={interpDisabled}
            onClick={() => onInterpolation(kind)}
          >
            {INTERP_ICONS[kind]}
          </IconButton>
        );
      })}

      <Divider />

      {/* Snap-to-grid toggle. A global editor preference — stays
          enabled even while the channel is locked (it changes future edits,
          not the current channel's data). */}
      <IconButton
        variant="toggle"
        label="Snap to grid"
        tip={snapEnabled ? "Snap to grid: on" : "Snap to grid: off"}
        pressed={snapEnabled}
        data-state={snapEnabled ? "on" : "off"}
        data-testid="curve-snap-toggle"
        onClick={onToggleSnap}
      >
        <Magnet className="size-3.5" aria-hidden="true" />
      </IconButton>

      <Divider />

      {/* Lock-to combo. Disabled only when the focus channel has no possible
          targets (Red / Scale / Index / Rotation — all of which can only be
          "None"). For Green/Blue/Alpha the dropdown stays enabled even while
          locked so the user can change the lock target or unlock. */}
      <label className="text-xs text-text-2" htmlFor="ce-lock-to-trigger">
        Lock to:&nbsp;
      </label>
      <Select
        size="sm"
        value={lockToValue}
        onValueChange={onLockToChange}
        options={lockToOptions.map((opt) => ({
          value: opt,
          label: opt,
          testId: `ce-lock-to-option-${opt.toLowerCase()}`,
        }))}
        placeholder="None"
        disabled={lockToDisabled}
        id="ce-lock-to-trigger"
        data-testid="ce-lock-to-trigger"
        data-locked={focusLocked ? "true" : "false"}
        aria-label="Lock-to track"
        className="min-w-[80px] data-[locked=true]:border-accent data-[locked=true]:text-accent"
        contentClassName="min-w-[120px]"
      />

      {/* Read-only mirror cue: a deliberate info glyph naming the master.
          The dashed curve is the in-canvas signal; this is the worded one.
          Non-interactive span (not a button) — the Tip carries the
          explanation; the aria-label is the always-on accessible name. */}
      {focusLocked && (
        <Tip content={`${focusLabel} is locked to ${lockToValue} and shows ${lockToValue}'s curve. Unlock to edit.`}>
          <span
            data-testid="ce-lock-glyph"
            role="img"
            aria-label={`${focusLabel} is locked to ${lockToValue} — read-only`}
            className="inline-flex h-6 items-center text-accent"
          >
            <Lock className="size-3.5" aria-hidden="true" />
          </span>
        </Tip>
      )}

      <Divider />

      {/* Delete action — useful as a visible affordance even with the Delete
          key wired (discoverability + works in browsers with the key
          intercepted by extensions). The "Select a non-border key first"
          hint only matters while the button is DISABLED. */}
      <IconButton
        variant="toggle"
        label="Delete selected keys"
        tip={deleteDisabled ? "Select a non-border key first" : "Delete selected key(s)"}
        tipWhenDisabled
        data-testid="ce-action-delete"
        disabled={deleteDisabled}
        onClick={onDelete}
        className={deleteDisabled ? undefined : "enabled:hover:border-danger enabled:hover:text-danger-fg"}
      >
        <Trash2 className="size-3.5" aria-hidden="true" />
      </IconButton>

      <div className="flex-1" />

      {/* Time / Value spinners — the focus channel's selected key, or the
          AVERAGE of the selected keys when >1 is selected. */}
      <label className="text-xs text-text-2" htmlFor="ce-spinner-time">Time:&nbsp;</label>
      <div style={{ width: 84 }} data-testid="ce-spinner-time-wrapper">
        <Spinner
          key={time.resetKey}
          aria-label="Selected key time"
          value={time.value}
          onChange={time.onChange}
          min={0}
          max={100}
          // Legacy used a 0.1 time step; wheel/arrows nudge in tenths of a
          // percent. Display picks up the app-wide 2dp default — decoupled
          // from step — so the Time field reads consistently with the Value
          // spinner beside it.
          step={0.1}
          unit="%"
          disabled={time.disabled}
          density="tight"
        />
      </div>
      <label className="text-xs text-text-2 ml-1" htmlFor="ce-spinner-value">Value:&nbsp;</label>
      <div style={{ width: 84 }} data-testid="ce-spinner-value-wrapper">
        <Spinner
          key={value.resetKey}
          aria-label="Selected key value"
          value={value.value}
          onChange={value.onChange}
          min={value.min}
          max={value.max}
          step={value.step}
          // Integer-grained tracks (Index, step 1) display as whole numbers;
          // fractional tracks (colour 0.01, scale/rotation 0.1) inherit the
          // 2dp default.
          decimals={value.step >= 1 ? 0 : undefined}
          disabled={value.disabled}
          density="tight"
        />
      </div>
    </div>
  );
}
