// Synthetic pointer-event dispatch for --record drag clips. The record cursor is a
// VISUAL-ONLY sprite (RecordCursor.tsx) — it paints at a coordinate but emits no
// events, so a clip can't drive a real reorder gesture on its own. This module
// synthesizes the pointer events the gesture listens for, off the per-frame
// evaluated cursor, so a --record clip plays out a REAL drag (lifted chip + make-room
// gap + drop) instead of a frozen still.
//
// Mechanism mirrors the gesture's own listener wiring (use-stack-reorder.tsx, and
// EmitterTree.tsx the same way):
//   press false→true : `pointerdown` on the reorderable ROW under the cursor — the
//                       gesture's React onPointerDown is on the row; bubbles:true
//                       reaches it. We resolve the row via `closest(...)` so a hit on
//                       an inner control (e.g. the stack row's remove button) is
//                       redirected to the row itself — otherwise the stack gesture's
//                       `closest("button")` guard would bail, leaving us "armed" with
//                       no gesture actually running (a silent, dead drag).
//   press stays true  : `pointermove` on `document` — the gesture's move listener is a
//                       raw document.addEventListener (NOT React), so dispatch there.
//   press true→false  : `pointerup` on `document` — same raw-document-listener reason.
//
// pointerId is fixed at 1 on all three (onMove/onUp gate on ev.pointerId). Coords are
// CSS/client px: the cursor eval returns DEVICE px, PointerEvent wants client px, so
// clientX = deviceX / dpr (the same round-trip RecordCursor does for its left/top).
//
// Record-mode-gated by construction: this is only ever called from the ui/cursor
// paths in App.tsx, which are dormant outside --record.
//
// Invariants (all enforced here, all regression-tested):
//   1. UNRESOLVED / NON-FINITE PRESS ≠ CLICK. The eval keeps `press` true even when a
//      target ref can't be resolved, reporting x=y=0 (holdKey) or NaN (empty track);
//      a naive down would then `elementFromPoint` the corner (or NaN). So the down and
//      the mid-drag move require `ok` AND finite coords; an unresolved frame never
//      clicks and never drags to a bogus point — it just retries / holds. (`ok`
//      guards the *resolution*; the finite check guards the *coordinate*, since the
//      eval's `ok:false` can carry either the (0,0) sentinel or NaN.)
//   2. A DRAG MUST NOT OUTLIVE ITS TRACK. If the cursor track is swapped while a drag
//      is armed, the real gesture is still mid-flight (captured pointer + document
//      listeners). `resetRecordDrag` fires a `pointercancel` (→ the gesture's onCancel
//      → finish(false), an ABORT with no reorder commit) and clears our state, so the
//      next track starts clean. The caller invokes it on every track message.
//   3. THE RELEASE IS UNCONDITIONAL. The up fires whenever a drag is armed and press
//      drops, regardless of `ok` — the gesture's finish() commits from the last
//      resolved gap, not this frame's coords — so a release on an unresolved frame
//      still ends the gesture cleanly (coords fall back to 0 to avoid dispatching NaN).

// Reorderable-row selector: a hit anywhere inside one of these is redirected to it so
// the pointerdown lands ON the row (where the gesture handler lives / bubbles from).
const ROW_SELECTOR = "[data-flip-key], [data-emitter-id]";

export interface RecordDragState {
  /** Whether the previous frame armed a drag (to detect down/up transitions). */
  prevPress: boolean;
}

export function createRecordDragState(): RecordDragState {
  return { prevPress: false };
}

