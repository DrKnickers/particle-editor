// ReferenceObjectDropdown — toolbar button + Radix Popover for picking a
// game/mod object to place in the preview as a scale reference. Mirrors
// BackgroundDropdown; the popover content reuses ReferenceObjectPickerBody.

import * as Popover from "@radix-ui/react-popover";
import { useEffect, useState } from "react";
import { ChevronDown } from "lucide-react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { OccludingPopover } from "@/components/OccludingPopover";
import { ReferenceObjectPickerBody } from "@/screens/ReferenceObjectPicker";

type Props = { bridge: Bridge };

export function ReferenceObjectDropdown({ bridge }: Props) {
  const [snap, setSnap] = useState<EngineStateDto | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge.request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => { if (!cancelled) setSnap(s); })
      .catch(() => { /* ignore */ });
    const off = bridge.on("engine/state/changed", (e) => setSnap(e.payload));
    return () => { cancelled = true; off(); };
  }, [bridge]);

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
        <OccludingPopover
          bridge={bridge}
          occlusionId="popover:reference-object"
          align="end"
          sideOffset={6}
          className="bg-panel border border-border-2 rounded-token shadow-[var(--shadow)] p-3 min-w-[280px] max-w-md z-50"
        >
          <ReferenceObjectPickerBody bridge={bridge} />
        </OccludingPopover>
      </Popover.Portal>
    </Popover.Root>
  );
}
