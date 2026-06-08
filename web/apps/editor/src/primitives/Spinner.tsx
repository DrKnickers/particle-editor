// Spinner.tsx — numeric input primitive.
//
// Behaviors (ported from legacy src/UI/Spinner.cpp):
//   - Up/down arrow buttons: always visible (matches legacy Win32
//     UDS_ALIGNRIGHT spin button), increment/decrement by `step`.
//   - Scroll-wheel adjust (F7): wheel-up increments, wheel-down
//     decrements. Base step = the field's `step` (legacy Increment).
//     Shift = ×10 (coarse); Ctrl = ×0.1 (fine) on decimal fields, ignored
//     on whole-number fields so it never yields a fraction (Spinner.cpp:107-117).
//   - Drag-to-adjust (F6): vertical mouse-Y drag on the ARROW COLUMN
//     (not the text input — dragging the input selects text). Shift =
//     coarse (×10), Ctrl = fine — matching the wheel and keyboard arrows.
//   - Hold-to-repeat: pressing and holding an arrow button auto-repeats the
//     step after a short delay (legacy Spinner.cpp:438-455).
//   - Scientific notation parse: "1e-3", "2.5E4", etc.
//   - Range clamp: clamp to [min, max] on blur/commit; NOT on keystroke.
//   - Unit suffix: greyed-out text after the number.
//   - onChange fires on commit (Enter/blur/arrow/wheel/drag-release), NOT on
//     every keystroke. Avoids bridge spam from Screens 4/5/6.
//   - density: row height override per call ("tight"=22px, "default"=26px, "loose"=32px).

import { useEffect, useRef, useState, useCallback, type KeyboardEvent } from "react";

export type SpinnerDensity = "tight" | "default" | "loose";

const ROW_HEIGHT: Record<SpinnerDensity, string> = {
  tight: "22px",
  default: "26px",
  loose: "32px",
};

// F6: pixels of vertical movement on the arrow column before a press is
// treated as a value-scrub rather than a click.
const DRAG_THRESHOLD_PX = 3;

// Hold-to-repeat on the arrow buttons: initial delay before auto-repeat
// kicks in, then the interval between repeats (≈20/s). Mirrors the legacy
// keyboard-repeat cadence (Spinner.cpp:558-559).
const HOLD_DELAY_MS = 350;
const HOLD_REPEAT_MS = 50;

export type SpinnerProps = {
  value: number;
  onChange: (value: number) => void;
  min?: number;
  max?: number;
  step?: number;
  decimals?: number;
  unit?: string;
  density?: SpinnerDensity;
  disabled?: boolean;
  "aria-label"?: string;
};

function parseValue(raw: string): number | null {
  // Handles scientific notation (1e-3, 2.5E4) and plain numbers.
  const trimmed = raw.trim();
  if (trimmed === "" || trimmed === "-") return null;
  const n = Number(trimmed);
  return isFinite(n) ? n : null;
}

function clamp(v: number, min?: number, max?: number): number {
  if (min !== undefined && v < min) return min;
  if (max !== undefined && v > max) return max;
  return v;
}

