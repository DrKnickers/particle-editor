import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { PreferencesDialog } from "../PreferencesDialog";

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
});
