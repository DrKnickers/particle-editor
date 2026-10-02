import { type ClassValue, clsx } from "clsx";
import { extendTailwindMerge } from "tailwind-merge";

// The small-type tokens (`text-2xs`/`3xs`/`4xs`, styles/tokens.css) are not in
// tailwind-merge's default scale; unregistered, it would read them as text
// COLOURS and drop them whenever a later `text-<colour>` class is merged.
const twMerge = extendTailwindMerge({
  extend: { classGroups: { "font-size": [{ text: ["2xs", "3xs", "4xs"] }] } },
});

export function cn(...inputs: ClassValue[]) {
  return twMerge(clsx(inputs));
}

export function clamp01(value: number): number {
  return Math.max(0, Math.min(1, value));
}
