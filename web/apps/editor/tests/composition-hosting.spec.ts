// Composition-hosting A/B parity guard.
//
// The composition-hosting work swapped WebView2 from HWND-mode hosting
// (CreateCoreWebView2Controller) to composition hosting
// (CreateCoreWebView2CompositionController), gated on the env-var
// pair `ALO_HOSTING_MODE != legacy (default)` + `ALO_VIEWPORT_TRANSPORT=
// canvas-jpeg`. Under composition the host HWND owns Win32 focus +
// input; mouse/keyboard reach WebView2 only through host-side
// forwarding (SendMouseInput, MoveFocus). The 96-baseline native
// suite runs under either hosting mode and proves the bridge layer
// is identical — but those specs don't EXPLICITLY assert "this
// composition-mode gesture must still work." This spec does.
//
// IMPORTANT CAVEAT — what these specs do and DON'T test.
//
// Playwright's `.click()`, `.keyboard.press()`, `.fill()`, etc.
// dispatch synthetic events through CDP at the Chromium renderer
// level — they bypass the OS WM_*-message path entirely. So none
// of the assertions here validate the host's SendMouseInput /
// SendKeyboardInput / MoveFocus forwarding code directly. They
// validate that the BRIDGE layer (WebView2 controller wiring,
// host-object proxy, postMessage round-trips, React event handling)
// works identically under composition mode to HWND mode.
//
// The OS-input-path coverage is irreducible to manual smoke —
// Playwright can't
// dispatch a real WM_LBUTTONDOWN that goes through the OS focus
// chain into the host's MainWndProc. The host-side correctness
// of mouse forwarding, cursor sync, DPI, and
// keyboard focus transfer all depend on smoke evidence
// outside this file.
//
// What these specs DO catch:
//   - Regression in OnCompositionControllerReady wiring (e.g.
//     accidentally breaking the QI to ICoreWebView2Controller
//     would cause every spec to fail loading)
//   - Regression in FinishWebView2ControllerSetup factoring
//     (e.g. if the refactor dropped a wire,
//     these specs would fail because the bridge handler isn't
//     registered)
//   - Regression in Compositor::AttachWebView2 (e.g. tree commit
//     fails -> no React rendering -> every spec times out)
//
// Skip behaviour: each test no-ops with a clear message when
// ALO_HOSTING_MODE == "legacy" (composition mode inactive). Running the harness
// without the env var (HWND-mode baseline) silently skips this
// file; running WITH it gates the composition path.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";
const COMPOSITION_MODE = process.env.ALO_HOSTING_MODE !== "legacy";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));

  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test.beforeEach(({}, testInfo) => {
  if (!COMPOSITION_MODE) {
    testInfo.annotations.push({
      type: "skip-reason",
      description:
        "ALO_HOSTING_MODE == 'legacy' (composition mode inactive) — composition-mode gate not " +
        "applicable to this run. Set ALO_HOSTING_MODE != legacy (default) to enable.",
    });
    test.skip();
  }
});

test("env-var pair signalling composition mode is set in process env", () => {
  // Sanity check: the test process inherits env from PowerShell where
  // the user (or run-native-tests harness) set the pair. The host
  // process inherits from the test process via spawn() without env
  // override (run-native-tests.mjs:49 spawns with default env).
  // If the host log shows the composition path actually ran, this
  // env-var sighting is the cause.
  expect(process.env.ALO_HOSTING_MODE).not.toBe("legacy"); // default = composition
  // ALO_VIEWPORT_TRANSPORT retired; canvas-jpeg path is now coupled to ALO_HOSTING_MODE
});

test("click on Background toolbar dropdown opens the popover (click routing under composition)", async () => {
  // Mirrors the existing tools.spec.ts:166 test but explicitly gated
  // on composition mode. If composition-controller wiring regresses
  // (e.g. RootVisualTarget binding fails silently), this would fail
  // because React's onClick wouldn't fire.
  await page.keyboard.press("Escape").catch(() => {});
  await page.locator('button[aria-label="Background"]').first().click();
  await page.waitForSelector("[data-radix-popper-content-wrapper]", {
    timeout: 2000,
  });
  // Cleanup.
  await page.keyboard.press("Escape");
});

test("click coords land at the expected DOM element under composition", async () => {
  // The host forwards WM_LBUTTONDOWN coords via lParam -> POINT ->
  // SendMouseInput. CDP click bypasses this path, but if the React
  // tree is being rendered through the composition surface correctly,
  // clicking a button at its DOM rect should hit the button (no
  // off-by-N-pixels translation).
  const trigger = page.locator('button[aria-label="Background"]').first();
  const box = await trigger.boundingBox();
  expect(box).not.toBeNull();
  if (!box) return;

  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
  await page.waitForSelector("[data-radix-popper-content-wrapper]", {
    timeout: 2000,
  });
  await page.keyboard.press("Escape");
});

