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
import { DRAG_FEEL, pickFlipDuration } from "@/lib/flip";
import { useRecording } from "@/lib/record/record-mode";

// Tuning — mirrors EmitterTree.tsx so every reorderable list feels the same.
const DRAG_THRESHOLD = 4;   // px (Manhattan) before a press becomes a drag
const CHIP_SPRING = 0.25;   // per-frame ease of the chip toward its target
const CHIP_PULL = 0.6;      // how far the chip leans toward the gap center
const CHIP_EXIT_MS = 160;   // chip fly-into-gap + fade on release
// Row-glide durations come from the shared DRAG_FEEL in lib/flip.ts (drag =
// snappy, settle = slower; recordDragMs is the --record active-drag glide).
// --record only (mirrors EmitterTree.tsx via pickFlipDuration in lib/flip.ts): the
// capture loop grabs a frame only after a slow paint+PrintWindow, so at the short
// live durations the transition finishes between captures and the reorder snaps in the
// clip. Long wall-clock durations span many captures so the glide records smoothly.
// Unlike the emitter tree (which keeps the short settle to match its chip despawn), the
// commit-settle is stretched here TOO — and the chip fly-in stretches with it
// (CHIP_EXIT_RECORD_MS + the 40ms timer pad = FLIP_RECORD_SETTLE_MS) so the chip never
// vanishes while rows are still gliding.
const FLIP_RECORD_SETTLE_MS = 800;
const CHIP_EXIT_RECORD_MS = 760;

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
  /** --record only: freeze a posed drag (lifted chip + make-room gap) WITHOUT real
   *  pointer events, so a clip still can show "drag to reorder". null = no pose. */
  pose?: { from: number; gap: number } | null;
}): StackReorder {
  const { order, labelFor, onReorder } = opts;
  const pose = opts.pose ?? null;
  const getSC = opts.getScrollContainer ?? ((list) => list.closest<HTMLElement>(".overflow-y-auto"));
  const recording = useRecording();
  // Chip fly-into-gap duration — stretched under --record in step with the record
  // settle (see the FLIP_RECORD_* comment above the constants).
  const chipExitMs = recording ? CHIP_EXIT_RECORD_MS : CHIP_EXIT_MS;

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

  // --record posed drag: render a frozen lift (dimmed row + make-room gap + a chip
  // leaning toward the gap) directly from `pose`, reusing the same geometry the live
  // gesture uses — no events/threshold/rAF/autoscroll/blur. Cleanup clears the visual
  // when the pose changes or unmounts. Keyed on the primitive fields so a fresh `pose`
  // object each render doesn't re-fire.
  useLayoutEffect(() => {
    if (!pose) return;
    const ul = listRef.current;
    if (!ul) return;
    const sc = getSC(ul);
    const scTop = sc ? sc.getBoundingClientRect().top - sc.scrollTop : 0;
    const tops: number[] = [], bottoms: number[] = [];
    const lis = ul.querySelectorAll<HTMLElement>(":scope > li[data-flip-key]");
    lis.forEach((li) => { const r = li.getBoundingClientRect(); tops.push(r.top - scTop); bottoms.push(r.bottom - scTop); });
    if (pose.from < 0 || pose.from >= tops.length) return;
    const geom: RootBlockGeometry = { tops, bottoms };
    const liftedH = liftedBlockHeight(geom, [pose.from]);
    const clampedGap = Math.max(0, Math.min(pose.gap, tops.length));
    const fromR = lis[pose.from]!.getBoundingClientRect();
    const baseT = sc ? sc.getBoundingClientRect().top - sc.scrollTop : 0;
    const gapCenter = gapContentY(geom, clampedGap) + baseT + liftedH / 2;
    const chipAt = computeChipTarget(fromR.left + 20, fromR.top + fromR.height / 2, gapCenter, CHIP_PULL);
    setGapHeight(liftedH);
    setDragIndex(pose.from);
    setGap(clampedGap);
    setChip({ x: chipAt.x, y: chipAt.y, name: labelFor(order[pose.from]!) });
    return () => { setDragIndex(null); setGap(null); setChip(null); };
    // Keyed on the posed indices + a STABLE STRING of the order — never the array itself,
    // whose identity consumers churn every render (that would loop: setState → re-render →
    // re-fire). The string key re-runs the pose once the stack CONTENT settles — e.g. after
    // refreshModsList() updates the rows — so a pose fired before the refresh isn't stuck on
    // the stale/out-of-range guard.
  }, [pose?.from, pose?.gap, pose ? order.join("\u0000") : ""]);

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
        chipTimer = window.setTimeout(() => setChip(null), chipExitMs + 40);
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
    // reduced-motion skips the glide — EXCEPT under --record, where the glide must
    // render into the captured clip regardless of the host's setting (mirrors EmitterTree).
    if (!recording && prefersReducedMotion()) return;
    // Long wall-clock transition only for the --record active-drag glide (so it spans
    // multiple slow captures); the settle keeps DRAG_FEEL.settleMs to stay in step
    // with the drag-chip despawn. See pickFlipDuration.
    // A --record POSE sets dragIndex but is a FROZEN still (no live drag motion), so it
    // must NOT get the long record-drag glide — pass dragging=false for a pose so it
    // settles instantly. Only a live active drag (pose === null) gets the record glide.
    const durationMs = pickFlipDuration(recording, dragIndex !== null && pose === null, {
      ...DRAG_FEEL,
      // Stretch the commit-settle under record too (the chip fly-in stretches in
      // step) — but a POSE must still settle instantly, so keep the short settle there.
      recordSettleMs: pose === null ? FLIP_RECORD_SETTLE_MS : DRAG_FEEL.settleMs,
    });
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
  }, [order, gap, dragIndex, recording]);

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
                    `left ${chipExitMs}ms ease-in, top ${chipExitMs}ms ease-in, ` +
                    `opacity ${chipExitMs}ms ease-in, transform ${chipExitMs}ms ease-in`,
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
