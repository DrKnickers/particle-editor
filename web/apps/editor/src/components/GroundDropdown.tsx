// GroundDropdown — toolbar button + Radix Popover replacing the
// GroundTexturePanel slide-in ToolPanel. Mirrors BackgroundDropdown
// from Task 2.2: a trigger with a preview swatch + ChevronDown opens
// a popover containing the existing picker body markup.

import * as Popover from "@radix-ui/react-popover";
import { useEffect, useState } from "react";
import { ChevronDown } from "lucide-react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { OccludingPopover } from "@/components/OccludingPopover";
import {
  GroundTexturePanelBody,
  BUNDLED_GROUND_SLOTS,
} from "@/screens/GroundTexturePanel";
import { colorrefToHex } from "@/lib/colorref";

type Props = { bridge: Bridge };

const SOLID_COLOR_SLOT = 4;

export function GroundDropdown({ bridge }: Props) {
  const [snap, setSnap] = useState<EngineStateDto | null>(null);

  useEffect(() => {
    let cancelled = false;
    bridge.request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => { if (!cancelled) setSnap(s); })
      .catch(() => { /* ignore */ });
    const off = bridge.on("engine/state/changed", (e) => setSnap(e.payload));
    return () => { cancelled = true; off(); };
  }, [bridge]);

  const slot = snap?.groundTexture ?? 0;
  const bundled = BUNDLED_GROUND_SLOTS.find((s) => s.slot === slot);
  const swatchStyle: React.CSSProperties = slot === SOLID_COLOR_SLOT
    ? { backgroundColor: snap ? colorrefToHex(snap.groundSolidColor) : "#888888" }
    : bundled
      ? { background: bundled.gradient }
      : { backgroundColor: "var(--bg-3)" }; // custom slot — no thumbnail yet

  return (
    <Popover.Root>
      <Popover.Trigger asChild>
        <button
          type="button"
          className="tb-btn"
          aria-label="Ground"
        >
          <span>Ground:</span>
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
          occlusionId="popover:ground"
          align="end"
          sideOffset={6}
          className="bg-panel border border-border-2 rounded-token shadow-[var(--shadow)] p-3 min-w-[280px] z-50"
        >
          <GroundTexturePanelBody bridge={bridge} />
        </OccludingPopover>
      </Popover.Portal>
    </Popover.Root>
  );
}
