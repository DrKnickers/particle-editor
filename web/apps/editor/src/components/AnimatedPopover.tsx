// AnimatedPopover — Radix Popover.Content with the shared `popover-animate`
// entrance (fade + slight zoom). Toolbar / appearance dropdowns use it so
// they all share one entrance/exit motion.

import * as Popover from "@radix-ui/react-popover";
import { type ComponentProps } from "react";

type Props = ComponentProps<typeof Popover.Content>;

export function AnimatedPopover({ children, className, ...rest }: Props) {
  // `popover-animate` (components.css) gives every toolbar/appearance
  // popover the shared fade+slight-zoom entrance/exit. Prepended so the
  // caller's structural classes still apply; it's a plain CSS class with
  // no Tailwind-utility conflict, so a simple join is safe.
  const merged = className ? `popover-animate ${className}` : "popover-animate";
  return (
    <Popover.Content className={merged} {...rest}>
      {children}
    </Popover.Content>
  );
}