export interface RecordDragCursor {
  /** Device px (cursor eval space). */
  x: number;
  /** Device px (cursor eval space). */
  y: number;
  press: boolean;
  /** Did this frame's target resolve? A pressed-but-unresolved frame carries the
   *  (0,0) sentinel or NaN; it must NOT synthesize a click/drag. (Point/"literal"
   *  targets are always ok:true, so they drive the drag like resolved element refs.) */
  ok: boolean;
  /** Opt-in activation flag from the governing key. A press is EITHER a reorder-drag
   *  gesture (activate:false) OR a real click/focus owned by record-cursor-activate
   *  (activate:true) — never both. When true, the drag must NOT arm: otherwise an
   *  activate-click on a curve key would ALSO fire this gesture's pointerdown on the
   *  key and wedge its pointer-capture drag, swallowing the activation click. */
  activate?: boolean;
}

const POINTER_ID = 1;

function makePointerEvent(type: string, buttons: number, button: number, clientX: number, clientY: number): PointerEvent {
  return new PointerEvent(type, {
    pointerId: POINTER_ID,
    pointerType: "mouse",
    isPrimary: true,
    bubbles: true,
    cancelable: true,
    button,
    buttons,
    clientX,
    clientY,
  });
}

export function applyRecordDrag(
  cursor: RecordDragCursor,
  state: RecordDragState,
  opts: { dpr?: number; doc?: Document } = {},
): void {
  const doc = opts.doc ?? document;
  const dpr = opts.dpr ?? (typeof window !== "undefined" ? window.devicePixelRatio || 1 : 1);
  const clientX = cursor.x / dpr;
  const clientY = cursor.y / dpr;
  const finite = Number.isFinite(clientX) && Number.isFinite(clientY);

  if (!state.prevPress && cursor.press) {
    // DOWN — only on a RESOLVED frame with FINITE coords (invariant 1): an unresolved
    // press reports (0,0)/NaN, so arming here would click the corner. Abort + retry
    // next frame. Literal point targets are always ok, so they arm normally.
    // An activate:true press is a click, not a drag — activation owns it; never arm
    // (invariant 4: activate and drag are mutually exclusive per press).
    if (cursor.activate || !cursor.ok || !finite) return;
    const hit = doc.elementFromPoint(clientX, clientY);
    if (!hit) return;
    // Redirect an inner-control hit to the reorderable row so the gesture accepts it
    // (see the header note); a true overlay has no row ancestor → dispatch as-is,
    // which correctly declines the drag (occlusion-correct).
    const target = hit.closest(ROW_SELECTOR) ?? hit;
    target.dispatchEvent(makePointerEvent("pointerdown", 1, 0, clientX, clientY));
    state.prevPress = true;
    return;
  }
  if (state.prevPress && cursor.press) {
    // MOVE — on document (button -1 per spec: no button change during a move). Skip an
    // unresolved / non-finite frame (invariant 1): holding leaves the gesture at its
    // last good position until the target resolves again.
    if (!cursor.ok || !finite) return;
    doc.dispatchEvent(makePointerEvent("pointermove", 1, -1, clientX, clientY));
    return;
  }
  if (state.prevPress && !cursor.press) {
    // UP — on document; ends the gesture (commit). Fires regardless of `ok` (invariant
    // 3); coords fall back to 0 on a non-finite release so we never dispatch NaN.
    doc.dispatchEvent(makePointerEvent("pointerup", 0, 0, finite ? clientX : 0, finite ? clientY : 0));
    state.prevPress = false;
  }
  // press false & !prevPress → idle, nothing to do.
}

/**
 * Abort any armed synthetic drag and reset state. Call this when the cursor track is
 * replaced/swapped (invariant 2): a track change mid-drag would otherwise strand the
 * real gesture (captured pointer + live document listeners) with no pointerup ever
 * coming. We dispatch `pointercancel` (pointerId 1) so the gesture's onCancel runs —
 * an ABORT (finish(false)), NOT a commit — leaving the stack/tree order unchanged.
 * No-op when nothing is armed.
 */
export function resetRecordDrag(state: RecordDragState, opts: { doc?: Document } = {}): void {
  if (!state.prevPress) return;
  const doc = opts.doc ?? document;
  doc.dispatchEvent(makePointerEvent("pointercancel", 0, 0, 0, 0));
  state.prevPress = false;
}
