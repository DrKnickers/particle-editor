// Vitest for SubmodsDialog — the reorderable submod-stack checklist.
// Covers: selected-first ordering on load, include/exclude, ↑/↓ reorder, and that
// Apply dispatches mods/set-submods with the checked names in precedence order.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { SubmodsDialog } from "../SubmodsDialog";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeBridge(opts: { submods: string[]; activeSubmods: string[] }) {
  const request = vi.fn().mockImplementation((req: { kind: string }) => {
    if (req.kind === "mods/list") {
      return Promise.resolve({
        mods: [],
        activePath: "C:/mock/Mods/X",
        submods: opts.submods,
        activeSubmods: opts.activeSubmods,
      });
    }
    return Promise.resolve({ ok: true, activeSubmods: [] });
  });
  return {
    request,
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

describe("SubmodsDialog", () => {
  it("lists selected submods first (in precedence order), then the rest", async () => {
    const bridge = makeBridge({ submods: ["Mod", "GCW", "Rev", "TR"], activeSubmods: ["GCW", "Mod"] });
    render(<SubmodsDialog bridge={bridge} open onOpenChange={() => {}} />);
    await waitFor(() => {
      expect(screen.getByRole("list", { name: "Submod load order" })).toBeTruthy();
    });
    const items = screen.getAllByRole("listitem").map((li) => li.textContent ?? "");
    expect(items[0]).toContain("GCW");   // selected, in order
    expect(items[1]).toContain("Mod");
    expect((screen.getByRole("checkbox", { name: "Include GCW" }) as HTMLInputElement).checked).toBe(true);
    expect((screen.getByRole("checkbox", { name: "Include Mod" }) as HTMLInputElement).checked).toBe(true);
    expect((screen.getByRole("checkbox", { name: "Include Rev" }) as HTMLInputElement).checked).toBe(false);
  });

  it("Apply dispatches mods/set-submods with the checked names in order", async () => {
    const bridge = makeBridge({ submods: ["Mod", "GCW", "Rev", "TR"], activeSubmods: ["Mod"] });
    render(<SubmodsDialog bridge={bridge} open onOpenChange={() => {}} />);
    await waitFor(() => screen.getByRole("checkbox", { name: "Include GCW" }));
    // Initial rows: [Mod✓, GCW, Rev, TR]. Add GCW to the stack.
    fireEvent.click(screen.getByRole("checkbox", { name: "Include GCW" }));
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/set-submods",
        params: { names: ["Mod", "GCW"] },
      });
    });
  });

  it("reordering with ↑ changes the precedence sent on Apply", async () => {
    const bridge = makeBridge({ submods: ["Mod", "GCW", "Rev", "TR"], activeSubmods: ["Mod", "GCW"] });
    render(<SubmodsDialog bridge={bridge} open onOpenChange={() => {}} />);
    await waitFor(() => screen.getByRole("button", { name: "Move GCW up" }));
    // Rows: [Mod✓, GCW✓, ...]. Move GCW above Mod.
    fireEvent.click(screen.getByRole("button", { name: "Move GCW up" }));
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/set-submods",
        params: { names: ["GCW", "Mod"] },
      });
    });
  });

  it("Cancel does not dispatch a set-submods", async () => {
    const bridge = makeBridge({ submods: ["Mod", "GCW"], activeSubmods: [] });
    render(<SubmodsDialog bridge={bridge} open onOpenChange={() => {}} />);
    await waitFor(() => screen.getByRole("checkbox", { name: "Include Mod" }));
    fireEvent.click(screen.getByRole("checkbox", { name: "Include Mod" }));
    fireEvent.click(screen.getByRole("button", { name: "Cancel" }));
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0].kind);
    expect(calls).not.toContain("mods/set-submods");
  });

  it("filters a stale active submod that is no longer discovered", async () => {
    // activeSubmods carries a name ("Ghost") absent from the discovered submods
    // (e.g. a persisted stack after a folder was removed). It must not render.
    const bridge = makeBridge({ submods: ["Mod", "GCW"], activeSubmods: ["GCW", "Ghost"] });
    render(<SubmodsDialog bridge={bridge} open onOpenChange={() => {}} />);
    await waitFor(() => screen.getByRole("checkbox", { name: "Include GCW" }));
    expect(screen.queryByText("Ghost")).toBeNull();
    expect(screen.queryByRole("checkbox", { name: "Include Ghost" })).toBeNull();
    // GCW is the only real selected submod and renders first, checked.
    const items = screen.getAllByRole("listitem").map((li) => li.textContent ?? "");
    expect(items[0]).toContain("GCW");
    // Apply sends only the surviving real name.
    fireEvent.click(screen.getByRole("button", { name: "Apply" }));
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "mods/set-submods",
        params: { names: ["GCW"] },
      });
    });
  });

  it("shows the empty state when the active mod has no submods", async () => {
    const bridge = makeBridge({ submods: [], activeSubmods: [] });
    render(<SubmodsDialog bridge={bridge} open onOpenChange={() => {}} />);
    await waitFor(() => {
      expect(screen.getByText("No submods found under the active mod.")).toBeTruthy();
    });
    expect(screen.queryByRole("list", { name: "Submod load order" })).toBeNull();
  });
});
