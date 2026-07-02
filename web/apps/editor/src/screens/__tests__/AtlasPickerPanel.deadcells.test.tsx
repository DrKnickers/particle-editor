// Dead-cell treatment in AtlasPickerPanel: frames whose atlas cell is effectively empty
// (alpha ≈ 0) are dimmed, aria-disabled, and non-selectable (mouse AND keyboard), with an
// aria-live announcement so the block isn't silent; the confirm modal re-checks at commit.
//
// The pixel logic lives in the pure `deadCellsFromAlpha` (unit-tested separately); jsdom has
// no real canvas, so here we mock the impure `computeDeadCells` shell to inject a known set —
// exactly the seam the plan review asked for.
import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { render, screen, waitFor, fireEvent, act } from "@testing-library/react";

vi.mock("@/lib/atlas-dead-cells", async (orig) => {
  const actual = await orig<typeof import("@/lib/atlas-dead-cells")>();
  return { ...actual, computeDeadCells: vi.fn(async () => new Set<number>()) };
});

import { AtlasPickerPanel, __resetAtlasPropsCache } from "../AtlasPickerPanel";
import { computeDeadCells } from "@/lib/atlas-dead-cells";
import { publishAtlasContext, __resetAtlasContext } from "@/lib/atlas-context";
import { MockBridge } from "@/bridge/mock";
import { useMockEmitterProperties } from "@/bridge/mock-state";
import { __resetPreviewCache } from "@/lib/atlas-preview-cache";
import { __resetModStackForTests } from "@/lib/mod-stack";
import { useDockAnim } from "@/lib/dock-anim";

beforeEach(() => {
  __resetAtlasContext();
  useMockEmitterProperties.getState().reset();
  __resetPreviewCache();
  __resetModStackForTests();
  __resetAtlasPropsCache();
  useDockAnim.setState({ atlasTerminalFirstPaint: false, atlasGridMounted: false });
  vi.mocked(computeDeadCells).mockResolvedValue(new Set<number>());
});
afterEach(() => { vi.restoreAllMocks(); });

// textureSize 16 → a 4×4 grid (16 frames). Default selection: a single index key so a click
// assigns directly (no confirm modal). Override `selection` for the keyboard / confirm cases.
function setup(
  selection: { frame: number | null; keyTimes: number[] } = { frame: 5, keyTimes: [0.3] },
  blendAlphaGated = true, // dead-cell dimming only runs for alpha-gated emitters
) {
  useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated });
  publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection });
  return render(<AtlasPickerPanel bridge={new MockBridge()} onClose={() => {}} />);
}

// For the transition cases: the picker refetches emitter-properties on the
// `emitters/tree/changed` broadcast (AtlasPickerPanel.tsx), NOT on a same-emitterId
// context republish. Build the bridge inline and fire the captured handler,
// mirroring AtlasPickerPanel.test.tsx's tree/changed tests. (setup() hides its
// bridge, so it can't be used here.)
function renderWithTreeChanged(blendAlphaGated: boolean) {
  const bridge = new MockBridge();
  const handlers: Array<(e: unknown) => void> = [];
  const realOn = bridge.on.bind(bridge);
  vi.spyOn(bridge, "on").mockImplementation((kind, cb) => {
    if (kind === "emitters/tree/changed") handlers.push(cb as (e: unknown) => void);
    return realOn(kind, cb as never);
  });
  useMockEmitterProperties.getState().patch(1, { textureSize: 16, colorTexture: "fire.dds", blendAlphaGated });
  publishAtlasContext({ emitterId: 1, focusedTrack: "index", interpolation: "step", selection: { frame: 5, keyTimes: [0.3] } });
  render(<AtlasPickerPanel bridge={bridge} onClose={() => {}} />);
  const fireChanged = () => act(() => { handlers.forEach((h) => h({ kind: "emitters/tree/changed" })); });
  return { fireChanged };
}

const cell = (k: number) =>
  screen.getAllByTestId("atlas-cell").find((c) => c.getAttribute("data-frame") === String(k))!;
const isDimmed = (k: number) =>
  cell(k).querySelector('[data-testid="atlas-cell-dead-scrim"]') !== null;

