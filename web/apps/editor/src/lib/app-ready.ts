// app-ready — the React→host first-paint handshake.
//
// In headless `--capture` mode the host wants to screenshot the whole composed
// window (engine viewport framed by real React chrome). It must not snap before
// React has actually painted, so the app posts this one-shot signal after its
// first meaningful paint and the host gates its composite capture on it.
//
// This is deliberately NOT a typed bridge event: it's a host-lifecycle signal,
// intercepted in HostWindow.cpp's OnWebMessage *before* the dispatcher (the
// dispatcher only handles `type:"req"` envelopes). Going through NativeBridge
// would be wrong — its ctor throws when chrome.webview is absent, and there is
// no app/ready request kind in bridge-schema.

/**
 * The wire `kind` the host matches. The C++ host extracts the `kind` value from
 * the message (via a `"kind":"` needle) and compares it to `app/ready`
 * (HostWindow.cpp OnWebMessage); keep this literal in lockstep with that
 * comparison. The app-ready.test.ts sentinel and the host-side comment guard
 * against silent drift.
 */
export const APP_READY_KIND = "app/ready";

/**
 * Post the first-paint signal to the native host. No-op (and never throws) when
 * `chrome.webview` is unavailable — pure-browser dev and the MockBridge path —
 * via optional chaining. Idempotent: the host just sets a bool, so a duplicate
 * (e.g. React StrictMode double-invoke in a dev bundle) is harmless.
 */
export function signalAppReady(): void {
  window.chrome?.webview?.postMessage?.(JSON.stringify({ kind: APP_READY_KIND }));
}
