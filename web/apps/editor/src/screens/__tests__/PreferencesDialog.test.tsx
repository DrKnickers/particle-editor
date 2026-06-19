import { describe, it, expect, beforeEach, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import type { Bridge, Request } from "@particle-editor/bridge-schema";
import { PreferencesDialog } from "../PreferencesDialog";
import { readConfirmDelete } from "@/lib/delete-emitters";

function makeBridgeStub(msaaLevels: number[] = [0, 2, 4]) {
  const request = vi.fn().mockImplementation((req: Request) => {
    if (req.kind === "engine/query/msaa-levels") {
      return Promise.resolve({ levels: msaaLevels, current: 4 });
    }
    return Promise.resolve({});
  });
  return { bridge: { request, on: vi.fn().mockReturnValue(() => {}) } as unknown as Bridge, request };
}

describe("PreferencesDialog", () => {
  beforeEach(() => localStorage.clear());
  it("renders a 3-way theme control", () => {
    render(<PreferencesDialog bridge={makeBridgeStub().bridge} open onOpenChange={() => {}} />);
    expect(screen.getByRole("radio", { name: /dark/i })).toBeInTheDocument();
    expect(screen.getByRole("radio", { name: /light/i })).toBeInTheDocument();
    expect(screen.getByRole("radio", { name: /system/i })).toBeInTheDocument();
  });
  it("selecting Light applies + persists the mode", () => {
    render(<PreferencesDialog bridge={makeBridgeStub().bridge} open onOpenChange={() => {}} />);
    fireEvent.click(screen.getByRole("radio", { name: /light/i }));
    expect(document.documentElement.dataset.theme).toBe("light");
    expect(localStorage.getItem("alo:theme")).toBe("light");
  });
  it("toggles and persists confirm-before-delete", async () => {
    localStorage.removeItem("alo:confirm-delete");
    render(<PreferencesDialog bridge={makeBridgeStub().bridge} open onOpenChange={() => {}} />);
    const box = screen.getByLabelText("Confirm before deleting emitters") as HTMLInputElement;
    expect(box.checked).toBe(true);            // default on
    await userEvent.click(box);
    expect(box.checked).toBe(false);
    expect(readConfirmDelete()).toBe(false);   // persisted
  });

  it("renders the preview guard controls (checkbox on, number enabled, no warning)", () => {
    const { bridge } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const box = screen.getByRole("checkbox", { name: /limit preview particle count/i });
    expect(box).toBeChecked();
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i });
    expect(num).toBeEnabled();
    expect((num as HTMLInputElement).value).toBe("10000");
    expect(screen.queryByText(/can crash the editor/i)).not.toBeInTheDocument();
  });

  it("unchecking sends enabled:false, persists, greys the number, shows the warning", () => {
    const { bridge, request } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    fireEvent.click(screen.getByRole("checkbox", { name: /limit preview particle count/i }));
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: false, maxParticles: 10_000 },
    });
    expect(JSON.parse(localStorage.getItem("alo:overload-guard")!)).toEqual({
      enabled: false,
      maxParticles: 10_000,
    });
    expect(screen.getByRole("spinbutton", { name: /max preview particles/i })).toBeDisabled();
    expect(screen.getByText(/can crash the editor/i)).toBeInTheDocument();
  });

  it("committing a new cap on blur clamps, persists, and sends", () => {
    const { bridge, request } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i });
    fireEvent.change(num, { target: { value: "50" } });
    fireEvent.blur(num);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: true, maxParticles: 1_000 },
    });
    expect((num as HTMLInputElement).value).toBe("1000");
  });

  it("Enter commits the cap too", () => {
    const { bridge, request } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i });
    fireEvent.change(num, { target: { value: "60000" } });
    fireEvent.keyDown(num, { key: "Enter" });
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: true, maxParticles: 60_000 },
    });
  });

  it("renders the Antialiasing select disabled while query is in-flight", () => {
    // The stub resolves on the next microtask — before that the select should be disabled.
    const { bridge } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = screen.getByRole("combobox", { name: /antialiasing/i });
    // Initially disabled (query not yet resolved)
    expect(sel).toBeDisabled();
  });

  it("populates Antialiasing options from the query result and defaults to saved level", async () => {
    localStorage.setItem("alo:msaa-quality", "2");
    const { bridge } = makeBridgeStub([0, 2, 4]);
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = await screen.findByRole("combobox", { name: /antialiasing/i });
    await waitFor(() => expect(sel).toBeEnabled());
    expect((sel as HTMLSelectElement).value).toBe("2");
    const options = Array.from((sel as HTMLSelectElement).options).map((o) => o.text);
    expect(options).toEqual(["Off", "2× MSAA", "4× MSAA"]);
  });

  it("selecting a different MSAA level persists and sends the bridge call", async () => {
    localStorage.setItem("alo:msaa-quality", "0");
    const { bridge, request } = makeBridgeStub([0, 2, 4]);
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = await screen.findByRole("combobox", { name: /antialiasing/i });
    await waitFor(() => expect(sel).toBeEnabled());
    fireEvent.change(sel, { target: { value: "2" } });
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/msaa-level",
      params: { level: 2 },
    });
    expect(localStorage.getItem("alo:msaa-quality")).toBe("2");
  });

  it("displays the engine's current level when saved level isn't offered, without writing localStorage or sending the bridge", async () => {
    // Saved 8× but the GPU only reports [0, 2, 4]; engine's current is 4.
    localStorage.setItem("alo:msaa-quality", "8");
    const { bridge, request } = makeBridgeStub([0, 2, 4]); // stub returns current: 4
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const sel = await screen.findByRole("combobox", { name: /antialiasing/i });
    await waitFor(() => expect(sel).toBeEnabled());
    // Should display the engine's authoritative current (4), not Off (0).
    expect((sel as HTMLSelectElement).value).toBe("4");
    // Must NOT have persisted or sent on mount — only onChange does that.
    expect(localStorage.getItem("alo:msaa-quality")).toBe("8"); // saved intent preserved
    expect(request).not.toHaveBeenCalledWith({
      kind: "engine/set/msaa-level",
      params: expect.anything(),
    });
  });

  it("query failure leaves the control showing the saved level without persisting or sending", async () => {
    localStorage.setItem("alo:msaa-quality", "4");
    // Bridge rejects the query entirely.
    const request = vi.fn().mockImplementation((req: Request) => {
      if (req.kind === "engine/query/msaa-levels") {
        return Promise.reject(new Error("bridge down"));
      }
      return Promise.resolve({});
    });
    const bridge = { request, on: vi.fn().mockReturnValue(() => {}) } as unknown as Bridge;
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    // The query will resolve (to the unknown fallback) on the next microtask.
    // Let it settle, then verify the control stayed as-is.
    await waitFor(() => {});
    const sel = screen.getByRole("combobox", { name: /antialiasing/i });
    // select stays disabled (msaaLevels never set) but shows saved value in the seed option.
    expect((sel as HTMLSelectElement).value).toBe("4");
    // No persist, no send.
    expect(localStorage.getItem("alo:msaa-quality")).toBe("4");
    expect(request).not.toHaveBeenCalledWith({
      kind: "engine/set/msaa-level",
      params: expect.anything(),
    });
  });

  it("Smooth skydome seams defaults on; unchecking sends enabled:false and persists", () => {
    const { bridge, request } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const box = screen.getByRole("checkbox", { name: /smooth skydome seams/i }) as HTMLInputElement;
    expect(box.checked).toBe(true); // default on
    fireEvent.click(box);
    expect(box.checked).toBe(false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/skydome-seam-fix",
      params: { enabled: false },
    });
    expect(localStorage.getItem("alo:skydome-seam-fix")).toBe("0");
  });

  it("Model shadows defaults on; unchecking sends enabled:false and persists", () => {
    const { bridge, request } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const box = screen.getByRole("checkbox", { name: /model shadows/i }) as HTMLInputElement;
    expect(box.checked).toBe(true); // default on
    fireEvent.click(box);
    expect(box.checked).toBe(false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/model-shadows",
      params: { enabled: false },
    });
    expect(localStorage.getItem("alo:model-shadows")).toBe("0");
  });

  it("Soft shadows defaults on; unchecking sends enabled:false and persists", () => {
    const { bridge, request } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const box = screen.getByRole("checkbox", { name: /soft shadows/i }) as HTMLInputElement;
    expect(box.checked).toBe(true); // default on
    fireEvent.click(box);
    expect(box.checked).toBe(false);
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/soft-shadows",
      params: { enabled: false },
    });
    expect(localStorage.getItem("alo:soft-shadows")).toBe("0");
  });

  // The Max-particles ▲/▼ steppers must step from the value VISIBLE in the
  // field (the uncommitted draft), not the last-committed guard value — else a
  // typed-but-unblurred edit is silently discarded (the s63 stepper regression).
  it("stepper ▲/▼ step from the visible draft, not the committed value", () => {
    const { bridge, request } = makeBridgeStub(); // default cap 10000
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const num = screen.getByRole("spinbutton", { name: /max preview particles/i }) as HTMLInputElement;
    // Type a new value but do NOT commit (no blur / Enter).
    fireEvent.change(num, { target: { value: "5000" } });
    // ▲ steps from the visible 5000 → 6000 (NOT from committed 10000 → 11000).
    fireEvent.click(screen.getByRole("button", { name: /increase max preview particles/i }));
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: true, maxParticles: 6_000 },
    });
    expect(num.value).toBe("6000");
    // ▼ steps back from the now-committed 6000 → 5000.
    fireEvent.click(screen.getByRole("button", { name: /decrease max preview particles/i }));
    expect(num.value).toBe("5000");
  });

  it("Soft shadows depends on Model shadows: disabled when off, stored pref not clobbered", () => {
    const { bridge } = makeBridgeStub(); // both default on
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    const soft = screen.getByRole("checkbox", { name: /soft shadows/i }) as HTMLInputElement;
    expect(soft).toBeEnabled();
    expect(soft.checked).toBe(true);
    // Turn Model shadows off — the Soft-shadows control disables…
    fireEvent.click(screen.getByRole("checkbox", { name: /model shadows/i }));
    expect(soft).toBeDisabled();
    // …but the parent toggle must never overwrite the stored soft preference.
    expect(localStorage.getItem("alo:soft-shadows")).not.toBe("0");
  });

});
