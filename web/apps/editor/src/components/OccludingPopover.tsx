// OccludingPopover — Radix Popover Content wrapped to register itself
// with the host as a viewport occlusion (so the AlphaCompositor stamps
// the cut-out). Identical pattern to OccludingMenubarContent in
// MenuBar.tsx; this version wraps Popover.Content for toolbar
// dropdowns rather than menubar triggers.
//
// Padding (24px) + smoothstep feather (24px) match the values
// the menubar dropdowns use — same shadow-xl drop shadow needs the
// same enclosure.

import * as Popover from "@radix-ui/react-popover";
import { useRef, type ComponentProps } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useViewportOcclusion } from "@/lib/viewport-occlusion";

type Props = ComponentProps<typeof Popover.Content> & {
  bridge: Bridge;
  occlusionId: string;
};

export function OccludingPopover({
  bridge,
  occlusionId,
  children,
  className,
  ...rest
}: Props) {
  const ref = useRef<HTMLDivElement | null>(null);
  useViewportOcclusion(bridge, occlusionId, ref, 24, 24);
  // `popover-animate` (components.css) gives every toolbar/appearance
  // popover the shared fade+slight-zoom entrance/exit. Prepended so the
  // caller's structural classes still apply; it's a plain CSS class with
  // no Tailwind-utility conflict, so a simple join is safe.
  const merged = className ? `popover-animate ${className}` : "popover-animate";
  return (
    <Popover.Content className={merged} {...rest}>
      <div ref={ref}>{children}</div>
    </Popover.Content>
  );
}
