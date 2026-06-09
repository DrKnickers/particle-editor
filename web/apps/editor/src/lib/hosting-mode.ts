// hosting-mode.ts — the build/runtime hosting-mode check, shared by the
// components that gate behaviour on architecture A (legacy) vs C (default).
//
// Originally local to ViewportSlot (); promoted to a shared lib when
// PanelLayout needed the same check to gate the Item-3 dock-slide animation
// (the host anim path is only, so the web side must not suppress
// per-frame scene-rects nor emit `animate-scene-rect` under --legacy — else
// the legacy viewport freezes-then-snaps during a dock slide).
//
// Default (architecture C): DXGI composition + DComp engine visual under the
// WebView2 visual. Opt out with VITE_HOSTING_MODE=legacy (architecture A:
// AlphaCompositor popup + JPEG-into-<img>). Mirrors the runtime
// ALO_HOSTING_MODE check in HostWindow.cpp; a build/runtime mismatch triggers
// the boot-time consistency banner (App.tsx mode-claim).
//
// Read the flag inside a function (not a module-level const) so vitest can
// override the env var per-test via vi.stubEnv() without vi.resetModules()
// chains. Check BOTH import.meta.env (Vite bakes the build-time value here in
// production) and process.env (vi.stubEnv writes here in vitest's node runtime).
export function isLegacyMode(): boolean {
  const fromImportMeta = (import.meta as { env?: Record<string, unknown> }).env?.VITE_HOSTING_MODE;
  const fromProcess = typeof process !== "undefined" && process.env
    ? process.env.VITE_HOSTING_MODE
    : undefined;
  return fromImportMeta === "legacy" || fromProcess === "legacy";
}
