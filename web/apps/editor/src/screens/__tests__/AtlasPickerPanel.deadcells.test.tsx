// Dead-cell treatment in AtlasPickerPanel: frames whose atlas cell is effectively empty
// (alpha ≈ 0) are dimmed (a scrim drawn on the grid canvas), announced as "empty" on the
// active a11y option, and non-selectable (mouse AND keyboard); the confirm modal re-checks
// at commit.
//
// [#572] The grid is ONE <canvas>, so per-cell "dimmed" pixels can't be queried in jsdom.
// These tests verify the OBSERVABLE contract instead: the active option (whichever cell is
// roving) announces aria-disabled + "empty" for a dead frame, and clicks/Enter on a dead
// frame announce "empty" and do NOT assign. The pixel logic lives in the pure
// `deadCellsFromAlpha` (unit-tested separately); here we mock the impure `computeDeadCells`
// shell to inject a known set — exactly the seam the plan review asked for.
import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { render, screen, waitFor, fireEvent, act, cleanup } from "@testing-library/react";

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
// Unmount BEFORE clearing mocks. Hooks run LIFO, so this afterEach fires
// ahead of Testing Library's auto-cleanup — with the bare restore, a
// late-resolving preview promise could re-fire the still-mounted panel's
// dead-cell effect AFTER the history clear, bleeding one computeDeadCells
// call into the NEXT test's assertions (the not-alpha-gated test flaked on
// exactly this under CPU load, ~1-in-15). Explicit cleanup() first unmounts
// (the effect's `live` guards then drop stragglers), then the restore wipes
// anything the test itself recorded.
afterEach(() => {
  cleanup();
  vi.restoreAllMocks();
});

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
// context republish. Build the bridge inline and fire the captured handler.
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

// ── canvas-grid helpers (4 cols × 50px cells, gap 4) ─────────────────────────
const listbox = () => screen.getByRole("listbox", { name: /atlas frames/i });
const activeDesc = () => listbox().getAttribute("aria-activedescendant");
const activeOption = () => screen.getByTestId("atlas-active-option");
function frameCenter(k: number, cols = 4, cell = 50) {
  const step = cell + 4;
  return { clientX: (k % cols) * step + cell / 2, clientY: Math.floor(k / cols) * step + cell / 2 };
}
const clickFrame = (k: number) => fireEvent.click(listbox(), frameCenter(k));
// Move the roving cursor to frame k (small k) from anywhere: Home then k ArrowRights.
function rovingTo(grid: HTMLElement, k: number) {
  fireEvent.keyDown(grid, { key: "Home" });
  for (let i = 0; i < k; i++) fireEvent.keyDown(grid, { key: "ArrowRight" });
}

describe("AtlasPickerPanel — dead cells", () => {
  it("announces the sampled dead frame as disabled/empty, and leaves the rest alive", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup();
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // cursor settled before nav
    rovingTo(grid, 2);
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-2"));
    await waitFor(() => expect(activeOption().getAttribute("aria-disabled")).toBe("true"));
    expect(activeOption().getAttribute("aria-label")).toMatch(/empty/i);
    // A live neighbor (3) is untouched.
    fireEvent.keyDown(grid, { key: "ArrowRight" });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-3"));
    expect(activeOption().getAttribute("aria-disabled")).toBeNull();
    expect(activeOption().getAttribute("aria-label")).not.toMatch(/empty/i);
  });

  it("blocks assigning a dead frame by MOUSE and announces it (no silent no-op)", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup();
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // cursor settled before nav
    // Confirm the dead set has landed (roving 2 reads disabled) before clicking.
    rovingTo(grid, 2);
    await waitFor(() => expect(activeOption().getAttribute("aria-disabled")).toBe("true"));
    clickFrame(2);
    await waitFor(() => expect(screen.getByText(/frame 2 is empty/i)).toBeTruthy());
    expect(screen.queryByText(/assigned frame 2/i)).toBeNull();
  });

  it("blocks assigning a dead frame by KEYBOARD (Enter) and announces it", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([0]));
    setup({ frame: null, keyTimes: [0.3] }); // no assignment → roving target defaults to frame 0
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeOption().getAttribute("aria-disabled")).toBe("true"));
    fireEvent.keyDown(grid, { key: "Enter" });
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
    await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(computeDeadCells).toHaveBeenCalled()); // effect fired, sample pending
    clickFrame(4);                                                    // deadCells empty → modal opens
    await waitFor(() => expect(screen.getByText(/set all to frame 4/i)).toBeTruthy());
    await act(async () => { resolveDead(new Set([4])); });           // frame 4 dies mid-modal
    fireEvent.click(screen.getByRole("button", { name: /set all/i }));
    await waitFor(() => expect(screen.getByText(/frame 4 is empty/i)).toBeTruthy());
    expect(screen.queryByText(/assigned frame 4/i)).toBeNull();
  });

  it("still assigns a live (non-dead) frame", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup();
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // cursor settled before nav
    rovingTo(grid, 2);
    await waitFor(() => expect(activeOption().getAttribute("aria-disabled")).toBe("true")); // dead set landed
    clickFrame(3);
    await waitFor(() => expect(screen.getByText(/assigned frame 3/i)).toBeTruthy());
  });

  it("dims nothing when the texture has no empty cells", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set<number>());
    setup();
    await screen.findByRole("listbox", { name: /atlas frames/i });
    // No frame is blocked → clicking a frame assigns it.
    clickFrame(2);
    await waitFor(() => expect(screen.getByText(/assigned frame 2/i)).toBeTruthy());
  });

  it("does NOT dim dead frames when the emitter is not alpha-gated", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    setup({ frame: 5, keyTimes: [0.3] }, /* blendAlphaGated */ false);
    await screen.findByRole("listbox", { name: /atlas frames/i });
    // The effect early-returns without dimming, and computeDeadCells is never
    // consulted for an additive/non-gated emitter → frame 2 assigns normally.
    clickFrame(2);
    await waitFor(() => expect(screen.getByText(/assigned frame 2/i)).toBeTruthy());
    expect(computeDeadCells).not.toHaveBeenCalled();
  });

  it("clears dimming when blendAlphaGated flips true → false", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    const { fireChanged } = renderWithTreeChanged(/* blendAlphaGated */ true);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // cursor settled before nav
    rovingTo(grid, 2);
    await waitFor(() => expect(activeOption().getAttribute("aria-disabled")).toBe("true"));
    useMockEmitterProperties.getState().patch(1, { blendAlphaGated: false });
    fireChanged(); // picker refetches → now not gated → dimming clears
    await waitFor(() => expect(activeOption().getAttribute("aria-disabled")).toBeNull());
  });

  it("runs detection when blendAlphaGated flips false → true", async () => {
    vi.mocked(computeDeadCells).mockResolvedValue(new Set([2]));
    const { fireChanged } = renderWithTreeChanged(/* blendAlphaGated */ false);
    const grid = await screen.findByRole("listbox", { name: /atlas frames/i });
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-5")); // cursor settled before nav
    rovingTo(grid, 2);
    await waitFor(() => expect(activeDesc()).toBe("atlas-opt-2"));
    expect(activeOption().getAttribute("aria-disabled")).toBeNull(); // not gated yet
    useMockEmitterProperties.getState().patch(1, { blendAlphaGated: true });
    fireChanged(); // picker refetches → now gated → detection runs
    await waitFor(() => expect(activeOption().getAttribute("aria-disabled")).toBe("true"));
  });
});
