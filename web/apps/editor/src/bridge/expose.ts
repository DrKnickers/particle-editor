// Expose the bridge instance to `window.bridge` so external tooling
// (DevTools console under --test-host, Playwright contract specs) can
// issue requests without going through React's component tree.
//
// In normal launches `window.bridge` is the production bridge
// (NativeBridge in WebView2, MockBridge in pure-browser dev). It's a
// diagnostic surface only — no production code path reads it.
//
// In --test-host mode, the C++ host registers a COM IDispatch object
// under `chrome.webview.hostObjects.hostBridge` via
// `ICoreWebView2::AddHostObjectToScript`. When present, we swap
// `window.bridge` for a TestHostBridge that routes requests through
// that host-object channel instead of `chrome.webview.postMessage`.
//
// Why: WebView2 silently drops postMessage calls from the page → host
// while a CDP debugger is attached (verified empirically). The host-object
// IPC channel is unaffected,
// so Playwright contract specs use the swapped bridge for
// request/response. Events (engine/state/changed etc.) still flow over
// the host → page postMessage direction, which is unaffected by the
// drop; TestHostBridge subscribes to those normally.

import type { Bridge } from "@particle-editor/bridge-schema";
import { TestHostBridge } from "./test-host";

declare global {
  interface Window {
    bridge?: Bridge;
  }
}

export function exposeBridgeForTests(bridge: Bridge): void {
  // If the test-host channel is present, prefer it — the schema-
  // contract Playwright specs need it for request/response (postMessage
  // is dropped under CDP).
  const hostObj = window.chrome?.webview?.hostObjects?.hostBridge;
  if (hostObj) {
    (window as { bridge?: Bridge }).bridge = new TestHostBridge();
    return;
  }
  (window as { bridge?: Bridge }).bridge = bridge;
}
