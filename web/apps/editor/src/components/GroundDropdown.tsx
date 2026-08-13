// GroundDropdown — toolbar button + Radix Popover for the
// GroundTexturePanel body. Mirrors BackgroundDropdown
// from Task 2.2: a trigger with a preview swatch + ChevronDown opens
// a popover containing the existing picker body markup.

import { ChevronDown } from "lucide-react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { ToolbarPickerPopover } from "@/components/ToolbarPickerPopover";
import { useEngineField } from "@/lib/use-engine-snapshot";
import {
  GroundTexturePanelBody,
  BUNDLED_GROUND_SLOTS,
} from "@/screens/GroundTexturePanel";
import { colorrefToHex } from "@/lib/colorref";

type Props = { bridge: Bridge };

const SOLID_COLOR_SLOT = 4;

export function GroundDropdown({ bridge }: Props) {
  const slot = useEngineField(bridge, (s) => s.groundTexture) ?? 0;
  const groundSolidColor = useEngineField(bridge, (s) => s.groundSolidColor);
  const bundled = BUNDLED_GROUND_SLOTS.find((s) => s.slot === slot);
  // #888888 depicts the engine's default solid ground color (scene state,
  // not UI chrome) — intentionally outside the token system, like the
  // BackgroundDropdown scene swatches (theming audit 2026-07-18).
  const swatchStyle: React.CSSProperties = slot === SOLID_COLOR_SLOT
    ? { backgroundColor: groundSolidColor === undefined ? "#888888" : colorrefToHex(groundSolidColor) }
    : bundled
      ? { background: bundled.gradient }
      : { backgroundColor: "var(--bg-3)" }; // custom slot — no thumbnail yet

  return (
    <ToolbarPickerPopover
      panelClassName="bg-panel border border-border-2 rounded-token shadow-[var(--shadow-soft)] p-3 min-w-[280px] z-50"
      trigger={
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
      }
    >
      <GroundTexturePanelBody bridge={bridge} />
    </ToolbarPickerPopover>
  );
}
