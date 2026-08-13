import { useCallback, useLayoutEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from "react";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";
import { rectFromPoints, emittersInMarquee, mergeMarqueeSelection, type Rect } from "@/lib/marquee";

type MarqueeRowsSnapshot = {
  rows: { id: number; rect: Rect }[];
  rowKey: string;
};

function sortedIds(ids: number[]): number[] {
  return [...ids].sort((a, b) => a - b);
}

function sameSortedIds(a: number[], b: number[]): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i += 1) if (a[i] !== b[i]) return false;
  return true;
}

function rowIdentityKey(ids: number[]): string {
  return ids.join("|");
}

function scrollContentOrigin(sc: HTMLElement): { left: number; top: number } {
  const rect = sc.getBoundingClientRect();
  return { left: rect.left - sc.scrollLeft, top: rect.top - sc.scrollTop };
}

function toScrollContentRect(sc: HTMLElement, rect: Rect): Rect {
  const origin = scrollContentOrigin(sc);
  return {
    left: rect.left - origin.left,
    top: rect.top - origin.top,
    right: rect.right - origin.left,
    bottom: rect.bottom - origin.top,
  };
}

function captureMarqueeRows(sc: HTMLElement | null): MarqueeRowsSnapshot {
  if (sc === null) return { rows: [], rowKey: "" };
  const origin = scrollContentOrigin(sc);
  const rows = [...sc.querySelectorAll<HTMLElement>("[data-emitter-id]")]
    .map((el) => {
      const id = Number(el.dataset.emitterId);
      const r = el.getBoundingClientRect();
      return {
        id,
        rect: {
          left: r.left - origin.left,
          top: r.top - origin.top,
          right: r.right - origin.left,
          bottom: r.bottom - origin.top,
        },
      };
    })
    .filter((row) => Number.isFinite(row.id));
  return { rows, rowKey: rowIdentityKey(rows.map((row) => row.id)) };
}

type UseEmitterMarqueeOptions = {
  orderedIds: number[];
};

export function useEmitterMarquee({ orderedIds }: UseEmitterMarqueeOptions) {
  const treeScrollRef = useRef<HTMLDivElement | null>(null);
  const [marqueeBox, setMarqueeBox] = useState<
    { left: number; top: number; width: number; height: number } | null
  >(null);
  // `mergeBase` is what swept rows union with (the prior selection only when
  // additive); `prior` is always the pre-marquee selection, restored on Esc.
  const marqueeRef = useRef<
    | {
        mergeBase: number[];
        prior: number[];
        startX: number;
        startY: number;
        rows: { id: number; rect: Rect }[];
        rowKey: string;
        lastSelectionSorted: number[];
      }
    | null
  >(null);

  useLayoutEffect(() => {
    const m = marqueeRef.current;
    if (m === null) return;
    const latestRowKey = rowIdentityKey(orderedIds);
    if (m.rowKey === latestRowKey) return;
    const snapshot = captureMarqueeRows(treeScrollRef.current);
    m.rows = snapshot.rows;
    m.rowKey = snapshot.rowKey;
  }, [orderedIds]);

  const handleScrollPointerDown = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (e.button !== 0) return;
      const target = e.target as HTMLElement;
      // A press on a row (or any interactive control) belongs to that row's
      // click-select / drag-reorder, not the marquee.
      if (target.closest("[data-emitter-id]") !== null) return;
      if (target.closest("input,button,[role='button']") !== null) return;
      e.preventDefault();
      const prior = [...useEmitterSelectionStore.getState().ids];
      const additive = e.ctrlKey || e.metaKey;
      const mergeBase = additive ? prior : [];
      const snapshot = captureMarqueeRows(treeScrollRef.current);
      marqueeRef.current = {
        mergeBase,
        prior,
        startX: e.clientX,
        startY: e.clientY,
        rows: snapshot.rows,
        rowKey: snapshot.rowKey,
        lastSelectionSorted: sortedIds(prior),
      };

      const onMove = (ev: PointerEvent) => {
        const m = marqueeRef.current;
        if (m === null) return;
        const mqViewport = rectFromPoints(m.startX, m.startY, ev.clientX, ev.clientY);
        const sc = treeScrollRef.current;
        const mq = sc !== null ? toScrollContentRect(sc, mqViewport) : mqViewport;
        const swept = emittersInMarquee(m.rows, mq);
        const { ids, primary } = mergeMarqueeSelection(m.mergeBase, swept);
        const nextSorted = sortedIds(ids);
        if (!sameSortedIds(m.lastSelectionSorted, nextSorted)) {
          m.lastSelectionSorted = nextSorted;
          useEmitterSelectionStore.getState().setIds(ids, primary);
        }
        if (sc !== null) {
          setMarqueeBox({
            left: mq.left,
            top: mq.top,
            width: mq.right - mq.left,
            height: mq.bottom - mq.top,
          });
        }
      };
      const cleanup = () => {
        document.removeEventListener("pointermove", onMove);
        document.removeEventListener("pointerup", onUp);
        document.removeEventListener("keydown", onKey, true);
        marqueeRef.current = null;
        setMarqueeBox(null);
      };
      const onUp = () => cleanup();
      const onKey = (ev: KeyboardEvent) => {
        if (ev.key !== "Escape") return;
        const m = marqueeRef.current;
        if (m !== null) {
          const primary = m.prior.length > 0 ? m.prior[m.prior.length - 1]! : null;
          useEmitterSelectionStore.getState().setIds(m.prior, primary);
        }
        ev.preventDefault();
        ev.stopPropagation();
        cleanup();
      };
      document.addEventListener("pointermove", onMove);
      document.addEventListener("pointerup", onUp);
      document.addEventListener("keydown", onKey, true);
    },
    [],
  );

  return { treeScrollRef, marqueeBox, handleScrollPointerDown };
}