// CameraDto's Vec3 is a TUPLE on the wire ([x, y, z]), not {x,y,z}.
type Vec3Tuple = readonly [number, number, number];
type CameraSnapshot = { position: Vec3Tuple; target: Vec3Tuple };

// Distance from the camera eye to its target — the scalar a zoom moves.
function camDistance(cam: CameraSnapshot): number {
  const dx = cam.position[0] - cam.target[0];
  const dy = cam.position[1] - cam.target[1];
  const dz = cam.position[2] - cam.target[2];
  return Math.sqrt(dx * dx + dy * dy + dz * dz);
}

test("wheel over the viewport canvas zooms the engine camera under composition", async () => {
  // What this guards, exactly: ViewportSlot.tsx registers
  //   canvas.addEventListener("wheel", onWheel, { passive: false })
  // and onWheel does two independent things — forwards a `viewport/input`
  // wheel event so the engine zooms, and calls preventDefault() so the
  // surrounding scroll container stays put. This test covers the FORWARDING
  // half; the sibling below covers preventDefault. Split deliberately: a
  // handler that does one and not the other is a real regression, and a single
  // "something happened" assertion passes both broken shapes.
  //
  // Uses page.mouse.wheel — CDP dispatches at the renderer's real input path,
  // so this exercises listener registration and hit-testing on the canvas, not
  // just the callback body.
  //
  // Supersedes a test that named the CURVE editor's wheel handler (there is no
  // wheel listener in CurveEditorPanel), asserted curve-layer counts rather
  // than wheel behaviour, and self-skipped on every machine because it looked
  // for an SVG that only mounts when an emitter is SELECTED while its setup
  // only tried to ADD one — via `emitters/add`, which is not a bridge kind
  // (2026-07 audit adjudication).

  const readCamera = () =>
    page.evaluate(async () => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const b = (window as any).bridge;
      const s = await b.request({ kind: "engine/state/snapshot", params: {} });
      return s.camera as CameraSnapshot;
    });

  const before = await readCamera();

  const canvas = page.locator("[data-testid='viewport-canvas']");
  await expect(canvas).toBeVisible();
  const box = await canvas.boundingBox();
  expect(box, "viewport canvas must have a layout box to aim the wheel at").not.toBeNull();

  await page.mouse.move(box!.x + box!.width / 2, box!.y + box!.height / 2);
  // DOM deltaY is opposite-sign from WHEEL_DELTA: negative here is one notch
  // AWAY from the user, which the renderer normalises to +120 = zoom IN.
  await page.mouse.wheel(0, -120);
  await page.waitForTimeout(150);

  const after = await readCamera();

  // Direction, not just change. A handler wired to the wrong sign still moves
  // the camera, and "distance !== before" would bless it.
  expect(camDistance(after)).toBeLessThan(camDistance(before));

  // Restore, so later cases in this file inherit the camera they expected
  // (2026-07 audit — shared engine state must not leak between cases).
  await page.evaluate(async (cam) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/camera", params: cam });
  }, before as unknown as Record<string, unknown>);
});

test("wheel over the viewport canvas is preventDefault'd so the parent does not scroll", async () => {
  // The "without parent scroll" half of the contract. Asserting a parent's
  // scrollTop is unreliable (it needs an overflowing container that actually
  // could scroll), so this asserts the mechanism that prevents it:
  // defaultPrevented on a cancelable wheel event delivered to the canvas.
  //
  // A listener registered { passive: true } — the plausible regression, since
  // passive is the browser default for wheel on many targets — silently drops
  // preventDefault and fails here while leaving the zoom test above green.
  const prevented = await page.evaluate(() => {
    const canvas = document.querySelector("[data-testid='viewport-canvas']");
    if (!canvas) return null;
    const ev = new WheelEvent("wheel", {
      deltaY: -120,
      bubbles: true,
      cancelable: true,
    });
    canvas.dispatchEvent(ev);
    return ev.defaultPrevented;
  });

  expect(prevented, "viewport canvas not found in the DOM").not.toBeNull();
  expect(prevented).toBe(true);
});

test("modifier keys round-trip via React event system under composition", async () => {
  // Shift-modified click — verifies that pointerdown's shiftKey
  // propagates through WebView2 to React under composition. CDP's
  // synthetic click with modifiers is sent via locator.click's
  // `modifiers` option (page.mouse.click() doesn't take modifiers;
  // its lowlevel API requires manual keyboard.down/up wrapping).
  // Even though this bypasses the host's wParam-MK_SHIFT-to-
  // VIRTUAL_KEYS_SHIFT translation in
  // ForwardMouseToCompositionWebView2, it validates the React chain
  // DID receive the modifier — paired with the manual smoke,
  // this proves the full path works.
  await page.keyboard.press("Escape").catch(() => {});

  // Capture the most recent click event's shiftKey value.
  await page.evaluate(() => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const w = window as any;
    w.__lastClickShift = null;
    document.addEventListener(
      "click",
      (e) => {
        w.__lastClickShift = e.shiftKey;
      },
      { capture: true, once: true },
    );
  });

  const trigger = page.locator('button[aria-label="Background"]').first();
  await trigger.click({ modifiers: ["Shift"] });

  const observedShift = await page.evaluate(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    () => (window as any).__lastClickShift,
  );
  expect(observedShift).toBe(true);
  await page.keyboard.press("Escape").catch(() => {});
});

