// Composition-mode UIA reachability backbone spec.
//
// What this asserts (positive contract): when the editor is hosted in
// composition mode, the host window's Win32 UI Automation tree exposes
// the expected structural backbone all the way down to the React DOM.
// Concretely:
//   - Root is `AloHostMain` (the registered host-window class).
//   - Contains a `Chrome_WidgetWin_1` descendant (Chromium chrome
//     wrapper is intact — WebView2 is hosted under our window).
//   - Contains an `EmbeddedBrowserFrameView` descendant (the
//     Chromium frame view that wraps the actual web content).
//   - Contains a UIA MenuBar with React menu items (File / Edit /
//     Emitters / Mods / View / Help) — proves Blink's a11y subsystem
//     is awake and walking the React DOM, AND that screen readers
//     can actually reach the React-app menus.
//
// What this does NOT assert: byte-equality with the HWND lane's
// goldens. The two modes' trees diverge for two architectural
// reasons that surfaced during design:
//   (a) HWND mode has an `AloHostViewport` Pane sibling under
//       AloHostMain — the D3D9 viewport's own HWND child. In
//       composition mode the viewport is an IDCompositionVisual with
//       no HWND, so UIA can't see it. Asserting equality would
//       require pretending this HWND-mode-only node doesn't exist
//       (lying about the tree).
//   (b) HWND mode's wrapper chain is deeper than composition mode's,
//       so a fixed-depth capture (depth=20) reaches different amounts
//       of React content. HWND's React subtree stops at the property
//       panel tabs; composition reaches into the `panel-section-
//       header` buttons inside the tabs. The captures aren't
//       comparable in depth without per-mode depth-balancing.
// The hybrid two-lane design (the HWND specs + the composition DOM-
// snapshot specs) provides the actual surface-by-surface regression
// coverage; this spec's job is the narrower one of asserting the
// composition mode's UIA backbone is reachable AT ALL — i.e. catching
// the case where WebView2's Blink a11y regresses back to its earlier
// lazy-init state, leaving composition mode with no UIA
// visibility into the React content.
//
// History: the original Phase 0 probe found composition mode's
// host HWND had "zero UIA descendants" and concluded cross-mode
// equality was infeasible. That was overstated — Phase 0 didn't have
// the enabling changes (--force-renderer-accessibility +
// uia_inspector warmup) that wake up Blink's a11y. With those, the
// React tree IS reachable in composition mode. The hybrid lanes are
// retained as resilience; this spec encodes the positive backbone
// reachability claim.
//
// The composition-specific assertions require the standard native host tree.
//
// If THIS spec ever starts failing — composition mode's host HWND
// stops exposing one of the asserted backbone nodes — READ THIS
// FILE BEFORE updating any assertion. The failure indicates a real
// regression in how WebView2 surfaces accessibility under composition
// hosting, and the cause should be understood before normalizing it
// away.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { captureUIA, discoverHostHwnd } from "./helpers/uia";
import type { UIANode } from "./helpers/a11y-normalizer";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;
let hostHwnd: bigint;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  page = context.pages()[0] ?? (await context.waitForEvent("page"));
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 }
  );
  hostHwnd = await discoverHostHwnd();
});

test.afterAll(async () => {
  // No state mutations to undo — this spec doesn't load a fixture
  // or freeze stats. The host process is killed by run-native-tests
  // .mjs.
});


// Walk the subtree, return the first node matching `pred`, or
// undefined if none.
function findFirst(node: UIANode, pred: (n: UIANode) => boolean): UIANode | undefined {
  if (pred(node)) return node;
  for (const c of node.children ?? []) {
    const found = findFirst(c, pred);
    if (found) return found;
  }
  return undefined;
}

test.describe("a11y/uia-composition-reachable [composition]", () => {
  test("composition mode host HWND exposes React backbone via Win32 UIA", async () => {
    // Depth 20 (matching the HWND lane). Composition mode actually
    // has the React tree deep enough that 12 wasn't reaching the
    // menubar — empirically determined during verification.
    const tree = await captureUIA(hostHwnd, "composition-backbone-check", { depth: 20 });

    // (1) Root is AloHostMain. Defensive sanity — confirms we captured
    // the right HWND (matches the class registered in
    // src/host/HostWindow.cpp:73).
    expect(tree.ClassName).toBe("AloHostMain");

    // (2) Chromium chrome is intact: Chrome_WidgetWin_1 wrapper is
    // present under the host. If WebView2 ever changes hosting so
    // this wrapper goes missing, our other goldens' wrapper-strip
    // assumptions would break too — catching the change here is
    // good early warning.
    expect(
      findFirst(tree, (n) => n.ClassName === "Chrome_WidgetWin_1"),
      "Chrome_WidgetWin_1 wrapper missing under host HWND — WebView2 " +
      "hosting topology may have changed."
    ).toBeDefined();

    // (3) Chromium frame view is present (the wrapper that hosts the
    // actual web content view). If absent, WebView2 is hosting
    // differently than expected.
    expect(
      findFirst(tree, (n) => n.ClassName === "EmbeddedBrowserFrameView"),
      "EmbeddedBrowserFrameView missing — Chromium frame-view layer " +
      "is no longer reachable under the host HWND."
    ).toBeDefined();

    // (4) The React menubar IS reachable, AND it contains the React
    // app's menu items. We identify the React MenuBar by content
    // rather than metadata — the host tree has a Win32 system MenuBar
    // too (AutomationId="MenuBar", Name="System", from the titlebar
    // chrome), and React's Radix MenuBar renders with empty
    // AutomationId + a Tailwind ClassName. The strongest semantic
    // invariant is "a UIA MenuBar exists whose children include at
    // least one of the React-defined menu items." This single check
    // proves Blink's a11y subsystem is awake, the React tree is
    // walked, AND the menubar shell isn't hollow.
    //
    // If this fails, composition-mode users with screen readers
    // would experience no accessibility — actionable regression.
    const expectedMenuItems = ["File", "Edit", "Emitters", "Mods", "View", "Help"];
    const reactMenuBar = findFirst(tree, (n) => {
      if (n.ControlType !== "MenuBar") return false;
      // Discriminator: at least one direct child is one of the
      // React app's menu items. Filters out the Win32 system MenuBar
      // (its child is the "System" menu item only).
      return (n.children ?? []).some(
        (c) =>
          c.ControlType === "MenuItem" &&
          expectedMenuItems.includes(c.Name ?? "")
      );
    });
    expect(
      reactMenuBar,
      `React menubar not reachable from host HWND. Looked for a UIA ` +
      `MenuBar with at least one child MenuItem named one of: ` +
      `${expectedMenuItems.join(", ")}. Either Blink's a11y has ` +
      `regressed to lazy-init under composition hosting (check the ` +
      `--force-renderer-accessibility flag in HostWindow.cpp and the ` +
      `UIA warmup in uia_inspector.cpp), or the React menubar's ` +
      `structure has changed and this assertion needs updating.`
    ).toBeDefined();
  });
});