export function Spinner({
  value,
  onChange,
  min,
  max,
  step = 1,
  decimals,
  unit,
  density = "default",
  disabled = false,
  "aria-label": ariaLabel,
}: SpinnerProps) {
  const height = ROW_HEIGHT[density];
  // Display decimal places. Default is 2 so every decimal-bearing field
  // renders consistently (e.g. "0.50", "45.00") regardless of its `step`.
  // Integer fields (particle counts, Index, colour channels, inverted
  // percents) opt out by passing `decimals={0}`. NOTE: display precision
  // is deliberately DECOUPLED from the wheel/step granularity below — a
  // field can show 2dp yet still nudge by whole units (e.g. angles step
  // 1° but display 45.00).
  const dp = decimals ?? 2;
  const fmt = (v: number) => v.toFixed(dp);
  // Wheel/keyboard "is this an integer-grained field?" test, derived from
  // `step` (NOT from `dp`). A whole-number step (≥1) nudges by 1 per wheel
  // notch; a fractional step nudges by 0.1. This matches the legacy
  // behaviour exactly (previously keyed on the step-derived `dp === 0`,
  // which is equivalent to `step >= 1`) while letting `dp` default to 2.
  const stepIsWhole = step >= 1;

  const [text, setText] = useState<string>(fmt(value));
  const [dragging, setDragging] = useState(false);

  const inputRef = useRef<HTMLInputElement>(null);
  const dragStartY = useRef(0);
  const dragStartValue = useRef(0);
  // Hold-to-repeat timers + a "currently held/scrubbing" guard so the
  // external-value resync effect doesn't clobber an in-flight ramp.
  const holdDelayTimer = useRef<number | undefined>(undefined);
  const holdRepeatTimer = useRef<number | undefined>(undefined);
  const repeatedRef = useRef(false);
  const holdingRef = useRef(false);
  const heldValue = useRef(0);

  // Keep displayed text in sync when value prop changes from outside
  // (but NOT during active text editing — we track that with isFocused).
  const isFocused = useRef(false);

  const commit = useCallback((raw: string, modifiers?: { shift?: boolean; ctrl?: boolean }) => {
    const parsed = parseValue(raw);
    if (parsed === null) {
      // Invalid input: revert.
      setText(fmt(value));
      return;
    }
    let final = parsed;
    if (modifiers) {
      // Modifier-adjusted steps aren't used in commit from text, only from
      // wheel/drag. But keep the hook consistent.
    }
    final = clamp(parsed, min, max);
    setText(fmt(final));
    if (final !== value) onChange(final);
  }, [value, onChange, min, max, fmt]);

  const adjustBy = useCallback((delta: number) => {
    const next = clamp(value + delta, min, max);
    setText(fmt(next));
    onChange(next);
  }, [value, onChange, min, max, fmt]);

  // Keyboard: Enter commits; arrow keys increment/decrement.
  const handleKeyDown = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === "Enter") {
      e.currentTarget.blur();
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      const s = e.shiftKey ? step * 10 : e.ctrlKey ? step / 10 : step;
      adjustBy(s);
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      const s = e.shiftKey ? step * 10 : e.ctrlKey ? step / 10 : step;
      adjustBy(-s);
    }
  };

  const handleBlur = () => {
    isFocused.current = false;
    commit(text);
  };

  const handleFocus = () => {
    isFocused.current = true;
    // Update text from value in case it was changed externally while unfocused.
    setText(fmt(value));
  };

  // Wheel handler — attached natively to the OUTER wrapper (not just
  // the input element) so the wheel works anywhere over the spinner:
  // hovering over the input *or* the up/down arrow column. Native
  // attachment with `{ passive: false }` is required because React
  // 18+ adds `wheel` listeners as PASSIVE at the delegated root,
  // which makes `preventDefault()` a no-op and lets the browser
  // scroll the parent pane before our handler runs.
  // The latest value/min/max/step/disabled are stashed in a ref so
  // the listener doesn't need to be re-bound on every render.
  const wrapRef = useRef<HTMLDivElement>(null);
  const wheelDepsRef = useRef({ value, min, max, step, stepIsWhole, disabled, onChange, fmt });
  wheelDepsRef.current = { value, min, max, step, stepIsWhole, disabled, onChange, fmt };
  useEffect(() => {
    const el = wrapRef.current;
    if (el === null) return;
    const onWheelNative = (e: WheelEvent) => {
      const d = wheelDepsRef.current;
      if (d.disabled) return;
      e.preventDefault();
      e.stopPropagation();
      // an-audit-finding: base step = the field's actual `step` (legacy Increment),
      // so a step=5 field nudges by 5 and a step=0.25 field by 0.25.
      // Shift = ×10 (coarse). Ctrl = ×0.1 (fine) on decimal fields only;
      // whole-number fields ignore Ctrl so the wheel never yields a fraction
      // (Spinner.cpp:107-117). Display precision (2dp default) is decoupled
      // from this nudge granularity.
      const base = d.step;
      const fine = d.stepIsWhole ? base : base / 10;
      const s = e.shiftKey ? base * 10 : e.ctrlKey ? fine : base;
      const delta = e.deltaY < 0 ? s : -s;
      // Round to kill float drift from repeated 0.1 additions
      // (0.1+0.1+0.1 = 0.30000000000000004).
      const next = clamp(Math.round((d.value + delta) * 1e6) / 1e6, d.min, d.max);
      setText(d.fmt(next));
      d.onChange(next);
    };
    el.addEventListener("wheel", onWheelNative, { passive: false });
    return () => el.removeEventListener("wheel", onWheelNative);
  }, []);

  // F6: value-scrub lives on the arrow column ONLY. The text input is a
  // plain field, so a horizontal drag across it selects text for partial
  // edits (the old behaviour scrubbed the value from the input and blocked
  // selection). A plain click on an arrow still steps by ±step (the
  // buttons' onClick); a vertical drag past the threshold scrubs
  // continuously. Shift = coarse (step*10), Ctrl = fine (step/10), to
  // match the keyboard arrows + wheel (see the scrub handler below).
  // `scrubbedRef` suppresses the trailing
  // click so a drag that ends on the button doesn't also step.
  const scrubbedRef = useRef(false);
  const clearHoldTimers = useCallback(() => {
    if (holdDelayTimer.current !== undefined) {
      clearTimeout(holdDelayTimer.current);
      holdDelayTimer.current = undefined;
    }
    if (holdRepeatTimer.current !== undefined) {
      clearInterval(holdRepeatTimer.current);
      holdRepeatTimer.current = undefined;
    }
  }, []);

  // Single unified press handler on the arrow COLUMN. A press can resolve to
  // one of three gestures: a quick click (one ±step on release), a hold
  // (auto-repeat after a delay), or a vertical scrub (drag past threshold).
  const handleArrowsMouseDown = (e: React.MouseEvent) => {
    if (disabled || e.button !== 0) return;
    // Keep the input's focus/caret (don't blur on arrow mousedown) and
    // suppress text selection while scrubbing.
    e.preventDefault();

    // Direction: the button under the pointer, falling back to the pressed
    // half of the column (top = up).
    const targetBtn = (e.target as HTMLElement).closest("button");
    const aria = targetBtn?.getAttribute("aria-label");
    let dir = aria === "Decrement" ? -1 : aria === "Increment" ? 1 : 0;
    if (dir === 0) {
      const rect = e.currentTarget.getBoundingClientRect();
      dir = e.clientY < rect.top + rect.height / 2 ? 1 : -1;
    }

    dragStartY.current = e.clientY;
    dragStartValue.current = value;
    heldValue.current = value;
    scrubbedRef.current = false;
    repeatedRef.current = false;
    holdingRef.current = true;

    // Arm hold-to-repeat. The interval ramps from a local accumulator so it
    // keeps stepping even before the controlled value prop echoes back.
    holdDelayTimer.current = window.setTimeout(() => {
      repeatedRef.current = true;
      holdRepeatTimer.current = window.setInterval(() => {
        const next = clamp(heldValue.current + dir * step, min, max);
        heldValue.current = next;
        setText(fmt(next));
        onChange(next);
      }, HOLD_REPEAT_MS);
    }, HOLD_DELAY_MS);

    const onMove = (me: MouseEvent) => {
      const dy = dragStartY.current - me.clientY; // up = positive = increase
      if (!scrubbedRef.current) {
        if (Math.abs(dy) < DRAG_THRESHOLD_PX) return;
        scrubbedRef.current = true;
        clearHoldTimers(); // a drag cancels the hold-repeat
        setDragging(true);
      }
      // Shift = coarse (×10), Ctrl = fine — matching the wheel/keyboard.
      // Whole-number fields ignore Ctrl so a scrub never yields a fraction.
      const fine = stepIsWhole ? step : step / 10;
      const s = me.shiftKey ? step * 10 : me.ctrlKey ? fine : step;
      const next = clamp(
        Math.round((dragStartValue.current + dy * s) * 1e6) / 1e6,
        min,
        max,
      );
      setText(fmt(next));
      // Fire onChange during drag (each px move fires); drag-release will
      // fire again on the final value. Callers that debounce are fine.
      onChange(next);
    };

    const onUp = () => {
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp);
      clearHoldTimers();
      holdingRef.current = false;
      setDragging(false);
      // Quick click: neither a scrub nor a hold-repeat fired → one step.
      if (!scrubbedRef.current && !repeatedRef.current) {
        adjustBy(dir * step);
      }
    };

    document.addEventListener("mousemove", onMove);
    document.addEventListener("mouseup", onUp);
  };

  // Clear any pending hold timers on unmount.
  useEffect(() => clearHoldTimers, [clearHoldTimers]);

  // Keep text in sync when prop changes from outside (not while editing).
  // Effect runs post-commit so the displayed value reflects external
  // updates (e.g. undo, mod-switch, parent rerender with a transformed
  // value like FieldSpinner's displayInvertedPercent) without requiring
  // the user to focus the input first. Guarded on isFocused/dragging so
  // mid-edit typing is never clobbered. Deps are kept primitive (value,
  // dp, dragging) so the effect doesn't run on every render and clobber
  // in-flight `setText` from `onChange`.
  useEffect(() => {
    if (isFocused.current || dragging || holdingRef.current) return;
    const expected = value.toFixed(dp);
    setText((prev) => (prev === expected ? prev : expected));
  }, [value, dp, dragging]);

  // Always-visible Win32-style up/down arrow column. Reserve 14px on
  // the right so digits don't sit underneath; if a unit is also
  // present, push the unit left of the arrow column too.
  const ARROW_W = 14;
  const unitPad = unit ? unit.length * 7 + 6 : 0; // ~7px per char + a hair
  const inputPadRight = ARROW_W + unitPad + 4;    // arrows + unit + breathing room

  return (
    <div
      ref={wrapRef}
      className={`relative flex items-center ${dragging ? "cursor-ns-resize" : ""}`}
      style={{ height }}
    >
      <input
        ref={inputRef}
        type="text"
        value={text}
        disabled={disabled}
        aria-label={ariaLabel}
        onChange={(e) => setText(e.target.value)}
        onKeyDown={handleKeyDown}
        onBlur={handleBlur}
        onFocus={handleFocus}
        className={`w-full rounded border border-border-2 bg-bg-2 pl-2 text-xs text-text outline-none transition focus:border-accent ${
          disabled ? "cursor-not-allowed opacity-40" : "cursor-text"
        } ${dragging ? "select-none" : ""}`}
        style={{ height, paddingRight: `${inputPadRight}px` }}
        spellCheck={false}
        autoComplete="off"
      />
      {/* Unit suffix — positioned to the left of the arrow column. */}
      {unit && (
        <span
          className="pointer-events-none absolute text-xs text-text-3"
          style={{ right: `${ARROW_W + 4}px`, top: "50%", transform: "translateY(-50%)" }}
          aria-hidden="true"
        >
          {unit}
        </span>
      )}
      {/* Up/down arrow column — always visible, mirrors Win32 spin
          button. Disabled state fades them to match the input. F6: also
          the value-scrub affordance — mousedown here starts a drag-scrub
          (ns-resize cursor); a plain click on a button steps by ±step. */}
      <div
        onMouseDown={handleArrowsMouseDown}
        className={`absolute right-0 top-0 flex flex-col border-l border-border-2 ${disabled ? "opacity-40" : "cursor-ns-resize"}`}
        style={{ height, width: `${ARROW_W}px` }}
        aria-hidden={disabled}
      >
        <button
          type="button"
          tabIndex={-1}
          disabled={disabled}
          aria-label="Increment"
          className="flex flex-1 items-center justify-center text-text-3 hover:bg-panel-2 hover:text-text disabled:cursor-not-allowed"
          style={{ fontSize: "7px", lineHeight: 1 }}
        >
          ▲
        </button>
        <button
          type="button"
          tabIndex={-1}
          disabled={disabled}
          aria-label="Decrement"
          className="flex flex-1 items-center justify-center text-text-3 hover:bg-panel-2 hover:text-text disabled:cursor-not-allowed"
          style={{ fontSize: "7px", lineHeight: 1 }}
        >
          ▼
        </button>
      </div>
    </div>
  );
}
