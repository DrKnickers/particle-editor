// useStackReorder — the shared pointer-drag "glide / preview-drop" engine for the
// ordered mod-layer stack. Used by BOTH the Load Order modal and the Mods toolbar
// dropdown (Option D in-place reorder) so the two feel identical.
//
// Mechanism (mirrors EmitterTree.tsx): pointer events + setPointerCapture (native
// HTML5 DnD is dead inside a Radix Dialog/Menu + WebView2); geometry snapshotted at
// activation + resolved with pure math (lib/multi-drag.ts) so a reflowing make-room
// gap never flickers; a floating chip that springs toward the target gap; FLIP so
// rows glide to their new slots; full teardown (re-entrancy latch, blur /
// visibilitychange / unmount cancel) so a gesture can't strand listeners or the rAF.
import { useEffect, useLayoutEffect, useRef, useState, type ReactNode } from "react";
import { createPortal } from "react-dom";
import {
  resolveGapFromGeometry,
  gapContentY,
  liftedBlockHeight,
  computeChipTarget,
  type RootBlockGeometry,
} from "@/lib/multi-drag";
import { computeAutoscrollDelta } from "@/lib/drag-autoscroll";

// Tuning — mirrors EmitterTree.tsx so every reorderable list feels the same.
const DRAG_THRESHOLD = 4;   // px (Manhattan) before a press becomes a drag
const CHIP_SPRING = 0.25;   // per-frame ease of the chip toward its target
const CHIP_PULL = 0.6;      // how far the chip leans toward the gap center
const CHIP_EXIT_MS = 160;   // chip fly-into-gap + fade on release
const FLIP_DRAG_MS = 120;   // row glide while dragging (snappy)
const FLIP_SETTLE_MS = 200; // row glide after the drop (slower)

type Chip = { x: number; y: number; name: string; exit?: { x: number; y: number } };

const prefersReducedMotion = () =>
  typeof window.matchMedia === "function" &&
  window.matchMedia("(prefers-reduced-motion: reduce)").matches;

export type StackReorder = {
  /** Index of the row being dragged (dim it), or null. */
  dragIndex: number | null;
  /** Resolved make-room gap (0..order.length), or null. */
  gap: number | null;
  /** Height (px) of the make-room spacer = the dragged row's measured height. */
  gapHeight: number;
  /** True while a drag is active — e.g. to suppress a dropdown's auto-dismiss. */
  dragging: boolean;
  /** Attach to the <ul> that holds the `data-flip-key` rows. */
  listRef: React.RefObject<HTMLUListElement | null>;
  /** Per-row pointerdown handler: `onPointerDown={startDrag(i)}`. */
  startDrag: (from: number) => (e: React.PointerEvent<HTMLElement>) => void;
  /** Abort a live gesture + clear visuals (call when the container closes). */
  cancel: () => void;
  /** The floating chip, portaled to <body>. Render it once: `{api.chipNode}`. */
  chipNode: ReactNode;
};

