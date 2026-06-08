// Phase 2 — DOM-event → ViewportInputEvent encoders.
//
// The host's InputDispatcher consumes the `viewport/input` bridge
// surface as Win32 messages; the engine's existing viewport WNDPROC
// reads modifiers exclusively from wParam MK_* bits and decodes
// coords from MAKEPOINTS(lParam). These helpers reassemble the
// MK_* bitmask from DOM events and quantise wheel deltas so the host
// can PostMessage with no further decoding.

import type { ViewportInputEvent } from "@particle-editor/bridge-schema";

// Win32 MK_* constants (winuser.h). Re-declared here so the renderer
// doesn't have to import platform headers.
export const MK_LBUTTON = 0x0001;
export const MK_RBUTTON = 0x0002;
export const MK_SHIFT   = 0x0004;
export const MK_CONTROL = 0x0008;
export const MK_MBUTTON = 0x0010;

// One wheel notch = WHEEL_DELTA = 120 units. The engine's wheel
// handler at HostWindow.cpp:1350 divides by WHEEL_DELTA to get the
// notch count, so we quantise here to keep the sign + magnitude
// stable across DOM-event units (which vary by platform).
export const WHEEL_DELTA = 120;

// Build the MK_* bitmask from a DOM mouse/pointer/wheel event.
//
// DOM `event.buttons` is a separate bitmask from MK_*:
//   bit 0 (=1) → LMB held, bit 1 (=2) → RMB held, bit 2 (=4) → MMB held
// `shiftKey` / `ctrlKey` come from the same event's modifier flags.
export function encodeMkButtons(
  e: MouseEvent | PointerEvent | WheelEvent,
): number {
  let m = 0;
  if (e.buttons & 1) m |= MK_LBUTTON;
  if (e.buttons & 2) m |= MK_RBUTTON;
  if (e.buttons & 4) m |= MK_MBUTTON;
  if (e.shiftKey)    m |= MK_SHIFT;
  if (e.ctrlKey)     m |= MK_CONTROL;
  return m;
}

// Quantise DOM WheelEvent.deltaY to ±WHEEL_DELTA per notch.
//
// DOM `deltaY` is positive when the user scrolls DOWN; Win32
// WM_MOUSEWHEEL is positive when the wheel rotates AWAY from the user
// (canonical "scroll-up" direction). The engine's handler treats
// positive as "zoom in" per the math at HostWindow.cpp:1360 (it
// negates internally). To preserve the existing user-visible
// behaviour (wheel up = zoom in) we flip the sign here.
export function quantiseWheelDelta(domDeltaY: number): number {
  if (domDeltaY === 0) return 0;
  return domDeltaY > 0 ? -WHEEL_DELTA : WHEEL_DELTA;
}

// Convert a DOM client-coords (CSS pixels) point relative to the
// viewport canvas to popup-client physical pixels. The popup spans
// the full main client per T4c.4, so popup-client-x == main-client-x
// at the same DPR — we just multiply by devicePixelRatio at event
// time (no caching: DPR can change mid-session per the
// `matchMedia('(resolution)')` listener in ViewportSlot).
export function toPopupClientCoords(
  clientX: number,
  clientY: number,
): { x: number; y: number } {
  const dpr = window.devicePixelRatio || 1;
  return { x: Math.round(clientX * dpr), y: Math.round(clientY * dpr) };
}

// Tag names whose focus suppresses global keyboard forwarding. Matches
// the CurveEditorPanel.tsx pattern so a user typing in a property
// field doesn't accidentally drive engine input.
export const TYPING_TAGS: ReadonlySet<string> = new Set([
  "INPUT",
  "TEXTAREA",
  "SELECT",
]);

export function isTypingTarget(target: EventTarget | null): boolean {
  if (target === null) return false;
  if (!(target instanceof Element)) return false;
  if (TYPING_TAGS.has(target.tagName)) return true;
  // contenteditable surfaces (e.g. some Radix editors) also count.
  if (target.getAttribute("contenteditable") === "true") return true;
  return false;
}

// Encoders — single-shot builders for each ViewportInputEvent variant.
// Kept here so component code stays declarative and the encoding
// rules live in one tested place.

export function makeMouseEvent(
  type: "mousedown" | "mouseup" | "mousemove",
  e: MouseEvent | PointerEvent,
  clientX: number,
  clientY: number,
): ViewportInputEvent {
  const { x, y } = toPopupClientCoords(clientX, clientY);
  const buttons = encodeMkButtons(e);
  if (type === "mousemove") return { type, x, y, buttons };
  const button: "left" | "right" | "middle" =
    e.button === 2 ? "right" : e.button === 1 ? "middle" : "left";
  return { type, button, x, y, buttons };
}

export function makeWheelEvent(
  e: WheelEvent,
  clientX: number,
  clientY: number,
): ViewportInputEvent {
  const { x, y } = toPopupClientCoords(clientX, clientY);
  return {
    type: "wheel",
    x,
    y,
    deltaY: quantiseWheelDelta(e.deltaY),
    buttons: encodeMkButtons(e),
  };
}

export function makeKeyEvent(
  type: "keydown" | "keyup",
  e: KeyboardEvent,
): ViewportInputEvent {
  return { type, vk: e.keyCode, repeat: e.repeat };
}

export const blurEvent: ViewportInputEvent = { type: "blur" };
