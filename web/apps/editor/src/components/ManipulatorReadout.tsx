// Owns the drag readout and its measured placement; the host owns gizmo interaction.
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import type { Bridge, Event } from "@particle-editor/bridge-schema";
import { useEngineField } from "@/lib/use-engine-snapshot";

type DragActive = Extract<Event, { kind: "engine/manipulator/drag" }>["payload"] & { active: true };

// Up-right offset (px) keeping the pill clear of the gizmo, which is screen-uniform:
// translate/plane show ~111px axis arrows (nothing on the up-right diagonal), but the
// ROTATE ring is larger — ~baseLen*1.15 ≈ 128px screen radius (engine.cpp
// kRingRadiusScale) — so the pill must sit further out for a rotate drag or it lands
// on the ring and is hard to read.
const OFFSET_AXIS = 72;
const OFFSET_RING = 112;
const EDGE_PADDING_PX = 4;
// Sign off the ROUNDED magnitude so a tiny negative (e.g. -0.04 at 1dp) renders
// "0.0", not "−0.0". U+2212 minus.
const fmt = (v: number, d: number) => {
  const r = Math.abs(v).toFixed(d);
  return (v < 0 && Number(r) !== 0 ? "−" : "") + r;
};

export function ManipulatorReadout({
  bridge,
  overlayRef,
}: {
  bridge: Bridge;
  overlayRef: React.RefObject<HTMLElement | null>;
}) {
  const [drag, setDrag] = useState<DragActive | null>(null);
  const pillRef = useRef<HTMLDivElement>(null);
  const hideDragForReference =
    useEngineField(bridge, (s) => s.referenceObjectName === "" || s.referenceObjectLocked) ?? false;

  useEffect(() => {
    const off1 = bridge.on("engine/manipulator/drag", (e) => {
      setDrag(e.payload.active ? e.payload : null);
    });
    return () => {
      off1();
    };
  }, [bridge]);

  // Belt-and-suspenders: if the reference object is cleared (mod-switch /
  // new-file → name "") or locked mid-something (gizmo gone), hide the pill.
  // (Deselect/lock don't route through the host drag-end sites; these are the
  // real DTO signals — there is no `referenceObjectSelected` field.)
  const [previousHideDrag, setPreviousHideDrag] = useState<boolean>(hideDragForReference);
  if (previousHideDrag !== hideDragForReference) {
    setPreviousHideDrag(hideDragForReference);
    if (hideDragForReference) setDrag(null);
  }

  // Measure after the pill mounts so clamping includes its current text dimensions.
  useLayoutEffect((): void => {
    const pill: HTMLDivElement | null = pillRef.current;
    if (!drag?.visible || pill === null) return;
    const box: DOMRect | undefined = overlayRef.current?.getBoundingClientRect();
    const width: number = box?.width ?? 0;
    const height: number = box?.height ?? 0;
    const clamp = (value: number, max: number): number =>
      Math.max(EDGE_PADDING_PX, Math.min(value, max - EDGE_PADDING_PX));
    const offset: number = drag.kind === "rotate" ? OFFSET_RING : OFFSET_AXIS;
    const left: number = clamp(drag.nx * width + offset, width - pill.offsetWidth);
    const top: number = clamp(drag.ny * height - offset, height - pill.offsetHeight);
    pill.style.transform = `translate(${left}px, ${top}px)`;
  });

  if (!drag || !drag.visible) return null;

  // Iterate the shorter of the two arrays so a (host-impossible but schema-allowed)
  // labels/values length mismatch can't render "NaN" silently.
  const n = Math.min(drag.labels.length, drag.values.length);
  const isRotate = drag.kind === "rotate";
  // Unit suffix: rotate is degrees (° per value); translate/plane are
  // distance, labelled once with a trailing "units" (the pill has room, unlike
  // the cramped XYZ spinner grids that label via their section header).
  const text =
    Array.from(
      { length: n },
      (_, i) => `${drag.labels[i]} ${fmt(drag.values[i], drag.decimals)}${isRotate ? "°" : ""}`,
    ).join("   ") + (isRotate ? "" : "   units");

  return (
    <div
      ref={pillRef}
      className="pointer-events-none absolute left-0 top-0 z-10 flex items-center gap-1.5 rounded-sm bg-accent-soft px-2 py-1 text-xs font-medium text-accent tabular-nums"
      aria-hidden="true"
    >
      {text}
    </div>
  );
}
