import { describe, it, expect, beforeEach } from "vitest";
import { StrictMode, useEffect } from "react";
import { renderHook, act, render } from "@testing-library/react";
import { useAtlasAutoOpen } from "../use-atlas-autoopen";
import { publishAtlasContext, __resetAtlasContext, type AtlasContext } from "../atlas-context";
import { setDock, __resetRightDockForTests, useRightDockStoreForTests } from "../right-dock";

const dock = () => useRightDockStoreForTests().getState().dock;
beforeEach(() => { localStorage.clear(); __resetRightDockForTests(); __resetAtlasContext(); setDock("lighting"); renders = 0; });

// A render-counting host for the mount site — mirrors how CurveEditorPanel
// mounts the hook. `renders` counts commits of the host so we can prove that a
// context publish does NOT re-render the component (the self-feedback fix).
let renders = 0;
function CountingHost({ atlasEligible }: { atlasEligible: boolean }) {
  renders += 1;
  useAtlasAutoOpen({ atlasEligible });
  return null;
}

describe("useAtlasAutoOpen", () => {
  it("auto-opens on first index key selection; restores lighting on channel leave", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "red", interpolation: "step", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("lighting");
  });
  it("a manual dock change while auto-open cancels the restore", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    act(() => setDock("spawner"));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "red", interpolation: "step", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("spawner"); // NOT restored to lighting
  });
  it("does not auto-open when not eligible", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: false }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("lighting");
  });
  it("restores on emitter cleared", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    act(() => publishAtlasContext({ emitterId: null, focusedTrack: "index", interpolation: null, selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("lighting");
  });
  it("emitter→emitter switch (still on index) is a smooth swap, not a restore", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    // switch to a DIFFERENT non-null emitter, still on index, fresh selection
    act(() => publishAtlasContext({ emitterId: 2, focusedTrack: "index", interpolation: "step", selection: { frame: 7, keyTimes: [0.5] } }));
    expect(dock()).toBe("atlas"); // stayed — NOT restored to "lighting"
  });
});

