// theme.ts — 3-way theme (dark/light/system). `alo:theme` stores the MODE;
// "system" follows prefers-color-scheme live. Resolves to a concrete
// "dark"|"light" applied as <html data-theme>.
export type ThemeMode = "dark" | "light" | "system";
export type ResolvedTheme = "dark" | "light";

const KEY = "alo:theme";

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
  document.documentElement.dataset.theme = resolveTheme(mode, osPrefersDark);
  localStorage.setItem(KEY, mode);
}