export function useStackReorder(opts: {
  order: string[];
  labelFor: (p: string) => string;
  /** Commit a reorder: move `from` to insertion gap `target` (=== order.length → end). */
  onReorder: (from: number, target: number) => void;
  /** Resolve the scroll container for content-space math; default `.overflow-y-auto`
   *  ancestor. Return null for a non-scrolling list (e.g. a short dropdown). */
  getScrollContainer?: (list: HTMLElement) => HTMLElement | null;
}): StackReorder {
  const { order, labelFor, onReorder } = opts;
  const getSC = opts.getScrollContainer ?? ((list) => list.closest<HTMLElement>(".overflow-y-auto"));

  const [dragIndex, setDragIndex] = useState<number | null>(null);
  const [gap, setGap] = useState<number | null>(null);
  const [gapHeight, setGapHeight] = useState(0);
  const [chip, setChip] = useState<Chip | null>(null);
  const listRef = useRef<HTMLUListElement | null>(null);
  // Aborts the live gesture (tears down listeners + rAF + capture); set on
  // activation, cleared in finish(). dragPointerRef is the re-entrancy latch.
  const activeDragCancelRef = useRef<(() => void) | null>(null);
  const dragPointerRef = useRef<number | null>(null);

  // Never let a gesture outlive the component.
  useEffect(() => () => activeDragCancelRef.current?.(), []);

  const cancel = () => { activeDragCancelRef.current?.(); setDragIndex(null); setGap(null); setChip(null); };

  const startDrag = (from: number) => (e: React.PointerEvent<HTMLElement>) => {
    // Left button only; let any in-row buttons keep their own clicks; one gesture
    // at a time (re-entrancy latch).
    if (e.button !== 0 || (e.target as HTMLElement).closest("button")) return;
    if (dragPointerRef.current !== null) return;
    e.preventDefault(); // suppress text selection
    const captureEl = e.currentTarget;
    const pointerId = e.pointerId;
    dragPointerRef.current = pointerId;
    try { captureEl.setPointerCapture(pointerId); } catch { /* jsdom */ }
    const startX = e.clientX, startY = e.clientY;
    let lastX = startX, lastY = startY;
    let active = false;
    let geom: RootBlockGeometry | null = null;
    let liftedH = 0;
    let lastGap: number | null = null;
    let rafId: number | null = null;
    let chipTimer: number | null = null;
    const chipPos = { x: startX + 12, y: startY + 12 };
    const chipName = labelFor(order[from]);
    const reduce = prefersReducedMotion();
    const sc = listRef.current ? getSC(listRef.current) : null;

    const toContentY = (clientY: number) =>
      sc ? clientY - sc.getBoundingClientRect().top + sc.scrollTop : clientY;

    const captureGeometry = (): RootBlockGeometry | null => {
      const ul = listRef.current;
      if (!ul) return null;
      const scTop = sc ? sc.getBoundingClientRect().top - sc.scrollTop : 0;
      const tops: number[] = [], bottoms: number[] = [];
      ul.querySelectorAll<HTMLElement>(":scope > li[data-flip-key]").forEach((li) => {
        const r = li.getBoundingClientRect();
        tops.push(r.top - scTop); bottoms.push(r.bottom - scTop);
      });
      return { tops, bottoms };
    };

    const setGapState = (g: number | null) => { if (g === lastGap) return; lastGap = g; setGap(g); };
    const updateTarget = (clientY: number) => {
      if (!geom) return;
      const res = resolveGapFromGeometry(geom, [from], toContentY(clientY), lastGap, liftedH);
      setGapState(res === "noop" ? null : res.rootIndex);
    };
    const stepChip = () => {
      if (!active) return;
      let gapCenter: number | null = null;
      if (geom && lastGap !== null) {
        const baseT = sc ? sc.getBoundingClientRect().top - sc.scrollTop : 0;
        gapCenter = gapContentY(geom, lastGap) + baseT + liftedH / 2;
      }
      const target = computeChipTarget(lastX, lastY, gapCenter, CHIP_PULL);
      if (reduce) { chipPos.x = target.x; chipPos.y = target.y; }
      else { chipPos.x += (target.x - chipPos.x) * CHIP_SPRING; chipPos.y += (target.y - chipPos.y) * CHIP_SPRING; }
      setChip({ x: chipPos.x, y: chipPos.y, name: chipName });
    };
    const tick = () => {
      if (sc) { const d = computeAutoscrollDelta(lastY, sc.getBoundingClientRect()); if (d !== 0) { sc.scrollTop += d; updateTarget(lastY); } }
      stepChip();
      rafId = requestAnimationFrame(tick);
    };

    const onMove = (ev: PointerEvent) => {
      if (ev.pointerId !== pointerId) return;
      lastX = ev.clientX; lastY = ev.clientY;
      if (!active) {
        if (Math.abs(ev.clientX - startX) + Math.abs(ev.clientY - startY) < DRAG_THRESHOLD) return;
        // Force-finish any in-flight FLIP glide so the snapshot reads RESTING rects.
        listRef.current?.querySelectorAll<HTMLElement>(":scope > li[data-flip-key]").forEach((li) => {
          li.style.transition = "none"; li.style.transform = "";
        });
        geom = captureGeometry();
        if (geom === null) { finish(false); return; }
        liftedH = liftedBlockHeight(geom, [from]);
        active = true;
        setGapHeight(liftedH);
        setDragIndex(from);
        activeDragCancelRef.current = () => finish(false);
        window.addEventListener("blur", onBlur);
        document.addEventListener("visibilitychange", onVis);
        rafId = requestAnimationFrame(tick);
      }
      updateTarget(ev.clientY);
      stepChip();
    };
    const onBlur = () => finish(false);
    const onVis = () => { if (document.visibilityState === "hidden") finish(false); };
    const finish = (commit: boolean) => {
      document.removeEventListener("pointermove", onMove);
      document.removeEventListener("pointerup", onUp);
      document.removeEventListener("pointercancel", onCancel);
      window.removeEventListener("blur", onBlur);
      document.removeEventListener("visibilitychange", onVis);
      if (dragPointerRef.current === pointerId) dragPointerRef.current = null;
      activeDragCancelRef.current = null;
      try { captureEl.releasePointerCapture(pointerId); } catch { /* already released */ }
      if (rafId !== null) { cancelAnimationFrame(rafId); rafId = null; }
      if (chipTimer !== null) { clearTimeout(chipTimer); chipTimer = null; }
      if (!active) return; // pre-threshold release — nothing was shown
      active = false;
      const targetGap = lastGap;
      if (commit && targetGap !== null) onReorder(from, targetGap);
      if (reduce || geom === null) {
        setChip(null);
      } else {
        let exit = { x: chipPos.x, y: chipPos.y };
        if (commit && targetGap !== null) {
          const baseT = sc ? sc.getBoundingClientRect().top - sc.scrollTop : 0;
          const listLeft = listRef.current?.getBoundingClientRect().left ?? 0;
          exit = { x: listLeft + 20, y: gapContentY(geom, targetGap) + baseT + liftedH / 2 - 10 };
        }
        setChip((c) => (c ? { ...c, exit } : null));
        chipTimer = window.setTimeout(() => setChip(null), CHIP_EXIT_MS + 40);
      }
      setDragIndex(null);
      setGap(null);
    };
    const onUp = (ev: PointerEvent) => { if (ev.pointerId === pointerId) finish(true); };
    const onCancel = (ev: PointerEvent) => { if (ev.pointerId === pointerId) finish(false); };

    document.addEventListener("pointermove", onMove);
    document.addEventListener("pointerup", onUp);
    document.addEventListener("pointercancel", onCancel);
  };

  // FLIP: rows that shifted (gap moved during a drag, or the commit on release)
  // glide to their new slots instead of snapping. Keyed by `data-flip-key`.
  const flipPrev = useRef<Map<string, number>>(new Map());
  useLayoutEffect(() => {
    const ul = listRef.current;
    const prev = flipPrev.current;
    const next = new Map<string, number>();
    const els = new Map<string, HTMLElement>();
    ul?.querySelectorAll<HTMLElement>(":scope > li[data-flip-key]").forEach((li) => {
      const k = li.dataset.flipKey!;
      next.set(k, li.offsetTop);
      els.set(k, li);
    });
    flipPrev.current = next;
    if (prefersReducedMotion()) return;
    const durationMs = dragIndex !== null ? FLIP_DRAG_MS : FLIP_SETTLE_MS;
    for (const [k, top] of next) {
      const was = prev.get(k);
      if (was === undefined || was === top) continue;
      const el = els.get(k)!;
      const m = /matrix\([^,]+,[^,]+,[^,]+,[^,]+,[^,]+,\s*(-?[\d.]+)\)/.exec(getComputedStyle(el).transform);
      const total = (was - top) + (m !== null ? parseFloat(m[1]!) : 0);
      if (total === 0) continue;
      el.style.transition = "none";
      el.style.transform = `translateY(${total}px)`;
      void el.offsetHeight;
      el.style.transition = `transform ${durationMs}ms ease`;
      el.style.transform = "";
    }
  }, [order, gap, dragIndex]);

  const chipNode = chip
    ? createPortal(
        <div
          aria-hidden
          data-testid="stack-drag-chip"
          className="drag-chip-enter pointer-events-none fixed z-[60] rounded-md border border-accent bg-bg-2/95 px-2 py-1 text-xs text-accent shadow-[var(--shadow-soft)]"
          style={
            chip.exit
              ? {
                  left: chip.exit.x,
                  top: chip.exit.y,
                  opacity: 0,
                  transform: "scale(0.85)",
                  transition:
                    `left ${CHIP_EXIT_MS}ms ease-in, top ${CHIP_EXIT_MS}ms ease-in, ` +
                    `opacity ${CHIP_EXIT_MS}ms ease-in, transform ${CHIP_EXIT_MS}ms ease-in`,
                }
              : { left: chip.x, top: chip.y }
          }
        >
          <div className="truncate px-2 leading-5">{chip.name}</div>
        </div>,
        document.body,
      )
    : null;

  return { dragIndex, gap, gapHeight, dragging: dragIndex !== null, listRef, startDrag, cancel, chipNode };
}
