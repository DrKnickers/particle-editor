// theme.ts — 3-way theme (dark/light/system). `alo:theme` stores the MODE;
// "system" follows prefers-color-scheme live. Resolves to a concrete
// "dark"|"light" applied as <html data-theme>.
import { isRecording } from "./record-mode";

export type ThemeMode = "dark" | "light" | "system";
export type ResolvedTheme = "dark" | "light";

const KEY = "alo:theme";

// Theme-flip cross-fade (design pass): a temporary html.theme-transition class
// scopes a ~150ms color transition to the flip (components.css). 220ms removal
// gives the 150ms transition slack without lingering. Skipped on first apply
// (boot must not fade in from the wrong palette), under --record (frame
// determinism), and under prefers-reduced-motion.
const THEME_TRANSITION_MS = 220;
let themeTransitionTimer: number | undefined;

function beginThemeTransition(root: HTMLElement, next: ResolvedTheme): void {
  const prev = root.dataset.theme;
  if (!prev || prev === next) return;
  if (isRecording()) return;
  if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
  root.classList.add("theme-transition");
  window.clearTimeout(themeTransitionTimer);
  themeTransitionTimer = window.setTimeout(() => {
    root.classList.remove("theme-transition");
  }, THEME_TRANSITION_MS);
}

export function readStoredMode(): ThemeMode {
  const v = localStorage.getItem(KEY);
  return v === "dark" || v === "light" || v === "system" ? v : "system";
}

export function prefersDark(): boolean {
  return window.matchMedia("(prefers-color-scheme: dark)").matches;
}

export function resolveTheme(mode: ThemeMode, osPrefersDark: boolean): ResolvedTheme {
  if (mode === "dark" || mode === "light") return mode;
  return osPrefersDark ? "dark" : "light";
}

export function applyMode(mode: ThemeMode, osPrefersDark = prefersDark()): void {
  const next = resolveTheme(mode, osPrefersDark);
  beginThemeTransition(document.documentElement, next);
  document.documentElement.dataset.theme = next;
  localStorage.setItem(KEY, mode);
}