test("bridge round-trip preserved under composition (engine/set/bloom snapshot)", async () => {
  // A bridge mutation + snapshot is the cleanest end-to-end
  // verification that composition-mode hosting hasn't accidentally
  // broken postMessage / TestHostBridge wiring. Mirrors the
  // tools.spec.ts:118 pattern but as a focused composition gate
  // rather than a Bloom-panel-UI-flow test.
  const before = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.bloom as boolean;
  });

  await page.evaluate(async (orig) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/bloom", params: { enabled: !orig } });
  }, before);

  await page.waitForTimeout(150);

  const after = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.bloom as boolean;
  });
  expect(after).toBe(!before);

  // Restore.
  await page.evaluate(async (orig) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/bloom", params: { enabled: orig } });
  }, before);
});

test("keyboard input via CDP reaches focused React input under composition", async () => {
  // Caveat: page.keyboard.press / page.keyboard.type dispatch synthetic
  // KeyboardEvents through CDP at the Chromium renderer level — they
  // bypass the OS WM_KEY*-via-MoveFocus path entirely. This test
  // therefore does NOT validate the host's MoveFocus call from
  // OnCompositionControllerReady or the WM_SETFOCUS routing in
  // MainWndProc. It validates the simpler claim: the React event
  // system inside WebView2 still receives and dispatches keyboard
  // events normally under composition mode (no DOM-level breakage).
  // The real keyboard-focus assertion is in the manual smoke.
  //
  // Open the Lighting pane so we have a known input to focus. Bloom's
  // controls now live as a collapsible section inside Lighting,
  // so expand that section before reaching the Enable bloom checkbox.
  await page.keyboard.press("Escape").catch(() => {});
  const trigger = page.locator('[role="menubar"] >> text=View').first();
  await trigger.click();
  await page.waitForSelector('[role="menu"]', { timeout: 2000 });
  const lightingItem = page
    .locator('[role="menuitem"]:has-text("Lighting")')
    .first();
  if ((await lightingItem.count()) === 0) {
    test.skip(true, "View → Lighting menu item not present in this build");
    return;
  }
  await lightingItem.click();
  await page.waitForSelector('[role="dialog"][aria-label="Lighting"]', {
    timeout: 2000,
  });
  // Expand the collapsible Bloom section so its checkbox is actionable.
  // (Section header is a controlled div[role="button"] since the
  // <details>→controlled conversion that enabled the collapse animation.)
  await page
    .locator('[role="dialog"][aria-label="Lighting"] [role="button"]:has-text("Bloom")')
    .first()
    .click();

  // Find the Enable bloom checkbox; toggling via keyboard (Space)
  // should fire engine/set/bloom + the snapshot should reflect it.
  const before = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.bloom as boolean;
  });

  const checkbox = page.locator('input[aria-label="Enable bloom"]').first();
  await checkbox.focus();
  await page.keyboard.press("Space");
  await page.waitForTimeout(200);

  const after = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.bloom as boolean;
  });
  expect(after).toBe(!before);

  // Restore + close.
  await page.evaluate(async (orig) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/bloom", params: { enabled: orig } });
  }, before);
  const closeBtn = page
    .locator('[role="dialog"][aria-label="Lighting"] button[aria-label="Close"]')
    .first();
  if (await closeBtn.count()) {
    await closeBtn.click();
  }
});

test("composition mode does not break the test-host bridge proxy (postMessage regression)", async () => {
  // WebView2 drops postMessage under CDP
  // attach — the test-host channel uses AddHostObjectToScript
  // instead. Under composition mode, the host-object channel is
  // still on ICoreWebView2 (accessible via get_CoreWebView2 on the
  // QI'd base controller), so it should work unchanged. If
  // composition-mode controller setup accidentally skipped the
  // AddHostObjectToScript call (e.g. FinishWebView2ControllerSetup
  // factoring regressed), every bridge.request() call from CDP
  // would error.
  //
  // The simplest assertion: a basic request returns a structured
  // response, not an error. We already do bridge.request() above;
  // this test just makes the contract explicit + named.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    return await b.request({ kind: "engine/state/snapshot", params: {} });
  });
  expect(result).toBeDefined();
  expect(typeof result).toBe("object");
  expect(result).toHaveProperty("bloom");
  expect(result).toHaveProperty("groundTexture");
});
