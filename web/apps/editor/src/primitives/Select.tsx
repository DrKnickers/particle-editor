// Select — the shared dropdown field, in two builds that share one look:
//
//   <Select value onValueChange options aria-label … />   Radix listbox (the
//       inspector's FieldSelect, the curve editor's Lock-to). Themed popup,
//       per-option test ids.
//   <NativeSelect value onChange aria-label …>{<option>s}</NativeSelect>
//       a styled native <select> for dialog/popover forms whose drivers and
//       specs speak the native control (Playwright selectOption, change events).
//
// Both draw the same trigger: bordered `bg-2` field, chevron at the right,
// hover lifts the fill, keyboard focus gets the canonical ring, disabled =
// opacity-40. The popup chrome comes from primitives/menu.ts.

import * as RadixSelect from "@radix-ui/react-select";
import { ChevronDown } from "lucide-react";
import { type ComponentPropsWithRef } from "react";
import { cn } from "@/lib/utils";
import { SELECT_CONTENT, SELECT_ITEM } from "@/primitives/menu";

const TRIGGER =
  "items-center rounded border border-border-2 bg-bg-2 text-xs text-text transition-colors motion-reduce:transition-none enabled:hover:bg-panel-2 focus-ring disabled:cursor-not-allowed disabled:opacity-40";

/** sm = 24px (toolbar density), md = --row-h (inspector rows). */
const TRIGGER_SIZE = { sm: "h-6", md: "h-[var(--row-h)]" } as const;

export type SelectOption = { value: string; label: string; testId?: string };

type SelectProps = Omit<ComponentPropsWithRef<typeof RadixSelect.Trigger>, "value" | "defaultValue" | "dir"> & {
  value: string;
  onValueChange: (value: string) => void;
  options: readonly SelectOption[];
  placeholder?: string;
  size?: "sm" | "md";
  /** Extra classes for the popup (its min-width). */
  contentClassName?: string;
};

export function Select({
  value,
  onValueChange,
  options,
  disabled,
  placeholder,
  size = "md",
  className,
  contentClassName,
  ...triggerProps
}: SelectProps) {
  const selected = options.find((o) => o.value === value);
  return (
    <RadixSelect.Root value={value} onValueChange={onValueChange} disabled={disabled}>
      <RadixSelect.Trigger
        {...triggerProps}
        className={cn("flex justify-between gap-1 px-2", TRIGGER, TRIGGER_SIZE[size], className)}
      >
        {/* Radix Select.Value strips className, so wrap it in a truncating
            span — white-space:nowrap + overflow-hidden + ellipsis apply to
            the selected-label text through the wrapper. min-w-0 lets this
            flex child shrink so long labels (e.g. "Diffuse transparent")
            ellipsize instead of wrapping onto a second line. */}
        <span className="min-w-0 truncate">
          <RadixSelect.Value placeholder={placeholder}>{selected?.label ?? ""}</RadixSelect.Value>
        </span>
        <RadixSelect.Icon className="shrink-0">
          <ChevronDown className="size-3 text-text-3" />
        </RadixSelect.Icon>
      </RadixSelect.Trigger>
      <RadixSelect.Portal>
        <RadixSelect.Content position="popper" sideOffset={4} className={cn(SELECT_CONTENT, contentClassName)}>
          <RadixSelect.Viewport>
            {options.map((opt) => (
              <RadixSelect.Item key={opt.value} value={opt.value} data-testid={opt.testId} className={SELECT_ITEM}>
                <RadixSelect.ItemText>{opt.label}</RadixSelect.ItemText>
              </RadixSelect.Item>
            ))}
          </RadixSelect.Viewport>
        </RadixSelect.Content>
      </RadixSelect.Portal>
    </RadixSelect.Root>
  );
}

type NativeSelectProps = ComponentPropsWithRef<"select"> & {
  /** Classes for the positioning wrapper (width / margins). */
  wrapperClassName?: string;
};

export function NativeSelect({ className, wrapperClassName, children, ...rest }: NativeSelectProps) {
  return (
    <div className={cn("relative", wrapperClassName)}>
      <select
        {...rest}
        className={cn("h-[var(--row-h-sm)] w-full cursor-pointer appearance-none pl-2 pr-6", TRIGGER, className)}
      >
        {children}
      </select>
      <ChevronDown
        aria-hidden
        className="pointer-events-none absolute right-[7px] top-1/2 size-3 -translate-y-1/2 text-text-3"
      />
    </div>
  );
}
