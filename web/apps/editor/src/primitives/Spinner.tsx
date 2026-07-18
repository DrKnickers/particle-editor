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
  /** Optional test id. When set, stamps the increment/decrement arrow buttons
   *  as `${testId}-inc` / `${testId}-dec` so a --record clip (or a test) can
   *  click a specific spinner's arrow. Off by default — most spinners don't
   *  need it, and the record cursor targets by testid only. */
  testId?: string;
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
  testId,
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
  // The pointerId that started the current gesture, so a SECOND pointer (a
  // second touch/pen moving or releasing elsewhere in the document) can't hijack
  // or terminate this scrub. null = the gesture began from a plain mouse/jsdom
  // mousedown (no pointerId) — those are single-pointer, so no filtering needed.
  const activePointerId = useRef<number | null>(null);
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
  // Whether the user has actually TYPED into the field since focusing. A bare
  // focus (click in, then click away) leaves this false. It gates two things so
  // a focused-but-untouched field can never go stale or clobber the live value:
  //   - the external-resync effect still updates the text while focused (so a
  //     gizmo drag moving the reference object shows up in the spinner), and
  //   - blur only commits the text when the user genuinely edited it.
  // Set on keystroke (input onChange); cleared on focus/blur.
  const edited = useRef(false);
  // Latest `value` prop, readable from event-handler closures (wheel/scrub/hold)
  // without re-binding them every render.
  const valueRef = useRef(value);
  valueRef.current = value;
  // Latest `onChange` prop, readable from the scrub/hold document listeners that
  // are bound ONCE at mousedown. Without this, a continuous gesture keeps firing
  // the onChange captured at press time; when the consumer recreates its handler
  // per committed value (CurveEditorPanel does, keyed on the selected key) the
  // gesture carries a stale closure and diverges after the first tick (#614).
  // The wheel handler already does this via wheelDepsRef.
  const onChangeRef = useRef(onChange);
  onChangeRef.current = onChange;
  // Optimistic-echo guard. When the field emits a new value via onChange, the
  // controlled `value` prop can lag before it reflects it — a bridge-backed
  // caller (the reference-object transform) round-trips through the host and
  // echoes back asynchronously. `pendingBase` records the prop value at emit
  // time; while the prop is still that baseline (genuinely awaiting the echo)
  // the resync effect keeps the optimistic text rather than flashing back to
  // the stale value for a frame. Any real prop movement — the echo or a fresh
  // external change (a gizmo drag) — reconciles immediately. null = nothing
  // pending. Comparing against the BASELINE (not the emitted value) also
  // sidesteps float-ULP differences between what we sent and what echoes back.
  const pendingBase = useRef<number | null>(null);

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
    if (final !== value) { pendingBase.current = valueRef.current; onChange(final); }
  }, [value, onChange, min, max, fmt]);

  const adjustBy = useCallback((delta: number) => {
    const next = clamp(value + delta, min, max);
    setText(fmt(next));
    pendingBase.current = valueRef.current;
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
    // Only write back text the user actually edited. A blur with no keystrokes
    // (clicking away, the panel/popover closing, focus stolen) must NOT commit
    // the field's text — if `value` changed externally while focused (a gizmo
    // drag moving the reference object), the un-updated text is stale and would
    // clobber the live value (the reference-object "slides to origin on close"
    // bug). Discard the untouched field and re-show the live value instead.
    if (edited.current) commit(text);
    else setText(fmt(value));
    edited.current = false;
  };

  const handleFocus = () => {
    isFocused.current = true;
    edited.current = false;
    pendingBase.current = null; // fresh interaction — drop any prior optimistic guard
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
      pendingBase.current = valueRef.current;
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
    // Run ONCE per gesture. The column binds both onPointerDown and onMouseDown:
    // a real mouse (and the --record cursor's activate press) fires pointerdown
    // THEN mousedown, so the pointerdown arms and the mousedown no-ops here. The
    // dual binding exists because the --record value-scrub arrives as a
    // NON-activate drag — pointerdown→pointermove→pointerup only, no mousedown
    // (record-cursor-drag) — so a mousedown-only handler could never be scrubbed;
    // and jsdom's fireEvent.mouseDown (no pointer event) still arms via mousedown.
    if (holdingRef.current) return;
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
    // Remember which pointer owns this gesture (pointerdown carries a pointerId;
    // a plain mousedown does not → null = unfiltered single-pointer path).
    const nat = e.nativeEvent as Event;
    activePointerId.current = nat instanceof PointerEvent ? nat.pointerId : null;

    // Arm hold-to-repeat. The interval ramps from a local accumulator so it
    // keeps stepping even before the controlled value prop echoes back.
    holdDelayTimer.current = window.setTimeout(() => {
      repeatedRef.current = true;
      holdRepeatTimer.current = window.setInterval(() => {
        const next = clamp(heldValue.current + dir * step, min, max);
        heldValue.current = next;
        setText(fmt(next));
        pendingBase.current = valueRef.current;
        onChangeRef.current(next); // latest handler — see onChangeRef (#614)
      }, HOLD_REPEAT_MS);
    }, HOLD_DELAY_MS);

    // Scrub emission is COALESCED to one onChange per animation frame (latest
    // value wins), flushed on release. Raw mousemove can outrun the frame rate
    // (125Hz+ mice), and heavy consumers (CurveEditorPanel writes the track
    // optimistically + round-trips the bridge per emission, #613) fall behind
    // when every pixel fires — the displayed text still updates per move, so
    // the field itself never feels throttled. Per-gesture locals: each press
    // owns its own rAF/pending state, dropped with the listeners on release.
    let scrubRafId: number | null = null;
    let scrubPendingVal: number | null = null;
    const flushScrub = () => {
      scrubRafId = null;
      if (scrubPendingVal === null) return;
      const v = scrubPendingVal;
      scrubPendingVal = null;
      pendingBase.current = valueRef.current;
      onChangeRef.current(v); // latest handler — see onChangeRef (#614)
    };

    // A pointer event from a DIFFERENT pointer than the one that started this
    // gesture must be ignored, else a second touch/pen moving or lifting anywhere
    // in the document would hijack or end this scrub. Mouse events carry no
    // pointerId (a single pointer); a jsdom/mouse-started gesture has a null id
    // and skips filtering entirely.
    const foreignPointer = (ev: Event): boolean =>
      activePointerId.current !== null &&
      typeof PointerEvent !== "undefined" &&
      ev instanceof PointerEvent &&
      ev.pointerId !== activePointerId.current;

    const onMove = (me: MouseEvent) => {
      if (foreignPointer(me)) return;
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
      // Queue the coalesced emission (release flushes the tail value).
      scrubPendingVal = next;
      if (scrubRafId === null) {
        if (typeof requestAnimationFrame === "function") {
          scrubRafId = requestAnimationFrame(flushScrub);
        } else {
          flushScrub();
        }
      }
    };

    // Release AND cancel share one teardown. A real mouse fires BOTH mouseup and
    // pointerup (and the --record cursor's synthetic release dispatches both too);
    // holdingRef is cleared here so the second event no-ops. `cancelled` (from
    // pointercancel — the OS/browser reclaimed the pointer for a permitted pan,
    // palm rejection, or capture loss) skips the trailing click step; a normal
    // release still applies it.
    const finish = (ev: Event | null, cancelled: boolean) => {
      if (!holdingRef.current) return;
      if (ev && foreignPointer(ev)) return; // a different pointer's up/cancel
      document.removeEventListener("mousemove", onMove);
      document.removeEventListener("mouseup", onUp);
      document.removeEventListener("pointermove", onMove as EventListener);
      document.removeEventListener("pointerup", onUp);
      document.removeEventListener("pointercancel", onCancel);
      clearHoldTimers();
      holdingRef.current = false;
      activePointerId.current = null;
      // Flush the tail scrub value BEFORE clearing drag state so the final
      // position always lands exactly once (the queued frame is cancelled).
      if (scrubRafId !== null && typeof cancelAnimationFrame === "function") {
        cancelAnimationFrame(scrubRafId);
      }
      flushScrub();
      setDragging(false);
      // Quick click: neither a scrub nor a hold-repeat fired → one step. A cancel
      // never steps (the gesture was reclaimed, not completed).
      if (!cancelled && !scrubbedRef.current && !repeatedRef.current) {
        adjustBy(dir * step);
      }
    };
    const onUp = (ev?: Event) => finish(ev ?? null, false);
    const onCancel = (ev?: Event) => finish(ev ?? null, true);

    document.addEventListener("mousemove", onMove);
    document.addEventListener("mouseup", onUp);
    // ALSO listen on pointer events: the --record cursor's value-scrub drives
    // the arrows via pointermove/pointerup ONLY (record-cursor-drag dispatches
    // pointermove on document, never mousemove), so a pointer-blind scrub can't
    // be recorded. onMove computes the value from the ABSOLUTE pointer position
    // (dragStartValue + dy*step), so a real mouse firing both mousemove and
    // pointermove for the same coordinate is idempotent (and emissions coalesce
    // to one onChange per frame regardless). pointercancel is REQUIRED: a
    // touch/pen stream can be cancelled instead of released, and without cleanup
    // the gesture would stay stuck (holdingRef true, listeners + hold-repeat
    // leaked, every later arrow press rejected by the holdingRef guard).
    document.addEventListener("pointermove", onMove as EventListener);
    document.addEventListener("pointerup", onUp);
    document.addEventListener("pointercancel", onCancel);
  };

  // Clear any pending hold timers on unmount.
  useEffect(() => clearHoldTimers, [clearHoldTimers]);

  // Keep text in sync when prop changes from outside (not while actively
  // editing). Effect runs post-commit so the displayed value reflects external
  // updates (e.g. undo, mod-switch, a gizmo drag moving the reference object,
  // parent rerender with a transformed value like FieldSpinner's
  // displayInvertedPercent) without requiring the user to focus the input
  // first. Skipped only while the user is genuinely mid-edit (focused AND has
  // typed) or scrubbing/holding, so in-flight typing is never clobbered — but a
  // focused-but-untouched field still tracks `value` so it can't go stale. Deps
  // are kept primitive (value, dp, dragging) so the effect doesn't run on every
  // render and clobber in-flight `setText` from `onChange`.
  useEffect(() => {
    if (dragging || holdingRef.current) return;
    // Reconcile the optimistic-echo guard FIRST — before the focus/edit skip —
    // so it can never outlive the echo: once the prop moves off the baseline it
    // was armed at, the value we emitted has been confirmed (or superseded), so
    // drop the guard even if the user is also mid-typing. (Otherwise a
    // type-then-arrow sequence could leave a stale baseline armed and later
    // swallow an external change back to that exact value.)
    if (pendingBase.current !== null && value !== pendingBase.current) {
      pendingBase.current = null;
    }
    if (isFocused.current && edited.current) return; // active typing — keep in-flight text
    // Optimistic-echo gap: a value we just emitted hasn't echoed into the prop
    // yet (prop still at the baseline). Keep the optimistic text the emit path
    // set rather than flashing back to the stale value for a frame.
    if (pendingBase.current !== null) return;
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
        onChange={(e) => { edited.current = true; setText(e.target.value); }}
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
        onPointerDown={handleArrowsMouseDown}
        onMouseDown={handleArrowsMouseDown}
        // Inset 1px on top/right/bottom and clip with rounded-right
        // corners so the buttons' hover/active background stays INSIDE
        // the input's rounded border instead of painting over the right
        // edge + corners (which made the box outline look broken on
        // press). rounded-r-[3px] is concentric with the input's 4px
        // `rounded` corner, exactly 1px inside — the border sits in the
        // gap. overflow-hidden clips the square-cornered button fills to
        // the rounded column.
        className={`absolute flex touch-none flex-col overflow-hidden rounded-r-[3px] border-l border-border-2 ${disabled ? "opacity-40" : "cursor-ns-resize"}`}
        style={{ top: 1, right: 1, bottom: 1, width: `${ARROW_W - 1}px` }}
        aria-hidden={disabled}
      >
        <button
          type="button"
          tabIndex={-1}
          disabled={disabled}
          aria-label="Increment"
          data-testid={testId ? `${testId}-inc` : undefined}
          className="flex flex-1 items-center justify-center text-text-3 hover:bg-panel-2 hover:text-text active:bg-accent-soft active:text-accent disabled:cursor-not-allowed"
          style={{ fontSize: "7px", lineHeight: 1 }}
        >
          ▲
        </button>
        <button
          type="button"
          tabIndex={-1}
          disabled={disabled}
          aria-label="Decrement"
          data-testid={testId ? `${testId}-dec` : undefined}
          className="flex flex-1 items-center justify-center text-text-3 hover:bg-panel-2 hover:text-text active:bg-accent-soft active:text-accent disabled:cursor-not-allowed"
          style={{ fontSize: "7px", lineHeight: 1 }}
        >
          ▼
        </button>
      </div>
    </div>
  );
}
