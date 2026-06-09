import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { PreferencesDialog } from "../PreferencesDialog";
import { readConfirmDelete } from "@/lib/delete-emitters";

describe("PreferencesDialog", () => {
  beforeEach(() => localStorage.clear());
  it("renders a 3-way theme control", () => {
    render(<PreferencesDialog open onOpenChange={() => {}} />);
    expect(screen.getByRole("radio", { name: /dark/i })).toBeInTheDocument();
    expect(screen.getByRole("radio", { name: /light/i })).toBeInTheDocument();
    expect(screen.getByRole("radio", { name: /system/i })).toBeInTheDocument();
  });
  it("selecting Light applies + persists the mode", () => {
    render(<PreferencesDialog open onOpenChange={() => {}} />);
    fireEvent.click(screen.getByRole("radio", { name: /light/i }));
    expect(document.documentElement.dataset.theme).toBe("light");
    expect(localStorage.getItem("alo:theme")).toBe("light");
  });
  it("toggles and persists confirm-before-delete", async () => {
    localStorage.removeItem("alo:confirm-delete");
    render(<PreferencesDialog open onOpenChange={() => {}} />);
    const box = screen.getByLabelText("Confirm before deleting emitters") as HTMLInputElement;
    expect(box.checked).toBe(true);            // default on
    await userEvent.click(box);
    expect(box.checked).toBe(false);
    expect(readConfirmDelete()).toBe(false);   // persisted
  });
});
