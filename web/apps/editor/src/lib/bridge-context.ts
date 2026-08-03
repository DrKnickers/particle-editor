// BridgeContext — share the live Bridge instance across the React tree
// without prop-drilling. Consumed by primitives that need to talk to
// the host but sit deep in the tree with many uninvolved layers above
// them (the Modal primitive is the canonical case — 9+ callers that
// would all need a `bridge` prop otherwise).
//
// Why not `window.bridge`? `exposeBridgeForTests` in `bridge/expose.ts`
// swaps `window.bridge` to a `TestHostBridge` whenever
// `chrome.webview.hostObjects.hostBridge` is truthy. WebView2 returns
// a proxy for that property access even when no host object is
// registered, so in non-`--test-host` runs the swap fires anyway and
// every `window.bridge` call hits `dispatchRequest` on an unregistered
// host object, rejecting with HRESULT 0x80070490 ("Element not found").
// React components that receive `bridge` by prop avoid this because
// `App.tsx` holds the original `NativeBridge` reference in its useMemo
// closure. Context preserves the same closure semantics for consumers
// without explicit prop threading.

import { createContext, useContext } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";

export const BridgeContext = createContext<Bridge | null>(null);

/** Read the current Bridge from React context. Returns null when used
 *  outside the App's `<BridgeContext.Provider>` (the case for unit
 *  tests that mount components in isolation). Callers should treat
 *  the result like an optional dependency — if null, skip any host
 *  call rather than throw. */
export function useBridge(): Bridge | null {
  return useContext(BridgeContext);
}
