// Centralized hosting-mode predicate for native Playwright
// specs + vitest unit tests. Reads ALO_HOSTING_MODE (the env var the
// test harness `run-native-tests.mjs --legacy` sets) and returns the
// active mode.
//
// Default (unset / "composition" / anything but "legacy") → architecture
// C (DXGI composition + DComp engine visual + WebView2 composition
// hosting). Set to "legacy" → architecture A (AlphaCompositor popup +
// HWND-hosted WebView2 + JPEG decode into <img>).
//
// Use this helper in any spec that gates on hosting mode rather than
// reading process.env directly — keeps the predicate consistent and
// makes future mode-name refactors a single-file change.

export function isLegacyMode(): boolean {
  return process.env.ALO_HOSTING_MODE === "legacy";
}

export function isCompositionMode(): boolean {
  return !isLegacyMode();
}
