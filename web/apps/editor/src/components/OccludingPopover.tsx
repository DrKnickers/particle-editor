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

export function OccludingPopover({ bridge, occlusionId, children, ...rest }: Props) {
  const ref = useRef<HTMLDivElement | null>(null);
  useViewportOcclusion(bridge, occlusionId, ref, 24, 24);
  return (
    <Popover.Content {...rest}>
      <div ref={ref}>{children}</div>
    </Popover.Content>
  );
}
