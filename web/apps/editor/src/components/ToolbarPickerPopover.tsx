import * as Popover from "@radix-ui/react-popover";
import type { ReactElement, ReactNode } from "react";
import { AnimatedPopover } from "@/components/AnimatedPopover";

type Props = {
  open?: boolean;
  onOpenChange?: (open: boolean) => void;
  trigger: ReactElement;
  panelClassName: string;
  children: ReactNode;
};

/** Shared toolbar-triggered picker shell; callers retain their exact trigger and body markup. */
export function ToolbarPickerPopover({ open, onOpenChange, trigger, panelClassName, children }: Props) {
  return (
    <Popover.Root open={open} onOpenChange={onOpenChange}>
      <Popover.Trigger asChild>{trigger}</Popover.Trigger>
      <Popover.Portal>
        <AnimatedPopover align="end" sideOffset={6} className={panelClassName}>
          {children}
        </AnimatedPopover>
      </Popover.Portal>
    </Popover.Root>
  );
}
