// ColorButton.tsx — swatch button that opens a Radix Popover color picker.
//
// Contains:
//   - 16-slot custom-color grid (Zustand palette-store, localStorage-persisted)
//   - 32-slot basic-colors preset (matches Win32 ChooseColor's left column)
//   - Hex input + RGB sliders for custom entry
//   - "Add to custom" button
//
// Flow:
//   - Clicking the swatch button opens a sticky Radix Popover.
//   - Clicking a basic/custom color fires onChange(rgb) immediately; popover
//     stays open (sticky-on-commit pattern from BackgroundPicker).
//   - "Add to custom" stores the picker's current color in the next empty slot.
//
// NOT routed through native ChooseColor — pure React, safe for CDP test mode.

import { useState, useCallback } from "react";
import { Tip } from "@/primitives/Tip";
import { useRovingIndex } from "@/lib/use-roving-index";
import * as Popover from "@radix-ui/react-popover";
import type { RgbColor } from "./palette-store";
import { usePaletteStore } from "./palette-store";
import type { SpinnerDensity } from "./Spinner";

// Win32 ChooseColor basic colors — 32 entries (4 rows × 8 columns).
// Colors chosen to match the standard Windows CHOOSECOLOR palette.
const BASIC_COLORS: readonly RgbColor[] = [
  { r: 255, g: 128, b: 128 }, { r: 255, g: 255, b: 128 }, { r: 128, g: 255, b: 128 }, { r: 0,   g: 255, b: 128 },
  { r: 128, g: 255, b: 255 }, { r: 0,   g: 128, b: 255 }, { r: 255, g: 128, b: 192 }, { r: 255, g: 128, b: 255 },
  { r: 255, g: 0,   b: 0   }, { r: 255, g: 255, b: 0   }, { r: 128, g: 255, b: 0   }, { r: 0,   g: 255, b: 64  },
  { r: 0,   g: 255, b: 255 }, { r: 0,   g: 128, b: 192 }, { r: 128, g: 128, b: 192 }, { r: 255, g: 0,   b: 255 },
  { r: 128, g: 64,  b: 64  }, { r: 255, g: 128, b: 64  }, { r: 0,   g: 255, b: 0   }, { r: 0,   g: 128, b: 128 },
  { r: 0,   g: 64,  b: 128 }, { r: 128, g: 128, b: 255 }, { r: 128, g: 0,   b: 64  }, { r: 255, g: 0,   b: 128 },
  { r: 128, g: 0,   b: 0   }, { r: 255, g: 128, b: 0   }, { r: 0,   g: 128, b: 0   }, { r: 0,   g: 128, b: 64  },
  { r: 0,   g: 0,   b: 255 }, { r: 0,   g: 0,   b: 160 }, { r: 128, g: 0,   b: 128 }, { r: 128, g: 0,   b: 255 },
] as const;

function rgbToHex({ r, g, b }: RgbColor): string {
  return (
    "#" +
    [r, g, b]
      .map((c) => Math.round(Math.max(0, Math.min(255, c))).toString(16).padStart(2, "0"))
      .join("")
  );
}

function hexToRgb(hex: string): RgbColor | null {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex.trim());
  if (!m) return null;
  return { r: parseInt(m[1], 16), g: parseInt(m[2], 16), b: parseInt(m[3], 16) };
}

function clampByte(v: number): number {
  return Math.round(Math.max(0, Math.min(255, v)));
}

export type ColorButtonProps = {
  value: RgbColor;
  onChange: (color: RgbColor) => void;
  density?: SpinnerDensity;
  disabled?: boolean;
  "aria-label"?: string;
};

