import { useEffect, useRef, useState } from "react";
import type { Bridge, Event } from "@particle-editor/bridge-schema";
import { useEngineField } from "@/lib/use-engine-snapshot";

type DragActive = Extract<Event, { kind: "engine/manipulator/drag" }>["payload"] & { active: true };

// Up-right offset (px) keeping the pill clear of the gizmo, which is screen-uniform:
// translate/plane show ~111px axis arrows (nothing on the up-right diagonal), but the
// ROTATE ring is larger — ~baseLen*1.15 ≈ 128px screen radius (engine.cpp
// kRingRadiusScale) — so the pill must sit further out for a rotate drag or it lands
// on the ring (the readability issue from the s55 live review).
const OFFSET_AXIS = 72;
const OFFSET_RING = 112;
// Sign off the ROUNDED magnitude so a tiny negative (e.g. -0.04 at 1dp) renders
// "0.0", not "−0.0". U+2212 minus.
const fmt = (v: number, d: number) => {
  const r = Math.abs(v).toFixed(d);
  return (v < 0 && Number(r) !== 0 ? "−" : "") + r;
};

export function ManipulatorReadout({
  bridge, overlayRef,
}: { bridge: Bridge; overlayRef: React.RefObject<HTMLElement | null> }) {
  const [drag, setDrag] = useState<DragActive | null>(null);
  const pillRef = useRef<HTMLDivElement>(null);
  const hideDragForReference = useEngineField(
    bridge,
    (s) => s.referenceObjectName === "" || s.referenceObjectLocked,
  ) ?? false;

  useEffect(() => {
    const off1 = bridge.on("engine/manipulator/drag", (e) => {
      setDrag(e.payload.active ? e.payload : null);
    });
    return () => { off1(); };
  }, [bridge]);

  // Belt-and-suspenders: if the reference object is cleared (mod-switch /
  // new-file → name "") or locked mid-something (gizmo gone), hide the pill.
  // (Deselect/lock don't route through the host drag-end sites; these are the
  // real DTO signals — there is no `referenceObjectSelected` field.)
  useEffect(() => {
    if (hideDragForReference) setDrag(null);
  }, [hideDragForReference]);

  if (!drag || !drag.visible) return null;

  const box = overlayRef.current?.getBoundingClientRect();
  const W = box?.width ?? 0, H = box?.height ?? 0;
  const pw = pillRef.current?.offsetWidth ?? 0, ph = pillRef.current?.offsetHeight ?? 0;
  const clamp = (v: number, max: number) => Math.max(4, Math.min(v, max - 4));
  const offset = drag.kind === "rotate" ? OFFSET_RING : OFFSET_AXIS;
  const left = clamp(drag.nx * W + offset, W - pw);
  const top  = clamp(drag.ny * H - offset, H - ph);

  // Iterate the shorter of the two arrays so a (host-impossible but schema-allowed)
  // labels/values length mismatch can't render "NaN" silently.
  const n = Math.min(drag.labels.length, drag.values.length);
  const isRotate = drag.kind === "rotate";
  // Unit suffix: rotate is degrees (° per value); translate/plane are
  // distance, labelled once with a trailing "units" (the pill has room, unlike
  // the cramped XYZ spinner grids that label via their section header).
  const text =
    Array.from({ length: n }, (_, i) =>
      `${drag.labels[i]} ${fmt(drag.values[i], drag.decimals)}${isRotate ? "°" : ""}`,
    ).join("   ") + (isRotate ? "" : "   units");

  return (
    <div
      ref={pillRef}
      className="pointer-events-none absolute left-0 top-0 z-10 flex items-center gap-1.5 rounded-sm bg-accent-soft px-2 py-1 text-xs font-medium text-accent tabular-nums"
      style={{ transform: `translate(${left}px, ${top}px)` }}
      aria-hidden="true"
    >
      {text}
    </div>
  );
}
