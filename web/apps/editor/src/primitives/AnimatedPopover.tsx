// AnimatedPopover — Radix Popover.Content with the shared `popover-animate`
// entrance/exit (an opacity fade; components.css). Every popover surface uses
// it — the toolbar dropdowns, the texture palette and ColorButton — so they all
// share one motion (and the a11y-snapshot settle predicate that keys off it).

import * as Popover from "@radix-ui/react-popover";
import { type ComponentProps } from "react";
import { cn } from "@/lib/utils";

type Props = ComponentProps<typeof Popover.Content>;

export function AnimatedPopover({ children, className, ...rest }: Props) {
  return (
    <Popover.Content className={cn("popover-animate", className)} {...rest}>
      {children}
    </Popover.Content>
  );
}
