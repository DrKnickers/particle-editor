// BackgroundDropdown — toolbar button + Radix Popover replacing the
// BackgroundPicker slide-in ToolPanel.
//
// Trigger: "Background:" label + preview swatch (background colour or
// active skydome's swatch) + chevron. Click opens the popover beneath.
// Popover content reuses BackgroundPickerBody — same slot grid, custom
// slots, colour picker chain as the sliding panel had.

import * as Popover from "@radix-ui/react-popover";
import { useEffect, useState } from "react";
import { ChevronDown } from "lucide-react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { OccludingPopover } from "@/components/OccludingPopover";
import { BackgroundPickerBody, BUNDLED_SLOTS } from "@/screens/BackgroundPicker";
import { colorrefToHex } from "@/lib/colorref";

type Props = { bridge: Bridge };

export function BackgroundDropdown({ bridge }: Props) {
  const [snap, setSnap] = useState<EngineStateDto | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge.request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => { if (!cancelled) setSnap(s); })
      .catch(() => { /* ignore */ });
    const off = bridge.on("engine/state/changed", (e) => setSnap(e.payload));
    return () => { cancelled = true; off(); };
  }, [bridge]);

  const slot = snap?.skydomeSlot ?? 0;
  const bundled = BUNDLED_SLOTS.find((s) => s.slot === slot);
  const swatchStyle: React.CSSProperties = slot === 0
    ? { backgroundColor: snap ? colorrefToHex(snap.background) : "#000000" }
    : bundled
      ? { background: bundled.gradient }
      : { backgroundColor: "var(--bg-3)" }; // custom slot — no thumbnail yet

  return (
    <Popover.Root>
      <Popover.Trigger asChild>
        <button
          type="button"
          className="tb-btn"
          aria-label="Background"
        >
          <span>Background:</span>
          <span
            className="inline-block w-4 h-4 rounded-sm border border-border-2"
            style={swatchStyle}
            aria-hidden="true"
          />
          <ChevronDown className="size-3.5" aria-hidden="true" />
        </button>
      </Popover.Trigger>
      <Popover.Portal>
        <OccludingPopover
          bridge={bridge}
          occlusionId="popover:background"
          align="end"
          sideOffset={6}
          className="bg-panel border border-border-2 rounded-token shadow-[var(--shadow)] p-3 min-w-[280px] z-50"
        >
          <BackgroundPickerBody bridge={bridge} />
        </OccludingPopover>
      </Popover.Portal>
    </Popover.Root>
  );
}