// --- Non-reactive observation fix (#549 audit follow-up). The hook now reads
// the atlas context via a NON-reactive subscription so CurveEditorPanel's own
// publish no longer re-renders it. These tests lock in that fix and the
// equivalence subtleties the plan review surfaced. ---
describe("useAtlasAutoOpen — non-reactive context observation", () => {
  it("a context publish does NOT re-render the host (the self-feedback fix)", () => {
    renders = 0;
    render(<CountingHost atlasEligible={false} />);
    expect(renders).toBe(1); // one mount commit
    // A dock-neutral publish (focus leaves to a non-index channel, no key
    // selection) drives the controller but triggers no setDock, so the ONLY
    // way the host could re-render is a reactive atlas-context read — which the
    // fix removed. Under the old reactive hook this incremented `renders`.
    act(() => publishAtlasContext({ emitterId: 5, focusedTrack: "red", interpolation: "linear", selection: { frame: null, keyTimes: [] } }));
    expect(renders).toBe(1); // unchanged — publish did not re-render the host
  });

  it("side effects still fire on publish (open reaches the dock)", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas"); // the non-reactive subscription still drives the reducer
  });

  it("a keyTimes-only publish does NOT fire a spurious emitterCleared restore (per-slice gate)", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    // emitterId stays null across both publishes; only keyTimes changes on the
    // second. The emitter-cleared branch is gated on emitterId CHANGING, so it
    // must not fire and cancel the just-opened picker (soundness-4).
    act(() => publishAtlasContext({ emitterId: null, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [] } }));
    act(() => publishAtlasContext({ emitterId: null, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas"); // opened AND stayed — emitterCleared did not spuriously restore
  });

  it("keySelected reads the CURRENT atlasEligible (layout-synced ref, not a frozen closure)", () => {
    const { rerender } = renderHook(
      ({ atlasEligible }) => useAtlasAutoOpen({ atlasEligible }),
      { initialProps: { atlasEligible: false } },
    );
    // Enter index while INELIGIBLE — arms, but a later selection won't open yet.
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("lighting");
    // Become eligible, THEN select a key. If the ref were frozen at false the
    // keySelected would carry eligible:false and never open.
    rerender({ atlasEligible: true });
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas"); // fresh eligibility observed → opened
  });

  it("mount replay opens when the store ALREADY holds an index+selection context, AND stays restorable", () => {
    // Pre-populate BEFORE mounting the hook — the existing suite always
    // publishes after mount, so the replay path was previously uncovered.
    publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    expect(dock()).toBe("atlas"); // the mount replay fired the open
    // The open must remain ACTIVE (restorable), not self-cancel: the dock
    // baseline set in the mount branch must NOT clobber the "atlas" the replay
    // wrote (else a spurious userDockMutation would cancel the auto-open).
    // Leaving the index channel must restore the remembered dock.
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "red", interpolation: "linear", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("lighting"); // restored — the auto-open was still active
  });

  it("losing eligibility while auto-open is active restores the remembered dock", () => {
    const { rerender } = renderHook(
      ({ atlasEligible }) => useAtlasAutoOpen({ atlasEligible }),
      { initialProps: { atlasEligible: true } },
    );
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    rerender({ atlasEligible: false }); // eligibility lost → restore
    expect(dock()).toBe("lighting");
  });

  it("keySelected captures the FRESH manually-set dock as remembered (ref stays synced)", () => {
    renderHook(() => useAtlasAutoOpen({ atlasEligible: true }));
    // Manual dock change cancels auto-driving (userDockMutation)...
    act(() => setDock("spawner"));
    // ...re-arm by leaving and re-entering index, then select a key. The
    // remembered dock must be the CURRENT "spawner", not the stale "lighting".
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "red", interpolation: "linear", selection: { frame: null, keyTimes: [] } }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [] } }));
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas");
    // Leave index → restore. If keySelected had captured a stale "lighting"
    // this would restore to "lighting" instead of "spawner".
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "red", interpolation: "linear", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("spawner");
  });

  it("under StrictMode: opens correctly, and the subscription is cleaned up on unmount", () => {
    const { unmount } = render(
      <StrictMode>
        <CountingHost atlasEligible={true} />
      </StrictMode>,
    );
    act(() => publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } }));
    expect(dock()).toBe("atlas"); // double-mount did not corrupt the open
    unmount();
    // Move the dock away from the remembered value, then publish a change that
    // a LEAKED subscription would act on (emitterId → null fires emitterCleared,
    // which for a stale-active closure would restore to remembered "lighting").
    act(() => setDock("spawner"));
    act(() => publishAtlasContext({ emitterId: null, focusedTrack: "red", interpolation: "linear", selection: { frame: null, keyTimes: [] } }));
    expect(dock()).toBe("spawner"); // no leaked listener — dock untouched
  });

  it("ref sync is a LAYOUT effect: a same-commit prop change + passive-effect publish reads the FRESH value", () => {
    // This mirrors CurveEditorPanel's real structure: the publish fires from a
    // PASSIVE effect declared BEFORE the hook is mounted. In one commit React
    // flushes layout effects before passive effects, so the `useLayoutEffect`
    // ref sync runs before the publish notifies `handle`. A passive (non-layout)
    // ref sync would instead be registered AFTER this publish effect and run
    // LATER, so `handle` would read a STALE `atlasEligible` — the earlier tests
    // can't catch that because they change props/publish in separate act()s.
    function PublishingHost({ eligible, ctx }: { eligible: boolean; ctx: AtlasContext }) {
      useEffect(() => { publishAtlasContext(ctx); }, [ctx]); // publisher — BEFORE the hook
      useAtlasAutoOpen({ atlasEligible: eligible });
      return null;
    }
    const armCtx: AtlasContext = { emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: null, keyTimes: [] } };
    const selectCtx: AtlasContext = { emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } };
    const { rerender } = render(<PublishingHost eligible={false} ctx={armCtx} />);
    expect(dock()).toBe("lighting"); // armed on index, but ineligible → not open yet
    // ONE commit: eligible false→true AND ctx arm→select (the rising key edge).
    // keySelected must observe the fresh eligible=true (layout-synced) and open.
    rerender(<PublishingHost eligible={true} ctx={selectCtx} />);
    expect(dock()).toBe("atlas");
  });
});
