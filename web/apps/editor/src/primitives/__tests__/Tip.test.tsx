import { describe, it, expect } from "vitest";
import { render, screen, act } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import { Tip } from "../Tip";
import { BridgeContext } from "@/lib/bridge-context";
import type { Bridge } from "@particle-editor/bridge-schema";

// Render helper: Radix Tooltip requires a Provider. delayDuration=0 so
// tests don't need fake timers. Opening via focus() is the reliable
// jsdom path (hover needs real pointer events Radix sniffs for).
function renderTip(ui: React.ReactElement, bridge: Bridge | null = null) {
  return render(
    <BridgeContext.Provider value={bridge}>
      <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{ui}</Tooltip.Provider>
    </BridgeContext.Provider>,
  );
}

describe("Tip", () => {
  it("renders the trigger unchanged (asChild — no wrapper element)", () => {
    renderTip(
      <Tip content="Save the file"><button aria-label="Save">S</button></Tip>,
    );
    const btn = screen.getByRole("button", { name: "Save" });
    expect(btn.parentElement?.tagName).not.toBe("SPAN"); // no shim injected
    expect(btn).not.toHaveAttribute("title");
  });

  it("opens on focus and shows the styled content", () => {
    renderTip(
      <Tip content="Save the file"><button aria-label="Save">S</button></Tip>,
    );
    act(() => screen.getByRole("button", { name: "Save" }).focus());
    // Radix renders the visible content + a duplicate inside a visually
    // hidden live-region span; getAllBy tolerates both.
    const contents = screen.getAllByText("Save the file");
    expect(contents.length).toBeGreaterThan(0);
    const surface = document.querySelector(".tip-surface");
    expect(surface).not.toBeNull();
    // The animated Content wraps the visual surface (so its overflow:hidden
    // can't clip the Arrow).
    const animated = document.querySelector(".tip-animate");
    expect(animated).not.toBeNull();
    expect(animated!.contains(surface)).toBe(true);
  });

  it("renders the bare child when content is nullish or empty", () => {
    renderTip(
      <Tip content={undefined}><button aria-label="Plain">P</button></Tip>,
    );
    act(() => screen.getByRole("button", { name: "Plain" }).focus());
    expect(document.querySelector(".tip-surface")).toBeNull();
  });

  it("forwards side and align to the content", () => {
    renderTip(
      <Tip content="hint" side="right" align="start"><button aria-label="T">T</button></Tip>,
    );
    act(() => screen.getByRole("button", { name: "T" }).focus());
    // side/align are forwarded to the Radix Content (now the .tip-animate element).
    const content = document.querySelector(".tip-animate");
    expect(content).toHaveAttribute("data-side", "right");
    expect(content).toHaveAttribute("data-align", "start");
  });

  it("wraps plain-string content in the padded tip-body (rich JSX brings its own padding)", () => {
    renderTip(
      <Tip content="plain hint"><button aria-label="T">T</button></Tip>,
    );
    act(() => screen.getByRole("button", { name: "T" }).focus());
    expect(document.querySelector(".tip-surface .tip-body")).not.toBeNull();
  });
});
