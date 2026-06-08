// RandomParam.tsx — mode select + conditional spinner rows.
//
// Mode options: Constant | UniformRange | Normal
// Spinner rows:
//   Constant    → 1 Spinner (value)
//   UniformRange → 2 Spinners (min, max) with "–" separator
//   Normal       → 2 Spinners (mean, σ) with µ / σ letter labels
//
// Single onChange({ mode, ...values }) callback; consumer decides bridge shape.

import * as Select from "@radix-ui/react-select";
import { ChevronDown, ChevronUp, Check } from "lucide-react";
import { Spinner } from "./Spinner";
import type { SpinnerDensity } from "./Spinner";

export type RandomMode = "Constant" | "UniformRange" | "Normal";

export type RandomParamValue =
  | { mode: "Constant"; value: number }
  | { mode: "UniformRange"; min: number; max: number }
  | { mode: "Normal"; mean: number; sigma: number };

export type RandomParamProps = {
  value: RandomParamValue;
  onChange: (value: RandomParamValue) => void;
  step?: number;
  decimals?: number;
  unit?: string;
  density?: SpinnerDensity;
  disabled?: boolean;
};

const MODE_LABELS: Record<RandomMode, string> = {
  Constant: "Constant",
  UniformRange: "Uniform range",
  Normal: "Normal",
};

export function RandomParam({
  value,
  onChange,
  step = 1,
  decimals,
  unit,
  density = "default",
  disabled = false,
}: RandomParamProps) {
  const handleModeChange = (newMode: string) => {
    const mode = newMode as RandomMode;
    // Initialize missing fields with defaults when switching modes.
    switch (mode) {
      case "Constant":
        onChange({ mode: "Constant", value: value.mode === "Constant" ? value.value : 0 });
        break;
      case "UniformRange":
        onChange({
          mode: "UniformRange",
          min: value.mode === "UniformRange" ? value.min : 0,
          max: value.mode === "UniformRange" ? value.max : 1,
        });
        break;
      case "Normal":
        onChange({
          mode: "Normal",
          mean: value.mode === "Normal" ? value.mean : 0,
          sigma: value.mode === "Normal" ? value.sigma : 1,
        });
        break;
    }
  };

  return (
    <div className="flex flex-col gap-1">
      {/* Mode selector */}
      <Select.Root value={value.mode} onValueChange={handleModeChange} disabled={disabled}>
        <Select.Trigger
          className="flex items-center justify-between rounded border border-border-2 bg-bg-2 px-2 py-0.5 text-xs text-text outline-none hover:border-border-2 focus:border-accent disabled:cursor-not-allowed disabled:opacity-40"
          aria-label="Random parameter mode"
        >
          <Select.Value />
          <Select.Icon>
            <ChevronDown className="size-3 text-text-3" />
          </Select.Icon>
        </Select.Trigger>

        <Select.Portal>
          <Select.Content
            position="popper"
            sideOffset={4}
            className="z-50 min-w-[140px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-xl"
          >
            <Select.ScrollUpButton className="flex items-center justify-center py-0.5 text-text-3">
              <ChevronUp className="size-3" />
            </Select.ScrollUpButton>

            <Select.Viewport>
              {(["Constant", "UniformRange", "Normal"] as RandomMode[]).map((mode) => (
                <Select.Item
                  key={mode}
                  value={mode}
                  className="flex cursor-pointer items-center gap-2 rounded px-2 py-1 text-xs text-text outline-none data-[highlighted]:bg-panel-2"
                >
                  <Select.ItemIndicator>
                    <Check className="size-3 text-accent" />
                  </Select.ItemIndicator>
                  <Select.ItemText>{MODE_LABELS[mode]}</Select.ItemText>
                </Select.Item>
              ))}
            </Select.Viewport>

            <Select.ScrollDownButton className="flex items-center justify-center py-0.5 text-text-3">
              <ChevronDown className="size-3" />
            </Select.ScrollDownButton>
          </Select.Content>
        </Select.Portal>
      </Select.Root>

      {/* Spinner rows — conditional on mode */}
      {value.mode === "Constant" && (
        <div className="flex items-center gap-1">
          <Spinner
            value={value.value}
            onChange={(v) => onChange({ mode: "Constant", value: v })}
            step={step}
            decimals={decimals}
            unit={unit}
            density={density}
            disabled={disabled}
            aria-label="Value"
          />
        </div>
      )}

      {value.mode === "UniformRange" && (
        <div className="flex items-center gap-1">
          <Spinner
            value={value.min}
            onChange={(min) => onChange({ mode: "UniformRange", min, max: value.max })}
            step={step}
            decimals={decimals}
            unit={unit}
            density={density}
            disabled={disabled}
            aria-label="Minimum"
          />
          <span className="shrink-0 text-xs text-text-3">–</span>
          <Spinner
            value={value.max}
            onChange={(max) => onChange({ mode: "UniformRange", min: value.min, max })}
            step={step}
            decimals={decimals}
            unit={unit}
            density={density}
            disabled={disabled}
            aria-label="Maximum"
          />
        </div>
      )}

      {value.mode === "Normal" && (
        <div className="flex items-center gap-1">
          <span className="shrink-0 text-xs text-text-2" aria-label="Mean">µ</span>
          <Spinner
            value={value.mean}
            onChange={(mean) => onChange({ mode: "Normal", mean, sigma: value.sigma })}
            step={step}
            decimals={decimals}
            unit={unit}
            density={density}
            disabled={disabled}
            aria-label="Mean"
          />
          <span className="shrink-0 text-xs text-text-2" aria-label="Sigma">σ</span>
          <Spinner
            value={value.sigma}
            onChange={(sigma) => onChange({ mode: "Normal", mean: value.mean, sigma })}
            step={step}
            decimals={decimals}
            unit={unit}
            density={density}
            disabled={disabled}
            aria-label="Sigma"
          />
        </div>
      )}
    </div>
  );
}