describe("AtlasPickerPanel — dead cells", () => {
  it("marks the sampled dead frame disabled + dimmed, and leaves the rest alive", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup();
    await waitFor(() => expect(cell(2).getAttribute("data-dead")).toBe("true"));
    expect(cell(2).getAttribute("aria-disabled")).toBe("true");
    expect(cell(2).getAttribute("aria-label")).toMatch(/empty/i);
    expect(cell(2).className).toContain("cursor-not-allowed");
    expect(isDimmed(2)).toBe(true);                    // dimmed via the scrim (not root opacity)
    // A live neighbor is untouched.
    expect(cell(3).getAttribute("data-dead")).toBe("false");
    expect(cell(3).getAttribute("aria-disabled")).toBeNull();
    expect(isDimmed(3)).toBe(false);
  });

  it("blocks assigning a dead frame by MOUSE and announces it (no silent no-op)", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup();
    await waitFor(() => expect(cell(2).getAttribute("data-dead")).toBe("true"));
    fireEvent.click(cell(2));
    await waitFor(() => expect(screen.getByText(/frame 2 is empty/i)).toBeTruthy());
    expect(screen.queryByText(/assigned frame 2/i)).toBeNull();
  });

  it("blocks assigning a dead frame by KEYBOARD (Enter) and announces it", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([0]));
    setup({ frame: null, keyTimes: [0.3] }); // no assignment → roving target defaults to frame 0
    await waitFor(() => expect(cell(0).getAttribute("data-dead")).toBe("true"));
    fireEvent.keyDown(screen.getByRole("listbox"), { key: "Enter" });
    await waitFor(() => expect(screen.getByText(/frame 0 is empty/i)).toBeTruthy());
    expect(screen.queryByText(/assigned frame 0/i)).toBeNull();
  });

  it("re-checks at the confirm modal: a frame that dies while the modal is open can't be assigned", async () => {
    // Multi-key selection with no common frame → a click opens the confirm modal (not a direct
    // assign). Hold the sample pending so the click lands while frame 4 is still "live"; resolve
    // it dead AFTER the modal opens, then confirm → the commit-time recheck blocks it.
    let resolveDead!: (s: Set<number>) => void;
    vi.mocked(computeDeadCells).mockReturnValue(new Promise<Set<number>>((r) => { resolveDead = r; }));
    setup({ frame: null, keyTimes: [0.3, 0.7] });
    await waitFor(() => expect(computeDeadCells).toHaveBeenCalled()); // effect fired, sample pending
    fireEvent.click(cell(4));                                          // deadCells empty → modal opens
    await waitFor(() => expect(screen.getByText(/set all to frame 4/i)).toBeTruthy());
    await act(async () => { resolveDead(new Set([4])); });            // frame 4 dies mid-modal
    await waitFor(() => expect(cell(4).getAttribute("data-dead")).toBe("true"));
    fireEvent.click(screen.getByRole("button", { name: /set all/i }));
    await waitFor(() => expect(screen.getByText(/frame 4 is empty/i)).toBeTruthy());
    expect(screen.queryByText(/assigned frame 4/i)).toBeNull();
  });

  it("still assigns a live (non-dead) frame", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup();
    await waitFor(() => expect(cell(2).getAttribute("data-dead")).toBe("true"));
    fireEvent.click(cell(3));
    await waitFor(() => expect(screen.getByText(/assigned frame 3/i)).toBeTruthy());
  });

  it("dims nothing when the texture has no empty cells", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set<number>());
    setup();
    await waitFor(() => expect(screen.getAllByTestId("atlas-cell")).toHaveLength(16));
    expect(screen.getAllByTestId("atlas-cell").every((c) => c.getAttribute("data-dead") === "false")).toBe(true);
  });

  it("does NOT dim dead frames when the emitter is not alpha-gated", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup({ frame: 5, keyTimes: [0.3] }, /* blendAlphaGated */ false);
    // Grid renders; the effect early-returns without dimming, and computeDeadCells
    // is never consulted for an additive/non-gated emitter.
    await waitFor(() => expect(cell(0)).toBeTruthy());
    expect(cell(2).getAttribute("data-dead")).toBe("false");
    expect(isDimmed(2)).toBe(false);
    expect(computeDeadCells).not.toHaveBeenCalled();
  });

  it("clears dimming when blendAlphaGated flips true → false", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    const { fireChanged } = renderWithTreeChanged(/* blendAlphaGated */ true);
    await waitFor(() => expect(cell(2).getAttribute("data-dead")).toBe("true"));
    useMockEmitterProperties.getState().patch(1, { blendAlphaGated: false });
    fireChanged(); // picker refetches → now not gated
    await waitFor(() => expect(cell(2).getAttribute("data-dead")).toBe("false"));
    expect(isDimmed(2)).toBe(false);
  });

  it("runs detection when blendAlphaGated flips false → true", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    const { fireChanged } = renderWithTreeChanged(/* blendAlphaGated */ false);
    await waitFor(() => expect(cell(0)).toBeTruthy());
    expect(cell(2).getAttribute("data-dead")).toBe("false");
    useMockEmitterProperties.getState().patch(1, { blendAlphaGated: true });
    fireChanged(); // picker refetches → now gated
    await waitFor(() => expect(cell(2).getAttribute("data-dead")).toBe("true"));
    expect(isDimmed(2)).toBe(true);
  });
});
