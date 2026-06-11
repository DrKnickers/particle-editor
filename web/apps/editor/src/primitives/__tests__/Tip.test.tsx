import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, act } from "@testing-library/react";
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

function makeBridge() {
  const request = vi.fn().mockResolvedValue({ ok: true });
  const on = vi.fn().mockReturnValue(() => {});
  return { bridge: { request, on } as unknown as Bridge, request };
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
    expect(surface!.className).toContain("tip-animate");
  });

  it("renders the bare child when content is nullish or empty (T4 conditional sites)", () => {
    renderTip(
      <Tip content={undefined}><button aria-label="Plain">P</button></Tip>,
    );
    act(() => screen.getByRole("button", { name: "Plain" }).focus());
    expect(document.querySelector(".tip-surface")).toBeNull();
  });

  it("registers a viewport occlusion while open when occlusionId is set, and releases on close", async () => {
    const { bridge, request } = makeBridge();
    renderTip(
      <Tip content="hint" occlusionId="tip:test:x"><button aria-label="T">T</button></Tip>,
      bridge,
    );
    const btn = screen.getByRole("button", { name: "T" });
    act(() => btn.focus());
    await act(async () => {}); // flush the occlusion request effect
    const occlude = request.mock.calls
      .map((c) => c[0] as { kind: string; params: { id: string; rect: unknown } })
      .filter((r) => r.kind === "viewport/occlude");
    expect(occlude.length).toBeGreaterThan(0);
    expect(occlude[0]!.params.id).toBe("tip:test:x");
    expect(occlude[0]!.params.rect).not.toBeNull();

    request.mockClear();
    act(() => { btn.blur(); fireEvent.pointerLeave(btn); });
    await act(async () => {});
    const release = request.mock.calls
      .map((c) => c[0] as { kind: string; params: { id: string; rect: unknown } })
      .filter((r) => r.kind === "viewport/occlude" && r.params.rect === null);
    expect(release.length).toBe(1);
  });

  it("makes no bridge traffic without an occlusionId", async () => {
    const { bridge, request } = makeBridge();
    renderTip(
      <Tip content="hint"><button aria-label="T">T</button></Tip>,
      bridge,
    );
    act(() => screen.getByRole("button", { name: "T" }).focus());
    await act(async () => {});
    expect(request).not.toHaveBeenCalled();
  });

  it("forwards side and align to the content", () => {
    renderTip(
      <Tip content="hint" side="right" align="start"><button aria-label="T">T</button></Tip>,
    );
    act(() => screen.getByRole("button", { name: "T" }).focus());
    const surface = document.querySelector(".tip-surface");
    expect(surface).toHaveAttribute("data-side", "right");
    expect(surface).toHaveAttribute("data-align", "start");
  });

  it("wraps plain-string content in the padded tip-body (rich JSX brings its own padding)", () => {
    renderTip(
      <Tip content="plain hint"><button aria-label="T">T</button></Tip>,
    );
    act(() => screen.getByRole("button", { name: "T" }).focus());
    expect(document.querySelector(".tip-surface .tip-body")).not.toBeNull();
  });
});
