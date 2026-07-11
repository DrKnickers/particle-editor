// Opt-in ACTIVATION dispatch for --record cursor presses. applyRecordDrag gives a
// press real pointerdown/move/up — enough for pointer-driven widgets (Radix menus,
// drag gestures) but NOT for click-driven controls or input focus: synthetic pointer
// events trigger no default actions, so no `click` fires and nothing focuses. This
// module adds that, ONLY for cursor keys authored with `"activate": true` — a global
// change would break existing clips whose theatrical presses sit on live controls
// (e.g. the spawner clips' Spawn-now press would double-fire with its scripted
// trigger).
//
// Semantics mirror a real click (pointerdown→mousedown→focus→pointerup→mouseup→
// click): synthetic PointerEvents do NOT auto-derive mouse events, and Radix Tabs
// (among others) listens on mousedown — so the mouse triple is dispatched here.
// The pointer pair is dispatched here TOO (2026-07-10): Radix Menubar triggers
// (the emitter tree's + menu) open ONLY on pointerdown, and the drag module
// deliberately skips activate presses (mutual exclusivity), so without a
// pointerdown from this module an activate click on a menubar trigger focuses
// the button but never opens the menu — the tutorial-03 muzzle-flash add-emitter
// beat regressed exactly this way. A real browser sends the full sextet in this
// order; matching it is strictly more faithful. (Radix's own same-gesture guard
// keeps the trailing click from toggling the just-opened menu closed — verified
// live against the mock app.)
//   press false→true (activate, ok, finite):
//     - if a text-entry element currently has focus and the hit lands outside it,
//       blur it first (a real pointerdown moves focus away — FieldText commits on
//       blur, so a typed rename "commits" when the cursor clicks elsewhere);
//     - focus() the nearest focusable at the hit (input/textarea/select/button/
//       [tabindex]) — real focus happens on mousedown's default action;
//     - arm the hit element for the click.
//   press true→false: dispatch MouseEvent("click") on the ARMED element when the
//     release lands back on it (or its subtree/ancestor — closest common activation
//     semantics simplified to containment either way); a release elsewhere cancels
//     (real browsers do the same).
//
// Ordering: call AFTER applyRecordDrag each frame, so pointerdown precedes focus and
// pointerup precedes click — the real event order.
//
// Record-mode-gated by construction (only called from the ui/cursor paths in
// App.tsx, dormant outside --record), and per-key opt-in on top of that.

const FOCUSABLE_SELECTOR = "input, textarea, select, button, [tabindex]";
const TEXT_ENTRY_SELECTOR = "input, textarea";

export interface RecordActivateState {
  prevPress: boolean;
  /** The element armed by an activating pointerdown; click target on release. */
  armed: Element | null;
}

export function createRecordActivateState(): RecordActivateState {
  return { prevPress: false, armed: null };
}

export interface RecordActivateCursor {
  /** Device px (cursor eval space). */
  x: number;
  y: number;
  press: boolean;
  ok: boolean;
  activate: boolean;
}

function makeMouseEvent(type: string, clientX: number, clientY: number): MouseEvent {
  return new MouseEvent(type, {
    bubbles: true,
    cancelable: true,
    button: 0,
    detail: 1,
    clientX,
    clientY,
  });
}

function makePointerEvent(type: string, clientX: number, clientY: number): PointerEvent {
  return new PointerEvent(type, {
    bubbles: true,
    cancelable: true,
    button: 0,
    pointerId: 1,
    pointerType: "mouse",
    isPrimary: true,
    clientX,
    clientY,
  });
}

export function applyRecordActivation(
  cursor: RecordActivateCursor,
  state: RecordActivateState,
  opts: { dpr?: number; doc?: Document } = {},
): void {
  const doc = opts.doc ?? document;
  const dpr = opts.dpr ?? (typeof window !== "undefined" ? window.devicePixelRatio || 1 : 1);
  const clientX = cursor.x / dpr;
  const clientY = cursor.y / dpr;
  const finite = Number.isFinite(clientX) && Number.isFinite(clientY);

  if (!state.prevPress && cursor.press) {
    // Same guards as the drag's down (invariant 1 there): an unresolved or
    // non-finite press must not activate the (0,0) corner. Mirror drag's
    // ordering too — do NOT commit `prevPress` until a hit is actually armed,
    // so a same-frame miss (target not yet mounted, elementFromPoint gap)
    // retries on the next still-pressed frame instead of silently eating the
    // whole click with no dispatch and no loud failure.
    if (!cursor.activate || !cursor.ok || !finite) return;
    const hit = doc.elementFromPoint(clientX, clientY);
    if (!hit) return;
    // pointerdown FIRST (real event order) — Radix Menubar triggers open here and
    // nowhere else. The drag module never arms on activate presses, so this is the
    // gesture's only pointerdown.
    hit.dispatchEvent(makePointerEvent("pointerdown", clientX, clientY));
    // mousedown precedes focus (its default action) — Radix Tabs et al. activate here.
    hit.dispatchEvent(makeMouseEvent("mousedown", clientX, clientY));
    // Focus-change next: blur a focused text-entry when the press lands outside
    // it (this is what commits FieldText drafts on "click elsewhere").
    const active = doc.activeElement;
    if (active && active.matches?.(TEXT_ENTRY_SELECTOR) && !active.contains(hit) && !hit.contains(active)) {
      (active as HTMLElement).blur?.();
    }
    const focusable = hit.closest?.(FOCUSABLE_SELECTOR);
    if (focusable) (focusable as HTMLElement).focus?.();
    state.armed = hit;
    state.prevPress = true;
    return;
  }

  if (state.prevPress && !cursor.press) {
    state.prevPress = false;
    const armed = state.armed;
    state.armed = null;
    if (!armed || !armed.isConnected) return;
    // pointerup ALWAYS fires on the armed element, even when the release lands
    // elsewhere — real pointer-capture semantics route pointerup to the captor
    // regardless of position, and skipping it would strand any gesture the
    // pointerdown armed (e.g. an EmitterTree row's reorder drag would keep its
    // document listeners + capture and silently eat every later gesture).
    armed.dispatchEvent(makePointerEvent("pointerup", finite ? clientX : 0, finite ? clientY : 0));
    // The CLICK (and its mouseup) stays containment-gated: a release elsewhere
    // is a cancel, like a real mouse.
    const hit = finite ? doc.elementFromPoint(clientX, clientY) : null;
    if (!hit || !(armed.contains(hit) || hit.contains(armed))) return;
    armed.dispatchEvent(makeMouseEvent("mouseup", finite ? clientX : 0, finite ? clientY : 0));
    armed.dispatchEvent(makeMouseEvent("click", finite ? clientX : 0, finite ? clientY : 0));
  }
}

/** Reset on cursor-track swap (mirrors resetRecordDrag): drop any armed click. */
export function resetRecordActivation(state: RecordActivateState): void {
  state.prevPress = false;
  state.armed = null;
}
