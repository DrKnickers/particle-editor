// SelectedBadge — the round ✓ chip that marks the selected tile in the picker
// grids (background solid colour, ground slots). Decorative: the tile's own
// aria-pressed carries the state. `corner` picks where it sits inside the
// (relative) tile; omit it to place the chip in normal flow.

import { cn } from "@/lib/utils";

export function SelectedBadge({ corner }: { corner?: "top-right" | "top-left" }) {
  return (
    <span
      aria-hidden="true"
      className={cn(
        "flex size-5 items-center justify-center rounded-full bg-accent-strong text-xs text-white",
        corner === "top-right" && "absolute right-1 top-1",
        corner === "top-left" && "absolute left-1 top-1",
      )}
    >
      ✓
    </span>
  );
}
