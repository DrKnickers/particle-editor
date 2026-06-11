// TexturePalette.tsx — 64×64 thumbnail grid with Radix ContextMenu.
//
// Items provided by caller as:
//   { path: string; label?: string; thumbnailSrc: string | null }[]
//
// Primitive doesn't fetch anything. thumbnailSrc === null renders a
// "missing" placeholder cell. Selected cell (value === item.path) gets
// accent.primary border. Empty palette: greyed "(no textures)" placeholder.
//
// Right-click → Radix ContextMenu:
//   - Browse for file… → calls onBrowse?(item.path)
//   - Clear           → calls onClear?(item.path)
//   - Open texture folder → calls onReveal?(item.path)
//   Items with no callback render as disabled.

import * as ContextMenu from "@radix-ui/react-context-menu";
import { Tip } from "./Tip";
import type { SpinnerDensity } from "./Spinner";

export type TextureItem = {
  path: string;
  label?: string;
  thumbnailSrc: string | null;
};

export type TexturePaletteProps = {
  items: TextureItem[];
  value: string | null;
  onChange: (path: string) => void;
  cellSize?: number;
  density?: SpinnerDensity;
  onBrowse?: (path: string) => void;
  onClear?: (path: string) => void;
  onReveal?: (path: string) => void;
};

function MissingPlaceholder({ size }: { size: number }) {
  return (
    <div
      className="flex items-center justify-center rounded border border-dashed border-border-2 bg-panel-2 text-text-3"
      style={{ width: size, height: size }}
      aria-label="Missing texture"
    >
      <span style={{ fontSize: Math.max(8, size * 0.18) }}>?</span>
    </div>
  );
}

export function TexturePalette({
  items,
  value,
  onChange,
  cellSize = 64,
  onBrowse,
  onClear,
  onReveal,
}: TexturePaletteProps) {
  if (items.length === 0) {
    return (
      <div className="flex items-center justify-center rounded border border-dashed border-border-2 p-4 text-xs text-text-3">
        (no textures)
      </div>
    );
  }

  return (
    <div className="flex flex-wrap gap-1" role="listbox" aria-label="Texture palette">
      {items.map((item) => {
        const selected = value === item.path;
        return (
          <ContextMenu.Root key={item.path}>
            {/* Tip wraps the ContextMenu.Trigger (not the button inside it) —
                Tooltip.Trigger asChild around another Radix trigger is the
                blessed nesting; both forward their props down to the button.
                Static occlusionId on grid cells is safe: only ONE tooltip is
                ever open at a time (app-level Radix Tooltip.Provider). */}
            <Tip content={item.label ?? item.path} occlusionId="tip:texpal:item">
              <ContextMenu.Trigger asChild>
                <button
                  type="button"
                  role="option"
                  aria-selected={selected}
                  aria-label={item.label ?? item.path}
                  onClick={() => onChange(item.path)}
                  className={`relative overflow-hidden rounded border-2 transition focus:outline-none focus:ring-1 focus:ring-accent ${
                    selected
                      ? "border-accent"
                      : "border-border-2 hover:border-border-2"
                  }`}
                  style={{ width: cellSize, height: cellSize }}
                >
                  {item.thumbnailSrc !== null ? (
                    <img
                      src={item.thumbnailSrc}
                      alt={item.label ?? item.path}
                      className="h-full w-full object-cover"
                      draggable={false}
                    />
                  ) : (
                    <MissingPlaceholder size={cellSize - 4} />
                  )}
                  {item.label && (
                    <span className="absolute inset-x-0 bottom-0 truncate bg-bg/80 px-0.5 text-center text-[9px] text-text">
                      {item.label}
                    </span>
                  )}
                </button>
              </ContextMenu.Trigger>
            </Tip>

            <ContextMenu.Portal>
              <ContextMenu.Content
                className="z-50 min-w-[160px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-xl"
              >
                <ContextMenu.Item
                  disabled={!onBrowse}
                  onSelect={() => onBrowse?.(item.path)}
                  className="flex cursor-pointer items-center rounded px-2 py-1 text-xs text-text data-[disabled]:cursor-not-allowed data-[disabled]:text-text-3 data-[highlighted]:bg-panel-2"
                >
                  Browse for file…
                </ContextMenu.Item>
                <ContextMenu.Item
                  disabled={!onClear}
                  onSelect={() => onClear?.(item.path)}
                  className="flex cursor-pointer items-center rounded px-2 py-1 text-xs text-text data-[disabled]:cursor-not-allowed data-[disabled]:text-text-3 data-[highlighted]:bg-panel-2"
                >
                  Clear
                </ContextMenu.Item>
                <ContextMenu.Item
                  disabled={!onReveal}
                  onSelect={() => onReveal?.(item.path)}
                  className="flex cursor-pointer items-center rounded px-2 py-1 text-xs text-text data-[disabled]:cursor-not-allowed data-[disabled]:text-text-3 data-[highlighted]:bg-panel-2"
                >
                  Open texture folder
                </ContextMenu.Item>
              </ContextMenu.Content>
            </ContextMenu.Portal>
          </ContextMenu.Root>
        );
      })}
    </div>
  );
}
