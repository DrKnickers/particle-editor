// SegmentedControl — a small exclusive choice drawn as a pill of segments
// (Theme: Dark / Light / System; Battle context: Space / Land; Texture slot
// filter: Color / Bump).
//
//   <SegmentedControl aria-label="Theme" value={mode} onValueChange={choose}
//     options={[{ value: "dark", label: "Dark" }, …]} />
//
// Semantics are the WAI-ARIA radio group: role="radiogroup" with
// role="radio" segments, ONE Tab stop (the checked segment), and the arrow
// keys (+ Home/End) move AND select, like native radios. The checked
// segment is the roving stop by construction, so focus can never land on a
// segment that isn't the current value.

import { useRef, type KeyboardEvent, type ReactNode } from "react";
import { cn } from "@/lib/utils";

export type SegmentedOption<T extends string> = { value: T; label: ReactNode; testId?: string };

type SegmentedControlProps<T extends string> = {
  value: T;
  onValueChange: (value: T) => void;
  options: readonly SegmentedOption<T>[];
  "aria-label": string;
  className?: string;
};

export function SegmentedControl<T extends string>({
  value,
  onValueChange,
  options,
  "aria-label": ariaLabel,
  className,
}: SegmentedControlProps<T>) {
  const refs = useRef<(HTMLButtonElement | null)[]>([]);
  const current = options.findIndex((o) => o.value === value);

  const onKeyDown = (e: KeyboardEvent) => {
    let next: number;
    switch (e.key) {
      case "ArrowRight":
      case "ArrowDown": next = (current + 1) % options.length; break;
      case "ArrowLeft":
      case "ArrowUp": next = (current - 1 + options.length) % options.length; break;
      case "Home": next = 0; break;
      case "End": next = options.length - 1; break;
      default: return;
    }
    e.preventDefault();
    onValueChange(options[next]!.value);
    refs.current[next]?.focus();
  };

  return (
    <div
      role="radiogroup"
      aria-label={ariaLabel}
      className={cn("inline-flex gap-0.5 rounded-[var(--radius-sm)] border border-border-2 bg-bg-2 p-0.5", className)}
    >
      {options.map((o, i) => {
        const checked = o.value === value;
        return (
          <button
            key={o.value}
            ref={(el) => { refs.current[i] = el; }}
            type="button"
            role="radio"
            aria-checked={checked}
            // No checked segment (value outside the options) → the first one
            // is the stop, so the group stays reachable.
            tabIndex={checked || (current < 0 && i === 0) ? 0 : -1}
            data-testid={o.testId}
            onClick={() => onValueChange(o.value)}
            onKeyDown={onKeyDown}
            className={cn(
              "rounded-[var(--radius-xs)] px-[11px] py-[3px] text-2xs transition-colors motion-reduce:transition-none focus-ring-inset",
              checked ? "bg-accent-soft font-semibold text-accent" : "text-text-3 hover:text-text-2",
            )}
          >
            {o.label}
          </button>
        );
      })}
    </div>
  );
}
