// Button — the shared text button: Modal's OK / Cancel, and the few action
// buttons that live outside a dialog (ErrorBoundary's Reload, the Spawner's
// Spawn now).
//
//   <Button variant="primary" onClick={…}>Apply</Button>
//
// primary / danger fill with the AA-safe *-strong tokens behind white text;
// secondary is the bordered neutral (Cancel). Disabled = opacity-40.

import { type ComponentPropsWithRef } from "react";
import { cn } from "@/lib/utils";

export type ButtonVariant = "primary" | "danger" | "secondary";

const VARIANT_CLASS: Record<ButtonVariant, string> = {
  primary: "bg-accent-strong font-medium text-white hover:bg-accent-strong-hover",
  danger: "bg-danger-strong font-medium text-white hover:bg-danger-strong-hover",
  secondary: "border border-border-2 bg-panel-2 text-text hover:bg-panel-3",
};

export function buttonClass(variant: ButtonVariant = "primary"): string {
  return cn(
    "rounded px-3 py-1 text-xs transition-colors motion-reduce:transition-none focus-ring disabled:cursor-not-allowed disabled:opacity-40",
    VARIANT_CLASS[variant],
  );
}

type ButtonProps = ComponentPropsWithRef<"button"> & { variant?: ButtonVariant };

export function Button({ variant = "primary", className, type = "button", ...rest }: ButtonProps) {
  return <button type={type} {...rest} className={cn(buttonClass(variant), className)} />;
}
