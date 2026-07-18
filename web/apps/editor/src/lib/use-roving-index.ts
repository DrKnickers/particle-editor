// use-roving-index.ts — single-tab-stop roving focus for a flat collection
// of focusable items (design pass, B5).
//
// The collection exposes ONE Tab stop (the active item, tabIndex=0; all
// others -1); Arrow keys move the active index and focus follows. With
// `columns` set, Up/Down jump by a row; without it they mirror Left/Right
// (useful for wrap-flow lists whose column count is fluid). Home/End jump
// to the ends. Clicking or programmatic focus on any item adopts it as the
// active stop (onFocus), so mouse and keyboard stay in sync.
//
// Deliberately minimal: no wrap-around, no typeahead, no selection state —
// selection stays the consumer's concern (aria-selected etc.). New consumers
// (TexturePalette, ColorButton) share this; the three pre-existing bespoke
// implementations (AtlasPickerPanel, ImportEmittersDialog,
// ReferenceObjectPicker) are intentionally NOT retrofitted (surgical rule).

import { useRef, useState, type KeyboardEvent } from "react";

export type RovingItemProps = {
  tabIndex: 0 | -1;
  ref: (el: HTMLElement | null) => void;
  onKeyDown: (e: KeyboardEvent) => void;
  onFocus: () => void;
};

export function useRovingIndex(
  count: number,
  opts?: { columns?: number; initial?: number },
): { activeIndex: number; itemProps: (i: number) => RovingItemProps } {
  const [active, setActive] = useState(opts?.initial ?? 0);
  const refs = useRef<(HTMLElement | null)[]>([]);
  // Clamp instead of effect-resetting so a shrinking collection can't strand
  // the stop out of range mid-render.
  const clamped = count <= 0 ? 0 : Math.min(Math.max(active, 0), count - 1);

  const move = (next: number) => {
    const n = Math.max(0, Math.min(count - 1, next));
    setActive(n);
    refs.current[n]?.focus();
  };

  const onKeyDown = (e: KeyboardEvent) => {
    const col = opts?.columns;
    let next: number;
    switch (e.key) {
      case "ArrowRight": next = clamped + 1; break;
      case "ArrowLeft":  next = clamped - 1; break;
      case "ArrowDown":  next = clamped + (col ?? 1); break;
      case "ArrowUp":    next = clamped - (col ?? 1); break;
      case "Home":       next = 0; break;
      case "End":        next = count - 1; break;
      default: return;
    }
    e.preventDefault();
    move(next);
  };

  const itemProps = (i: number): RovingItemProps => ({
    tabIndex: i === clamped ? 0 : -1,
    // Radix `asChild` triggers compose refs through Slot, so this callback
    // ref coexists with ContextMenu/Tooltip trigger refs.
    ref: (el) => { refs.current[i] = el; },
    onKeyDown,
    onFocus: () => setActive(i),
  });

  return { activeIndex: clamped, itemProps };
}
