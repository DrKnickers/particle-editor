// ReferenceObjectPopover — toolbar button + Radix Popover for picking a
// game/mod object to place in the preview as a scale reference. Mirrors
// BackgroundPopover; the popover content reuses ReferenceObjectPopoverBody.

import { useState } from "react";
import { ChevronDown } from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { ToolbarPickerPopover } from "@/components/ToolbarPickerPopover";
import { useOpenPickerMessage } from "@/lib/use-open-picker-message";
import { useEngineField } from "@/lib/use-engine-snapshot";
import { ReferenceObjectPopoverBody } from "@/screens/ReferenceObjectPopoverBody";

type Props = { bridge: Bridge };

export function ReferenceObjectPopover({ bridge }: Props) {
  const [open, setOpen] = useState(false);
  const name = useEngineField(bridge, (s) => s.referenceObjectName) ?? "";
  const label = name === "" ? "None" : name;

  useOpenPickerMessage("reference", setOpen);

  return (
    <ToolbarPickerPopover
      open={open}
      onOpenChange={setOpen}
      panelClassName="bg-panel border border-border-2 rounded-token shadow-[var(--shadow-soft)] p-3 min-w-[280px] max-w-md z-50"
      trigger={
        <button type="button" className="tb-btn" aria-label="Reference object picker">
          <span>Object:</span>
          <span className="max-w-[10rem] truncate text-text-2">{label}</span>
          <ChevronDown className="size-3.5" aria-hidden="true" />
        </button>
      }
    >
      <ReferenceObjectPopoverBody bridge={bridge} />
    </ToolbarPickerPopover>
  );
}
