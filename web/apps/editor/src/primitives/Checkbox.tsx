// Checkbox — the one checkbox look, on a real <input type="checkbox">.
//
//   <Checkbox checked={on} onChange={(e) => setOn(e.target.checked)} aria-label="Show ground" />
//
// Native on purpose: label association, Space-to-toggle, form semantics, the
// `indeterminate` tri-state (set through a ref, as with any input) and every
// existing test/driver selector keep working unchanged. The look (box, fill,
// check/dash glyph, hover, focus, disabled) is the `.checkbox` class in
// components.css — an appearance:none input, so it is the same in every
// surface and theme.
//
// sizes: "sm" 14px (dialogs, panels, lists) · "md" 18px (inspector rows).
// `data-state` (checked/unchecked) and `data-disabled` mirror Radix's
// vocabulary so selectors and tests written against the old Radix inspector
// checkbox keep resolving.

import { type ComponentPropsWithRef } from "react";
import { cn } from "@/lib/utils";

type CheckboxProps = Omit<ComponentPropsWithRef<"input">, "type" | "size"> & {
  size?: "sm" | "md";
};

export function Checkbox({ size = "sm", className, checked, disabled, ...rest }: CheckboxProps) {
  return (
    <input
      type="checkbox"
      checked={checked}
      disabled={disabled}
      data-state={checked === undefined ? undefined : checked ? "checked" : "unchecked"}
      data-disabled={disabled ? "" : undefined}
      {...rest}
      className={cn("checkbox", size === "md" && "checkbox-md", className)}
    />
  );
}
