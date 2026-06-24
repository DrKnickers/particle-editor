// Tip — the shared styled+animated tooltip primitive, replacing
// native `title` attributes app-wide.
//
//   <Tip content="Save the file"><button aria-label="Save">…</button></Tip>
//
// - Trigger is asChild: the existing element IS the trigger; no wrapper.
// - content: string → padded plain tier; JSX → rich tier (brings its own
//   padding, e.g. ChainWarningTip's amber band). Nullish/empty → the bare
//   child renders with no tooltip at all (conditional T4 sites).
// - Motion/styling: `tip-animate` (the Radix Content) carries the fast-tier fade
//   + 4px directional slip keyed off Radix data-state/data-side, reduced-motion
//   guarded; the inner `tip-surface` div wears the visual (bg/border/--shadow-soft
//   + overflow:hidden) so its corner-clip never clips the Arrow. Both in
//   components.css.
//
// Disabled triggers (T6): disabled elements fire no pointer events — wrap
// the disabled element in <span className="inline-block"> at the call site
// and put <Tip> on the span.

import * as Tooltip from "@radix-ui/react-tooltip";
import { type ReactNode, type ReactElement } from "react";

type TipProps = {
  content: ReactNode;
  side?: "top" | "right" | "bottom" | "left";
  align?: "start" | "center" | "end";
  children: ReactElement;
};

export function Tip({ content, side = "top", align = "center", children }: TipProps) {
  // No hooks above this return — the early-out is render-order safe even
  // when a conditional site's content flips between string and undefined.
  if (content === null || content === undefined || content === "") return children;
  const body = typeof content === "string" ? <span className="tip-body">{content}</span> : content;
  return (
    <Tooltip.Root>
      <Tooltip.Trigger asChild>{children}</Tooltip.Trigger>
      <Tooltip.Portal>
        {/* Animation lives on the Content; the visual surface (incl. its
            overflow:hidden corner-clip) is an INNER wrapper so it never clips
            the Arrow — keeping the arrow in sync with the surface's entrance
            instead of popping in a beat late. */}
        <Tooltip.Content className="tip-animate" side={side} align={align} sideOffset={6} collisionPadding={8}>
          <div className="tip-surface">{body}</div>
          <Tooltip.Arrow className="tip-arrow" width={10} height={5} />
        </Tooltip.Content>
      </Tooltip.Portal>
    </Tooltip.Root>
  );
}
