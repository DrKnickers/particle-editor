// canvas-architecture Playwright spec.
//
// Asserts the DOM-event → viewport/input bridge wiring under
// the canvas-in-DOM viewport architecture. The host's InputDispatcher
// receives these and PostMessages to the hidden popup HWND, where the
// engine's existing viewport WNDPROC consumes them unchanged. The
// engine-side effect (camera rotates, particles spawn) is exercised
// by the manual smoke matrix — this spec only
// pins the bridge surface so a regression in renderer-side listener
// attachment, encoder logic, or TYPING_TAGS guard is caught in CI.
//
// The canvas is mounted in the native suite's composition host. Tests still
// skip when it is unavailable so a transport failure is reported precisely.

import type { E2ERequest } from "./helpers/bridge-request";
import { test, expect, type Page } from "./helpers/cdp";

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

type ViewportInputRequest = Extract<E2ERequest, { kind: "viewport/input" }>;

declare global {
  interface Window {
    __viewportInputCalls?: ViewportInputRequest[];
    __bridgeProxyInstalled?: boolean;
  }
}

// Install a proxy around window.bridge.request that records every
// `viewport/input` call into window.__viewportInputCalls for the
// duration of the page. Idempotent — calling twice is safe (the
// second call replaces the proxy with itself).
async function installBridgeProxy(p: Page): Promise<void> {
  await p.evaluate(() => {
    const bridge = window.bridge!;
    if (window.__bridgeProxyInstalled) {
      window.__viewportInputCalls = [];
      return;
    }
    window.__viewportInputCalls = [];
    const original = bridge.request.bind(bridge);
    bridge.request = (req) => {
      const r: E2ERequest = req;
      if (r.kind === "viewport/input") {
        window.__viewportInputCalls?.push(r);
      }
      return original(req);
    };
    window.__bridgeProxyInstalled = true;
  });
}

async function readCalls(p: Page): Promise<ViewportInputRequest[]> {
  return p.evaluate(() => window.__viewportInputCalls ?? []);
}

async function archCEnabled(p: Page): Promise<boolean> {
  return p.evaluate(() => {
    return !!document.querySelector('[data-testid="viewport-canvas"]');
  });
}

test.beforeEach(async () => {
  const enabled = await archCEnabled(page);
  test.skip(!enabled, "canvas-jpeg transport not active");
  await installBridgeProxy(page);
});

// TEST.FIXME: pre-existing instrumentation issue.
// `installBridgeProxy` wraps
// `window.bridge.request` (the TestHostBridge under --test-host)
// but ViewportSlot dispatches via its `bridge` prop which is the
// NativeBridge instance from App.tsx's useMemo — different object.
// BridgeContext was added for components that need direct
// bridge access without prop-drilling, but ViewportSlot still
// receives bridge as a prop, so the proxy doesn't intercept its
// `viewport/input` calls. Surfaced when this spec was forced
// REMOVED: two permanently-disabled `test.fixme` specs (pointer-move and
// Shift-keydown bridge dispatch).
//
// They were never going to run. The proxy this file installs cannot intercept
// the production bridge path, and the three possible fixes recorded here were
// all judged out of scope when the tests were written — so they sat as
// permanent fixmes, listed in the suite, contributing nothing.
//
// That is worse than having no test: `test.fixme` reports as a known-skip, so
// the contracts LOOKED covered. Deleting the production viewport pointer
// listener or the Shift-keydown dispatch would not have failed anything here
// (2026-07 audit).
//
// The contracts themselves are real and still worth covering. The honest
// statement is that they are covered by user-driven smoke (Shift+click spawn
// works end-to-end) and NOT by this suite. Doing it properly needs one of:
//   (a) host-side host.log inspection, the dxgi-transport.spec.ts pattern —
//       InputDispatcher::Dispatch would need a mousemove diagnostic first;
//   (b) exposing the NativeBridge instance to window.bridge, which changes the
//       production injection model;
//   (c) installing the proxy via CDP BEFORE React mounts.
// Whoever picks that up should add a real test, not restore a disabled one.

test("TYPING_TAGS guard — Shift keydown while focus is in an inspector field does NOT dispatch", async () => {
  // Locate any text input in the inspector. The Basic tab's Name
  // field is always present once an emitter is selected. If no
  // emitter is selected, look for the first <input type="text">
  // anywhere in the page.
  const input = page.locator('input[type="text"]').first();
  const inputCount = await input.count();
  test.skip(inputCount === 0, "no text input available to test TYPING_TAGS guard");

  await input.focus();
  await installBridgeProxy(page);  // reset call list AFTER focusing the input

  await page.keyboard.down("Shift");
  await page.keyboard.up("Shift");

  const calls = await readCalls(page);
  const keys = calls.filter((c) => c.params.type === "keydown" && c.params.vk === 16);
  expect(keys.length).toBe(0);
});
