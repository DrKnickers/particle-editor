// Tip — the shared styled+animated tooltip primitive, replacing
// native `title` attributes app-wide.
//
//   <Tip content="Save the file"><button aria-label="Save">…</button></Tip>
//
// - Trigger is asChild: the existing element IS the trigger; no wrapper.
// - content: string → padded plain tier; JSX → rich tier (brings its own
//   padding, e.g. ChainWarningTip's amber band). Nullish/empty → the bare
//   child renders with no tooltip at all (conditional T4 sites).
// - occlusionId (opt-in): registers a viewport occlusion while open so the
//   D3D-composited viewport popup doesn't overpaint the portaled tooltip
//   (the OccludingPopover precedent — see spec §3). Sites that can't reach
//   the viewport skip it; when in doubt, opt in.
// - Motion/styling: `tip-surface tip-animate` in components.css — fast-tier
//   fade + 4px directional slip keyed off Radix data-state/data-side,
//   reduced-motion guarded. Surface wears --shadow-soft.
//
// Disabled triggers (T6): disabled elements fire no pointer events — wrap
// the disabled element in <span className="inline-block"> at the call site
// and put <Tip> on the span.

import * as Tooltip from "@radix-ui/react-tooltip";
import { useLayoutEffect, useRef, useState, type ReactNode, type ReactElement } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useBridge } from "@/lib/bridge-context";
import { useViewportOcclusion } from "@/lib/viewport-occlusion";

type TipProps = {
  content: ReactNode;
  side?: "top" | "right" | "bottom" | "left";
  align?: "start" | "center" | "end";
  occlusionId?: string;
  children: ReactElement;
};

// Hooks live in a child component so they only run while the content is
// mounted (Radix mounts Content only while open) — same shape as
// OccludingPopover. pad/feather 12/12: the soft shadow's extent is smaller
// than the menus' shadow-xl, so the 24/24 enclosure would be oversized.
function OccludingTipBody({ id, children }: { id: string; children: ReactNode }) {
  const bridge = useBridge();
  const ref = useRef<HTMLDivElement | null>(null);
  // Radix Tooltip mounts Content's children TWICE: the visible copy plus an
  // `aria` duplicate inside a VisuallyHidden span carrying role="tooltip"
  // (verified in @radix-ui/react-tooltip dist — `ariaLabel || children`).
  // The duplicate's instance must NOT register: it mounts after the visible
  // copy, so its 1x1 hidden rect would overwrite the real occlusion under
  // the same id. Radix's own Arrow null-outs via an internal context that
  // isn't exported, so we detect the hidden copy from the DOM instead.
  // Registration is deferred behind state (starts disarmed) so neither copy
  // races the check: the layout effect arms only the visible instance, and
  // only then does useViewportOcclusion see a bridge and register.
  const [armedBridge, setArmedBridge] = useState<Bridge | undefined>(undefined);
  useLayoutEffect(() => {
    if (!ref.current?.closest('[role="tooltip"]')) setArmedBridge(bridge ?? undefined);
  }, [bridge]);
  useViewportOcclusion(armedBridge, id, ref, 12, 12);
  return <div ref={ref}>{children}</div>;
}

export function Tip({ content, side = "top", align = "center", occlusionId, children }: TipProps) {
  // No hooks above this return — the early-out is render-order safe even
  // when a conditional site's content flips between string and undefined.
  if (content === null || content === undefined || content === "") return children;
  const body = typeof content === "string" ? <span className="tip-body">{content}</span> : content;
  return (
    <Tooltip.Root>
      <Tooltip.Trigger asChild>{children}</Tooltip.Trigger>
      <Tooltip.Portal>
        <Tooltip.Content className="tip-surface tip-animate" side={side} align={align} sideOffset={6} collisionPadding={8}>
          {occlusionId ? <OccludingTipBody id={occlusionId}>{body}</OccludingTipBody> : body}
          <Tooltip.Arrow className="tip-arrow" width={10} height={5} />
        </Tooltip.Content>
      </Tooltip.Portal>
    </Tooltip.Root>
  );
}
