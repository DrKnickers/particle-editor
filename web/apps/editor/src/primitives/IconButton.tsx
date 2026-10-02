// IconButton — the shared icon-only button. ONE `label` string feeds both the
// accessible name (aria-label) and the tooltip, so the two can't drift; `tip`
// overrides the tooltip copy only where it deliberately says more than the
// name ("Step" → "Step one frame").
//
//   <IconButton label="Undo" variant="toolbar" disabled={!canUndo} onClick={…}>
//     <Undo2 className="size-3.5" />
//   </IconButton>
//
// Variants — each the single home of a look that used to be copy-pasted:
//   toolbar  the main toolbar's `.tb-btn` (components.css; 28px tall)
//   tree     the emitter-tree footer strip (28px square)
//   toggle   the curve editor's bordered 24px toggles (`pressed` → accent)
//   ghost    a dialog / tool-panel header close glyph (24px)
//   row      a compact in-row action (20px; load-order rows)
//
// `pressed` sets aria-pressed (a toggle). Disabled buttons fire no pointer
// events, so `tipWhenDisabled` wraps the button in an inline-block span the
// tooltip can listen on — the wrapper is rendered in every state so the DOM
// doesn't change shape as the button enables/disables.

import { type ComponentPropsWithRef, type ReactNode } from "react";
import { cn } from "@/lib/utils";
import { Tip } from "@/primitives/Tip";

export type IconButtonVariant = "toolbar" | "tree" | "toggle" | "ghost" | "row";

const VARIANT_CLASS: Record<IconButtonVariant, string> = {
  toolbar: "tb-btn",
  // `:active` pressed state (lighter bg + slight scale), suppressed while
  // disabled — the Tailwind twin of `.tb-btn`'s press feedback.
  tree: "flex h-7 w-7 items-center justify-center rounded text-text-2 transition motion-reduce:transition-none hover:bg-panel-2 hover:text-text active:bg-panel-3 active:scale-95 disabled:cursor-not-allowed disabled:text-text-3 disabled:hover:bg-transparent disabled:active:scale-100 focus-ring",
  toggle: "grid h-6 w-6 place-items-center rounded border transition-colors motion-reduce:transition-none disabled:cursor-not-allowed disabled:opacity-40 focus-ring",
  ghost: "flex size-6 items-center justify-center rounded text-text-2 transition-colors motion-reduce:transition-none hover:bg-panel-2 hover:text-text focus-ring",
  row: "flex size-5 shrink-0 items-center justify-center rounded-[var(--radius-xs)] text-text-2 transition-colors motion-reduce:transition-none hover:bg-hover hover:text-text disabled:pointer-events-none disabled:opacity-40 focus-ring",
};

// The bordered toggle's two states. Off has a real hover (the old copies
// re-stated their resting border on hover — a no-op).
const TOGGLE_OFF = "border-border-2 bg-bg-2 text-text-2 enabled:hover:bg-panel-2 enabled:hover:text-text";
const TOGGLE_ON = "border-accent bg-accent-soft text-accent";

/** The class string for a variant, for the few triggers that must render a
 *  Radix element themselves (e.g. a Menubar.Trigger) rather than an IconButton. */
export function iconButtonClass(variant: IconButtonVariant, pressed?: boolean): string {
  if (variant === "toggle") return cn(VARIANT_CLASS.toggle, pressed ? TOGGLE_ON : TOGGLE_OFF);
  return VARIANT_CLASS[variant];
}

type IconButtonProps = Omit<ComponentPropsWithRef<"button">, "aria-label" | "aria-pressed"> & {
  /** Accessible name — and the tooltip, unless `tip` overrides it. */
  label: string;
  /** Tooltip copy when it differs from `label`; `null` = no tooltip. */
  tip?: ReactNode;
  tipSide?: "top" | "right" | "bottom" | "left";
  variant?: IconButtonVariant;
  /** Toggle state → aria-pressed (and the toggle variant's accent look). */
  pressed?: boolean;
  tipWhenDisabled?: boolean;
};

export function IconButton({
  label,
  tip,
  tipSide,
  variant = "toolbar",
  pressed,
  tipWhenDisabled = false,
  className,
  type = "button",
  ...rest
}: IconButtonProps) {
  const button = (
    <button
      type={type}
      aria-label={label}
      aria-pressed={pressed}
      {...rest}
      className={cn(iconButtonClass(variant, pressed), className)}
    />
  );
  const content = tip === undefined ? label : tip;
  return (
    <Tip content={content} side={tipSide}>
      {tipWhenDisabled ? <span className="inline-block">{button}</span> : button}
    </Tip>
  );
}
