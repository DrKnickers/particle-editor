// ReferenceObjectDropdown — toolbar button + Radix Popover for picking a
// game/mod object to place in the preview as a scale reference. Mirrors
// BackgroundDropdown; the popover content reuses ReferenceObjectPickerBody.

import * as Popover from "@radix-ui/react-popover";
import { ChevronDown } from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { AnimatedPopover } from "@/components/AnimatedPopover";
import { useEngineSnapshot } from "@/lib/use-engine-snapshot";
import { ReferenceObjectPickerBody } from "@/screens/ReferenceObjectPicker";

type Props = { bridge: Bridge };

export function ReferenceObjectDropdown({ bridge }: Props) {
  const snap = useEngineSnapshot(bridge);

  const name = snap?.referenceObjectName ?? "";
  const label = name === "" ? "None" : name;

  return (
    <Popover.Root>
      <Popover.Trigger asChild>
        <button type="button" className="tb-btn" aria-label="Reference object picker">
          <span>Object:</span>
          <span className="max-w-[10rem] truncate text-text-2">{label}</span>
          <ChevronDown className="size-3.5" aria-hidden="true" />
        </button>
      </Popover.Trigger>
      <Popover.Portal>
        <AnimatedPopover
          align="end"
          sideOffset={6}
          className="bg-panel border border-border-2 rounded-token shadow-[var(--shadow-soft)] p-3 min-w-[280px] max-w-md z-50"
        >
          <ReferenceObjectPickerBody bridge={bridge} />
        </AnimatedPopover>
      </Popover.Portal>
    </Popover.Root>
  );
}
