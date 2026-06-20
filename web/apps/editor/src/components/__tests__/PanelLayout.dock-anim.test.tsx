// [Item 3] PanelLayout dock-slide host-anim contract tests.
//
// These render the FULL PanelLayout to exercise the real toggle effect, but
// MOCK the heavy child screens to trivial stubs. The children each do an
// on-mount bridge fetch that, under a stub bridge, resolves to {} and crashes
// its render once an `await` flushes the microtask queue (EmitterTree reads
// tree.root, SpawnerPanel reads config.mode, …). The dock effect under test
// needs none of them — it reads PanelLayout's own quadrant-viewport div and the
// dock panel ref — so stubbing the children isolates the behaviour. Kept in a
// separate file from PanelLayout.test.tsx so those synchronous DOM-structure
// tests stay mock-free.
//
// What we lock here is the WEB CONTRACT only; the actual smooth interpolation
// is host-side and verified in the real editor ():
//   - the suppression-signal lifecycle (raised on a dock slide, cleared at
//     the settle, and NOT stranded by a rapid re-toggle).
//   - reduced-motion: the panel snaps (no animate-scene-rect).
//
// Coverage note: every toggle below is close→open, so only the OPEN direction's
// integration is exercised. jsdom never lays out, so getSize().inPixels is 0 →
// a CLOSE always takes the no-animate branch and the remembered-width capture
// can't fire. The CLOSE/grow direction and the analytic `to` math ARE locked,
// just in scene-rect.test.ts (dockSlideTarget open-shrink / close-grow / clamp);
// the close-direction effect WIRING is left to host-side verification (),
// where real geometry exists.
import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { act, render, waitFor } from "@testing-library/react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { BridgeContext } from "@/lib/bridge-context";
import { __resetRightDockForTests, setDock } from "@/lib/right-dock";
import { useDockAnim } from "@/lib/dock-anim";

vi.mock("@/screens/EmitterTree", () => ({ EmitterTree: () => <div /> }));
vi.mock("@/screens/EmitterPropertyTabs", () => ({ EmitterPropertyTabs: () => <div /> }));
vi.mock("@/screens/SpawnerPanel", () => ({ SpawnerPanel: () => <div /> }));
vi.mock("@/screens/LightingPanel", () => ({ LightingPanel: () => <div /> }));
vi.mock("../CurveEditorPanel", () => ({ CurveEditorPanel: () => <div /> }));
vi.mock("../ViewportSlot", () => ({ ViewportSlot: () => <div /> }));

// eslint-disable-next-line import/first
import { PanelLayout } from "../PanelLayout";

describe("PanelLayout — dock-slide animation (Item 3)", () => {
  beforeEach(() => {
    localStorage.clear();
    __resetRightDockForTests();
    useDockAnim.setState({ animating: false });
  });
  afterEach(() => {
    useDockAnim.setState({ animating: false });
  });

  function makeSpyBridge(): Bridge & { request: ReturnType<typeof vi.fn> } {
    const request = vi.fn().mockResolvedValue({});
    return { request, on: () => () => {} } as unknown as Bridge & {
      request: ReturnType<typeof vi.fn>;
    };
  }
  function renderLayout(bridge: Bridge) {
    return render(
      <BridgeContext.Provider value={bridge}>
        <PanelLayout bridge={bridge} />
      </BridgeContext.Provider>,
    );
  }
  function animateCalls(bridge: { request: ReturnType<typeof vi.fn> }) {
    return bridge.request.mock.calls.filter((c) => c[0]?.kind === "animate-scene-rect");
  }
  // Let one rAF fire (the toggle defers expand/collapse one frame) so the panel
  // reaches a real collapsed state before the next toggle.
  async function flushRaf() {
    await act(async () => {
      await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));
    });
  }

  it("opening a previously-closed dock raises the suppression signal, then clears it", async () => {
    const bridge = makeSpyBridge();
    renderLayout(bridge); // open at mount
    await act(async () => {
      setDock(null); // close
    });
    await flushRaf(); // p.collapse() runs → panel is really collapsed
    act(() => {
      setDock("spawner"); // open
    });
    // The signal is raised synchronously in the toggle effect (before the rAF
    // that posts animate-scene-rect), so it is observable immediately.
    expect(useDockAnim.getState().animating).toBe(true);
    // …and lowered at the 260ms authoritative-settle timer.
    await waitFor(() => expect(useDockAnim.getState().animating).toBe(false), {
      timeout: 2000,
    });
  });

  it("opening sends EXACTLY ONE animate-scene-rect with the new contract shape", async () => {
    const bridge = makeSpyBridge();
    renderLayout(bridge);
    await act(async () => {
      setDock(null);
    });
    await flushRaf();
    const before = animateCalls(bridge).length; // close side sends none in jsdom
    await act(async () => {
      setDock("spawner"); // open — the animated path
    });
    await flushRaf(); // the open's rAF posts animate-scene-rect
    // ONE-SHOT contract: exactly one send for the open (a double-send must fail).
    await waitFor(() => expect(animateCalls(bridge).length).toBe(before + 1));
    const params = animateCalls(bridge)[before][0].params;
    expect(params).toMatchObject({
      from: { x: expect.any(Number), y: expect.any(Number), w: expect.any(Number), h: expect.any(Number) },
      to: { x: expect.any(Number), y: expect.any(Number), w: expect.any(Number), h: expect.any(Number) },
      durationMs: 200,
      easing: "ease",
      msElapsedAtSend: expect.any(Number),
    });
  });

  it("a rapid re-toggle (superseded before its settle) does NOT strand the suppression signal", async () => {
    // Regression for the cross-component-signal leak: the suppression signal's
    // only happy-path clear is the 260ms settle timer, which a re-toggle's
    // effect cleanup cancels. If the superseding run then early-returns at the
    // `need` guard, the signal must STILL be cleared (by the cleanup) — else
    // ViewportSlot's ResizeObserver is silenced indefinitely.
    const bridge = makeSpyBridge();
    renderLayout(bridge); // open at mount
    await act(async () => {
      setDock(null); // close
    });
    await flushRaf(); // panel really collapses
    await act(async () => {
      setDock("spawner"); // OPEN A — animates, raises the signal
    });
    expect(useDockAnim.getState().animating).toBe(true); // non-vacuous: A raised it
    await act(async () => {
      setDock(null); // CLOSE B supersedes A before A's rAF/settle fire
    });
    // The superseding cleanup must drop the stranded signal.
    await waitFor(() => expect(useDockAnim.getState().animating).toBe(false), {
      timeout: 2000,
    });
  });

  it("under reduced-motion: opening sends NO animate-scene-rect (panel snaps)", async () => {
    const realMM = window.matchMedia;
    Object.defineProperty(window, "matchMedia", {
      configurable: true,
      value: (q: string) => ({
        matches: q.includes("reduce"),
        media: q,
        onchange: null,
        addEventListener() {},
        removeEventListener() {},
        addListener() {},
        removeListener() {},
        dispatchEvent() {
          return false;
        },
      }),
    });
    try {
      const bridge = makeSpyBridge();
      renderLayout(bridge);
      await act(async () => {
        setDock(null); // close
      });
      await flushRaf();
      const before = animateCalls(bridge).length;
      await act(async () => {
        setDock("spawner"); // open — reduced-motion → snap, no host anim
      });
      await flushRaf();
      expect(animateCalls(bridge).length).toBe(before);
      expect(useDockAnim.getState().animating).toBe(false);
    } finally {
      Object.defineProperty(window, "matchMedia", { configurable: true, value: realMM });
    }
  });
});
