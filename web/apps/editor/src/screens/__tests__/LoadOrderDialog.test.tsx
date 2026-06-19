import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor, within } from "@testing-library/react";
import { LoadOrderDialog } from "../LoadOrderDialog";
import type { Bridge } from "@particle-editor/bridge-schema";

const LAYERS = [
  { path: "C:/m/Alpha",          label: "Alpha",    isFoC: true,  kind: "mod" as const },
  { path: "C:/m/Alpha/Mod",     label: "Mod",     parentLabel: "Alpha", isFoC: true, kind: "nested" as const },
  { path: "C:/m/Alpha/Core", label: "Core", parentLabel: "Alpha", isFoC: true, kind: "nested" as const },
];
function makeBridge(stack: string[]) {
  const request = vi.fn().mockImplementation((req: { kind: string }) =>
    req.kind === "mods/list"
      ? Promise.resolve({ mods: [], layers: LAYERS, stack, activePath: stack[0] ?? null })
      : Promise.resolve({ ok: true, stack: [] }));
  return { request, on: vi.fn().mockReturnValue(() => {}) } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

describe("LoadOrderDialog", () => {
  it("initialises the load order from the current stack, numbered", async () => {
    render(<LoadOrderDialog bridge={makeBridge(["C:/m/Alpha/Mod", "C:/m/Alpha/Core"])} open onOpenChange={() => {}} onApplied={() => {}} />);
    await waitFor(() => expect(screen.getByRole("list", { name: "Load order" })).toBeTruthy());
    // Scope to the Load-order list (role=list "Load order") — it's the only list.
    const orderList = screen.getByRole("list", { name: "Load order" });
    const items = within(orderList).getAllByRole("listitem").map((li) => li.textContent ?? "");
    expect(items[0]).toContain("Mod");
    expect(items[1]).toContain("Core");
  });

  it("＋ add appends an available layer to the stack", async () => {
    render(<LoadOrderDialog bridge={makeBridge([])} open onOpenChange={() => {}} onApplied={() => {}} />);
    await waitFor(() => screen.getByRole("button", { name: "Add Mod" }));
    fireEvent.click(screen.getByRole("button", { name: "Add Mod" }));
    expect(screen.getByRole("list", { name: "Load order" }).textContent).toContain("Mod");
  });

  it("↑ reorder changes Apply order; × removes; Apply dispatches set-layers", async () => {
    const bridge = makeBridge(["C:/m/Alpha/Mod", "C:/m/Alpha/Core"]);
    const onApplied = vi.fn();
    render(<LoadOrderDialog bridge={bridge} open onOpenChange={() => {}} onApplied={onApplied} />);
    await waitFor(() => screen.getByRole("button", { name: "Move Core up" }));
    fireEvent.click(screen.getByRole("button", { name: "Move Core up" }));
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({ kind: "mods/set-layers", params: { paths: ["C:/m/Alpha/Core", "C:/m/Alpha/Mod"] } });
    });
    expect(onApplied).toHaveBeenCalled();
  });

  it("Cancel does not dispatch set-layers", async () => {
    const bridge = makeBridge(["C:/m/Alpha/Mod"]);
    render(<LoadOrderDialog bridge={bridge} open onOpenChange={() => {}} onApplied={() => {}} />);
    await waitFor(() => screen.getByRole("list", { name: "Load order" }));
    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));
    const kinds = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0].kind);
    expect(kinds).not.toContain("mods/set-layers");
  });

  // FIX A regression: a stack path absent from the catalog (a rootHasArt=false
  // mod root that the catalog excludes) MUST survive in the Load-order list AND
  // be dispatched on Apply — never silently dropped.
  it("preserves an in-stack path not present in the catalog (no data-loss on Apply)", async () => {
    // "C:/m/GrayMod" is genuinely ABSENT from LAYERS — it stands in for a
    // rootHasArt=false / MEG-packed mod root the catalog excludes. The old
    // catalog-membership filter would have dropped it on open (data loss).
    const bridge = makeBridge(["C:/m/GrayMod", "C:/m/Alpha/Mod"]);
    const onApplied = vi.fn();
    render(<LoadOrderDialog bridge={bridge} open onOpenChange={() => {}} onApplied={onApplied} />);
    await waitFor(() => expect(screen.getByRole("list", { name: "Load order" })).toBeTruthy());
    const orderList = screen.getByRole("list", { name: "Load order" });
    const items = within(orderList).getAllByRole("listitem").map((li) => li.textContent ?? "");
    // The non-catalog root survives, labelled by its basename ("GrayMod" — NOT in LAYERS).
    expect(items.some((t) => t.includes("GrayMod"))).toBe(true);
    expect(items.some((t) => t.includes("Mod"))).toBe(true);
    // Apply dispatches the FULL stack including the non-catalog path.
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/set-layers",
        params: { paths: ["C:/m/GrayMod", "C:/m/Alpha/Mod"] },
      });
    });
    expect(onApplied).toHaveBeenCalled();
  });

  it("Search mods filters the available list (case-insensitive) with an empty state", async () => {
    render(<LoadOrderDialog bridge={makeBridge([])} open onOpenChange={() => {}} onApplied={() => {}} />);
    await waitFor(() => screen.getByRole("button", { name: "Add Mod" }));
    const search = screen.getByLabelText("Search mods");
    // Lowercase query vs "Mod" — case-insensitive match keeps Mod, drops Core.
    fireEvent.change(search, { target: { value: "Mod" } });
    expect(screen.getByRole("button", { name: "Add Mod" })).toBeTruthy();
    expect(screen.queryByRole("button", { name: "Add Core" })).toBeNull();
    // No match → no add buttons + the empty-state hint.
    fireEvent.change(search, { target: { value: "zzz-nope" } });
    expect(screen.queryByRole("button", { name: /^Add / })).toBeNull();
    expect(screen.getByText(/no mods match/i)).toBeTruthy();
  });

  // Pointer-event drag (native HTML5 DnD is dead inside this Radix Dialog +
  // WebView2 — see LoadOrderDialog.tsx). jsdom returns zeroed rects, so stub each
  // row's geometry and drive synthetic pointer events past the drag threshold.
  it("pointer drag reorders the stack", async () => {
    const bridge = makeBridge(["C:/m/Alpha/Mod", "C:/m/Alpha/Core"]);
    const onApplied = vi.fn();
    render(<LoadOrderDialog bridge={bridge} open onOpenChange={() => {}} onApplied={onApplied} />);
    await waitFor(() => screen.getByRole("list", { name: "Load order" }));
    const list = screen.getByRole("list", { name: "Load order" });
    const rows = within(list).getAllByRole("listitem");
    const rect = (top: number): DOMRect =>
      ({ top, height: 26, bottom: top + 26, left: 0, right: 0, width: 0, x: 0, y: top, toJSON: () => ({}) }) as DOMRect;
    rows[0].getBoundingClientRect = () => rect(0);   // Mod    y 0–26  (mid 13)
    rows[1].getBoundingClientRect = () => rect(26);  // Core y 26–52 (mid 39)
    // Grab Mod (row 0) and drag below Core's midpoint → append (index 2).
    fireEvent.pointerDown(rows[0], { button: 0, pointerId: 1, clientY: 13 });
    fireEvent.pointerMove(rows[0], { pointerId: 1, clientY: 50 });
    fireEvent.pointerUp(rows[0], { pointerId: 1, clientY: 50 });
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/set-layers",
        params: { paths: ["C:/m/Alpha/Core", "C:/m/Alpha/Mod"] },
      });
    });
    expect(onApplied).toHaveBeenCalled();
  });

  it("a sub-threshold pointer press does not reorder (click, not drag)", async () => {
    const bridge = makeBridge(["C:/m/Alpha/Mod", "C:/m/Alpha/Core"]);
    render(<LoadOrderDialog bridge={bridge} open onOpenChange={() => {}} onApplied={() => {}} />);
    await waitFor(() => screen.getByRole("list", { name: "Load order" }));
    const rows = within(screen.getByRole("list", { name: "Load order" })).getAllByRole("listitem");
    // Press + tiny move (< threshold) + release on the same row → no reorder.
    fireEvent.pointerDown(rows[0], { button: 0, pointerId: 1, clientY: 13 });
    fireEvent.pointerMove(rows[0], { pointerId: 1, clientY: 15 });
    fireEvent.pointerUp(rows[0], { pointerId: 1, clientY: 15 });
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/set-layers",
        params: { paths: ["C:/m/Alpha/Mod", "C:/m/Alpha/Core"] },
      });
    });
  });

  // A drag interrupted by the modal closing (Esc/overlay) must tear down the
  // gesture's document listeners + rAF loop — not leave a zombie loop running.
  it("tears down an in-flight drag when the modal closes", async () => {
    const bridge = makeBridge(["C:/m/Alpha/Mod", "C:/m/Alpha/Core"]);
    const cancelRaf = vi.spyOn(window, "cancelAnimationFrame");
    const removeListener = vi.spyOn(document, "removeEventListener");
    const { rerender } = render(<LoadOrderDialog bridge={bridge} open onOpenChange={() => {}} onApplied={() => {}} />);
    await waitFor(() => screen.getByRole("list", { name: "Load order" }));
    const rows = within(screen.getByRole("list", { name: "Load order" })).getAllByRole("listitem");
    const rect = (top: number): DOMRect =>
      ({ top, height: 26, bottom: top + 26, left: 0, right: 0, width: 0, x: 0, y: top, toJSON: () => ({}) }) as DOMRect;
    rows[0].getBoundingClientRect = () => rect(0);
    rows[1].getBoundingClientRect = () => rect(26);
    fireEvent.pointerDown(rows[0], { button: 0, pointerId: 1, clientY: 13 });
    fireEvent.pointerMove(rows[0], { pointerId: 1, clientY: 50 }); // activate → rAF + document listeners
    // Close the modal mid-drag — the open-effect must abort the live gesture.
    rerender(<LoadOrderDialog bridge={bridge} open={false} onOpenChange={() => {}} onApplied={() => {}} />);
    expect(cancelRaf).toHaveBeenCalled();
    expect(removeListener).toHaveBeenCalledWith("pointermove", expect.any(Function));
    cancelRaf.mockRestore();
    removeListener.mockRestore();
  });
});