export function ColorButton({
  value,
  onChange,
  density = "default",
  disabled = false,
  "aria-label": ariaLabel = "Pick color",
}: ColorButtonProps) {
  const { slots, addColor, setSlot } = usePaletteStore();
  // In-flight picker state. `pickerColor` previews live; `originalColor` is the
  // color the picker opened with, so Cancel/Escape can restore it (the engine has
  // no undo of its own). Both are (re)snapshotted on each open.
  const [pickerColor, setPickerColor] = useState<RgbColor>(value);
  const [originalColor, setOriginalColor] = useState<RgbColor>(value);
  const [hexText, setHexText] = useState<string>(rgbToHex(value).slice(1).toUpperCase());
  // Transient invalid flag for the hex field (design pass, C4): a rejected
  // commit used to silently revert with zero feedback; now the field flashes
  // the shared aria-invalid danger border until the next valid input.
  const [hexInvalid, setHexInvalid] = useState(false);
  const [open, setOpen] = useState(false);

  // Roving tabindex per swatch grid (design pass, B5): one Tab stop each
  // instead of 48 sequential stops through the picker.
  const basicRoving = useRovingIndex(BASIC_COLORS.length, { columns: 8 });
  const customRoving = useRovingIndex(slots.length, { columns: 8 });

  const swatchStyle = { backgroundColor: rgbToHex(value) };

  const handleSelectColor = useCallback((color: RgbColor) => {
    setPickerColor(color);
    setHexText(rgbToHex(color).slice(1).toUpperCase());
    onChange(color);
  }, [onChange]);

  const handleHexChange = (raw: string) => {
    setHexText(raw.toUpperCase());
    const rgb = hexToRgb(raw);
    if (rgb) {
      setHexInvalid(false);
      setPickerColor(rgb);
      onChange(rgb); // Live preview on each valid hex.
    }
  };

  const handleHexCommit = () => {
    const rgb = hexToRgb(hexText);
    if (rgb) {
      setHexInvalid(false);
      setPickerColor(rgb);
      onChange(rgb);
    } else {
      // Revert hex text to the current picker color on invalid input, and
      // flag the field so the rejection is visible (C4).
      setHexInvalid(true);
      setHexText(rgbToHex(pickerColor).slice(1).toUpperCase());
    }
  };

  const handleSliderChange = (channel: "r" | "g" | "b", v: number) => {
    const next = { ...pickerColor, [channel]: clampByte(v) };
    setPickerColor(next);
    setHexText(rgbToHex(next).slice(1).toUpperCase());
    onChange(next); // Live preview — drive the engine as the value changes.
  };

  // Re-commit the open-time color. Shared by the Cancel button + Escape.
  const handleCancel = () => {
    onChange(originalColor);
    setPickerColor(originalColor);
    setHexText(rgbToHex(originalColor).slice(1).toUpperCase());
  };

  // Single funnel for every open/close. On the open transition, snapshot the
  // pre-edit color (and re-sync pickerColor, which would otherwise stay at the
  // mount value). Outside-click / re-clicking the trigger land here too = keep.
  const handleOpenChange = (next: boolean) => {
    if (next && !open) {
      setOriginalColor(value);
      setPickerColor(value);
      setHexText(rgbToHex(value).slice(1).toUpperCase());
    }
    setOpen(next);
  };

  const handleAddToCustom = () => {
    addColor(pickerColor);
  };

  const HEIGHT_MAP = { tight: "h-[var(--row-h-sm)]", default: "h-[var(--row-h)]", loose: "h-[32px]" };

  return (
    <Popover.Root open={open} onOpenChange={handleOpenChange}>
      <Popover.Trigger asChild>
        <button
          type="button"
          disabled={disabled}
          aria-label={ariaLabel}
          className={`flex items-center gap-1.5 rounded border border-border-2 bg-bg-2 px-2 text-xs text-text-2 transition hover:border-border-2 disabled:cursor-not-allowed disabled:opacity-40 focus-ring ${HEIGHT_MAP[density]}`}
        >
          <span
            className="inline-block size-3 rounded-sm border border-border-2"
            style={swatchStyle}
            aria-hidden="true"
          />
          <span className="font-mono text-[10px]">{rgbToHex(value).toUpperCase()}</span>
        </button>
      </Popover.Trigger>

      <Popover.Portal>
        <Popover.Content
          side="bottom"
          align="start"
          sideOffset={4}
          className="z-50 w-72 rounded-md border border-border-2 bg-bg-2 p-3 shadow-[var(--shadow-soft)] popover-animate"
          onOpenAutoFocus={(e) => e.preventDefault()}
          onEscapeKeyDown={handleCancel}
        >
          {/* Basic colors — 4 rows × 8 columns = 32 slots. Roving tabindex
              (design pass, B5): each grid is ONE Tab stop; arrows move within
              it (Up/Down jump a row of 8). */}
          <div className="mb-2">
            <div className="mb-1 text-[10px] text-text-3">Basic colors</div>
            <div className="grid grid-cols-8 gap-0.5">
              {BASIC_COLORS.map((color, i) => (
                <Tip key={i} content={rgbToHex(color).toUpperCase()}>
                  <button
                    type="button"
                    aria-label={`Basic color ${rgbToHex(color).toUpperCase()}`}
                    {...basicRoving.itemProps(i)}
                    onClick={() => handleSelectColor(color)}
                    className="size-5 rounded-sm border border-transparent hover:border-border-2 focus-ring"
                    style={{ backgroundColor: rgbToHex(color) }}
                  />
                </Tip>
              ))}
            </div>
          </div>

          {/* Custom colors — 2 rows × 8 columns = 16 slots (own roving group). */}
          <div className="mb-3">
            <div className="mb-1 text-[10px] text-text-3">Custom colors</div>
            <div className="grid grid-cols-8 gap-0.5">
              {slots.map((color, i) => (
                <Tip key={i} content={color ? rgbToHex(color).toUpperCase() : "Empty"}>
                  <button
                    type="button"
                    aria-label={color ? `Custom color ${rgbToHex(color).toUpperCase()}` : `Custom slot ${i + 1} (empty)`}
                    {...customRoving.itemProps(i)}
                    onClick={() => { if (color) handleSelectColor(color); }}
                    onContextMenu={(e) => { e.preventDefault(); setSlot(i, null); }}
                    className={`size-5 rounded-sm border hover:border-border-2 focus-ring ${
                      color ? "border-border-2" : "border-dashed border-border-2"
                    }`}
                    style={color ? { backgroundColor: rgbToHex(color) } : { backgroundColor: "transparent" }}
                  />
                </Tip>
              ))}
            </div>
          </div>

          {/* Hex input */}
          <div className="mb-2 flex items-center gap-2">
            <span className="text-[10px] text-text-3">#</span>
            <input
              type="text"
              value={hexText}
              onChange={(e) => handleHexChange(e.target.value)}
              onBlur={handleHexCommit}
              onKeyDown={(e) => { if (e.key === "Enter") { handleHexCommit(); setOpen(false); } }}
              maxLength={6}
              className="w-20 rounded border border-border-2 bg-panel-2 px-2 py-0.5 font-mono text-xs text-text outline-none transition-colors motion-reduce:transition-none focus:border-accent"
              aria-label="Hex color input"
              aria-invalid={hexInvalid || undefined}
              spellCheck={false}
            />
            <span
              className="inline-block size-5 rounded border border-border-2"
              style={{ backgroundColor: rgbToHex(pickerColor) }}
              aria-hidden="true"
            />
          </div>

          {/* RGB sliders */}
          <div className="mb-3 space-y-1.5">
            {(["r", "g", "b"] as const).map((ch) => (
              <div key={ch} className="flex items-center gap-2">
                <span className="w-3 text-[10px] text-text-3 uppercase">{ch}</span>
                <input
                  type="range"
                  min={0}
                  max={255}
                  value={pickerColor[ch]}
                  onChange={(e) => handleSliderChange(ch, parseInt(e.target.value, 10))}
                  className="flex-1 accent-[var(--accent-strong)]"
                  aria-label={`${ch.toUpperCase()} channel`}
                />
                <input
                  type="number"
                  min={0}
                  max={255}
                  value={pickerColor[ch]}
                  onChange={(e) => {
                    const n = parseInt(e.target.value, 10);
                    if (!Number.isNaN(n)) handleSliderChange(ch, n);
                  }}
                  className="w-12 rounded border border-border-2 bg-panel-2 px-1 py-0.5 text-right font-mono text-[10px] text-text-2 outline-none transition-colors motion-reduce:transition-none focus:border-accent"
                  aria-label={`${ch.toUpperCase()} value`}
                />
              </div>
            ))}
          </div>

          {/* Before / after preview + Cancel / OK. The swatch pair makes the
              Cancel affordance discoverable: "Cancel goes back to Original". */}
          <div className="mb-2 flex items-center gap-2">
            <div className="flex items-center gap-1.5" aria-hidden="true">
              <div className="flex flex-col items-center gap-0.5">
                <span className="text-[9px] text-text-3">Original</span>
                <span
                  data-testid="color-original"
                  className="inline-block h-5 w-7 rounded-sm border border-border-2"
                  style={{ backgroundColor: rgbToHex(originalColor) }}
                />
              </div>
              <div className="flex flex-col items-center gap-0.5">
                <span className="text-[9px] text-text-3">New</span>
                <span
                  className="inline-block h-5 w-7 rounded-sm border border-border-2"
                  style={{ backgroundColor: rgbToHex(pickerColor) }}
                />
              </div>
            </div>
            <div className="ml-auto flex items-center gap-1.5">
              <button
                type="button"
                onClick={() => { handleCancel(); setOpen(false); }}
                className="rounded border border-border-2 bg-panel-2 px-3 py-1 text-[10px] text-text-2 hover:bg-panel-3 hover:text-text focus-ring"
              >
                Cancel
              </button>
              <button
                type="button"
                onClick={() => setOpen(false)}
                className="rounded border border-accent-strong bg-accent-strong px-3 py-1 text-[10px] font-medium text-white hover:opacity-90 focus-ring"
              >
                OK
              </button>
            </div>
          </div>

          {/* Add to custom */}
          <button
            type="button"
            onClick={handleAddToCustom}
            className="w-full rounded border border-border-2 bg-panel-2 px-2 py-1 text-[10px] text-text-2 hover:bg-panel-3 hover:text-text focus-ring"
          >
            Add to custom colors
          </button>

          {/* Matches the popover surface (bg-bg-2) so it theme-flips; was a
              Tailwind palette gray that broke on the light theme. */}
          <Popover.Arrow className="fill-[var(--bg-2)]" />
        </Popover.Content>
      </Popover.Portal>
    </Popover.Root>
  );
}
