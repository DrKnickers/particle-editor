import { render, screen, waitFor, fireEvent } from "@testing-library/react";
import { describe, it, expect, beforeEach } from "vitest";
import * as Tooltip from "@radix-ui/react-tooltip";
import { MockBridge } from "@/bridge/mock";
import { useMockEngineState } from "@/bridge/mock-state";
import { ViewportToggleOverlay } from "@/components/ViewportToggleOverlay";

function setup() {
  const bridge = new MockBridge();
  // Mirror the Tooltip.Provider that App.tsx supplies in production (the pill's
  // buttons use the <Tip> primitive).
  render(
    <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>
      <ViewportToggleOverlay bridge={bridge} />
    </Tooltip.Provider>,
  );
  return bridge;
}

// The mock engine state is a module-level singleton — reset it so each test
// starts from defaults (incl. ground: true) and never leaks into the next.
beforeEach(() => useMockEngineState.getState().reset());

describe("ViewportToggleOverlay", () => {
  it("renders the four toggles", async () => {
    setup();
    await waitFor(() =>
      expect(screen.getByRole("button", { name: /show ground/i })).toBeInTheDocument(),
    );
    expect(screen.getByRole("button", { name: /show grid/i })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: /toggle bloom/i })).toBeInTheDocument();
    // lock label is conditional; /lock/i matches both "Lock…" and "…to lock it"
    expect(screen.getByRole("button", { name: /lock/i })).toBeInTheDocument();
  });

  it("reflects ground state via aria-pressed and toggles it", async () => {
    setup();
    const btn = await screen.findByRole("button", { name: /show ground/i });
    const before = btn.getAttribute("aria-pressed");
    fireEvent.click(btn);
    await waitFor(() =>
      expect(
        screen.getByRole("button", { name: /show ground/i }).getAttribute("aria-pressed"),
      ).not.toBe(before),
    );
  });

  it("disables the lock when no reference object is loaded", async () => {
    setup();
    const lock = await screen.findByRole("button", { name: /lock/i });
    expect(lock).toBeDisabled();
  });

  it("enables + toggles the lock once a reference object is loaded", async () => {
    const bridge = setup();
    await bridge.request({ kind: "engine/set/reference-object", params: { name: "AT_ST_Walker" } });
    const lock = await screen.findByRole("button", { name: /lock/i });
    await waitFor(() => expect(lock).not.toBeDisabled());
    fireEvent.click(lock);
    await waitFor(() =>
      expect(screen.getByRole("button", { name: /lock/i }).getAttribute("aria-pressed")).toBe("true"),
    );
  });

  it("uses a light scrim over a dark/neutral background (ground hidden)", async () => {
    const bridge = setup();
    await bridge.request({ kind: "engine/set/ground", params: { enabled: false } });
    await bridge.request({ kind: "engine/set/background", params: { rgb: 0x006e6e6e } });
    const group = await screen.findByRole("group", { name: /viewport display options/i });
    await waitFor(() => expect(group.getAttribute("data-scrim")).toBe("light"));
  });

  it("uses a dark scrim over a bright background (ground hidden)", async () => {
    const bridge = setup();
    await bridge.request({ kind: "engine/set/ground", params: { enabled: false } });
    await bridge.request({ kind: "engine/set/background", params: { rgb: 0xffffff } });
    const group = await screen.findByRole("group", { name: /viewport display options/i });
    await waitFor(() => expect(group.getAttribute("data-scrim")).toBe("dark"));
  });

  it("uses the averaged TEXTURE colour when a textured ground is shown (ignores the background)", async () => {
    const bridge = setup();
    await bridge.request({ kind: "engine/set/ground", params: { enabled: true } }); // default slot 0 = dirt (dark avg)
    await bridge.request({ kind: "engine/set/background", params: { rgb: 0xffffff } }); // bright bg — ignored when ground shown
    const group = await screen.findByRole("group", { name: /viewport display options/i });
    await waitFor(() => expect(group.getAttribute("data-scrim")).toBe("light")); // dark dirt floor → light scrim
  });

  it("uses the SOLID ground colour when the ground plane is shown (solid-colour slot)", async () => {
    const bridge = setup();
    await bridge.request({ kind: "engine/set/ground", params: { enabled: true } });
    await bridge.request({ kind: "engine/set/ground-texture", params: { slot: 4 } }); // solid-colour slot
    await bridge.request({ kind: "engine/set/ground-solid-color", params: { rgb: 0xffffff } }); // white floor
    await bridge.request({ kind: "engine/set/background", params: { rgb: 0x000000 } }); // black bg — ignored when ground shown
    const group = await screen.findByRole("group", { name: /viewport display options/i });
    await waitFor(() => expect(group.getAttribute("data-scrim")).toBe("dark")); // white floor → dark scrim
  });
});
