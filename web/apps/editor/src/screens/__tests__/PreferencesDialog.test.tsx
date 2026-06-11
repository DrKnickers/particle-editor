import { describe, it, expect, beforeEach, vi } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import type { Bridge } from "@particle-editor/bridge-schema";
import { PreferencesDialog } from "../PreferencesDialog";
import { readConfirmDelete } from "@/lib/delete-emitters";

function makeBridgeStub() {
  const request = vi.fn().mockResolvedValue({});
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
    expect((num as HTMLInputElement).value).toBe("15000");
    expect(screen.queryByText(/can crash the editor/i)).not.toBeInTheDocument();
  });

  it("unchecking sends enabled:false, persists, greys the number, shows the warning", () => {
    const { bridge, request } = makeBridgeStub();
    render(<PreferencesDialog bridge={bridge} open onOpenChange={() => {}} />);
    fireEvent.click(screen.getByRole("checkbox", { name: /limit preview particle count/i }));
    expect(request).toHaveBeenCalledWith({
      kind: "engine/set/overload-guard",
      params: { enabled: false, maxParticles: 15_000 },
    });
    expect(JSON.parse(localStorage.getItem("alo:overload-guard")!)).toEqual({
      enabled: false,
      maxParticles: 15_000,
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
});
