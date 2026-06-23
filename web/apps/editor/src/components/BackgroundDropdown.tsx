// BackgroundDropdown — toolbar button + Radix Popover replacing the
// BackgroundPicker slide-in ToolPanel.
//
// Trigger: "Background:" label + preview swatch (background colour or
// active skydome's swatch) + chevron. Click opens the popover beneath.
// Popover content reuses BackgroundPickerBody — the Game dome (Land/Space)
// + Solid colour surface (the custom skydome-texture slots were removed).

import * as Popover from "@radix-ui/react-popover";
import { ChevronDown } from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { AnimatedPopover } from "@/components/AnimatedPopover";
import { useEngineSnapshot } from "@/lib/use-engine-snapshot";
import { BackgroundPickerBody } from "@/screens/BackgroundPicker";
import { colorrefToHex } from "@/lib/colorref";

type Props = { bridge: Bridge };

export function BackgroundDropdown({ bridge }: Props) {
  const snap = useEngineSnapshot(bridge);

  const slot = snap?.skydomeSlot ?? 0;
  ///S49] a game dome takes render precedence — but show the dome swatch
  // only when it actually loaded (status "ok"), mirroring the picker body's
  // gameDomeRendering. A selected-but-failed dome falls back to the simple
  // background, so its swatch must too (not paint the dome gradient).
  const gameDomeRendering =
    (snap?.skydomePrimaryStatus ?? "none") === "ok" ||
    (snap?.skydomeSecondaryStatus ?? "none") === "ok";
  const swatchStyle: React.CSSProperties = gameDomeRendering
    ? { background: "linear-gradient(180deg, #2a3a5a 0%, #141c2b 100%)" }
    : slot === 0
      ? { backgroundColor: snap ? colorrefToHex(snap.background) : "#000000" }
      : { backgroundColor: "var(--bg-3)" }; // legacy texture slot — no thumbnail

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
        <AnimatedPopover
          align="end"
          sideOffset={6}
          className="bg-panel border border-border-2 rounded-token shadow-[var(--shadow-soft)] p-3 min-w-[280px] z-50"
        >
          <BackgroundPickerBody bridge={bridge} />
        </AnimatedPopover>
      </Popover.Portal>
    </Popover.Root>
  );
}
