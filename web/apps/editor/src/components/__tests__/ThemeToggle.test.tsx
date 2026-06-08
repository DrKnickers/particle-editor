// Vitest unit tests for ThemeToggle:
//   - Renders Sun + Moon icon buttons.
//   - Clicking Sun sets dataset.theme to "light" and writes localStorage.
//   - Clicking Moon sets dataset.theme to "dark" and writes localStorage.
//   - Reads localStorage on mount and reflects stored value.

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { ThemeToggle } from "../ThemeToggle";

beforeEach(() => {
  localStorage.removeItem("alo:theme");
  document.documentElement.dataset.theme = "";
});

describe("ThemeToggle", () => {
  it("renders Sun and Moon buttons", () => {
    render(<ThemeToggle />);
    expect(screen.getByRole("button", { name: /light theme/i })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: /dark theme/i })).toBeInTheDocument();
  });

  it("clicking Light writes localStorage and sets dataset.theme", () => {
    render(<ThemeToggle />);
    fireEvent.click(screen.getByRole("button", { name: /light theme/i }));
    expect(localStorage.getItem("alo:theme")).toBe("light");
    expect(document.documentElement.dataset.theme).toBe("light");
  });

  it("clicking Dark writes localStorage and sets dataset.theme", () => {
    render(<ThemeToggle />);
    fireEvent.click(screen.getByRole("button", { name: /dark theme/i }));
    expect(localStorage.getItem("alo:theme")).toBe("dark");
    expect(document.documentElement.dataset.theme).toBe("dark");
  });

  it("reads localStorage on mount and reflects active theme via aria-pressed", () => {
    localStorage.setItem("alo:theme", "light");
    render(<ThemeToggle />);
    expect(screen.getByRole("button", { name: /light theme/i })).toHaveAttribute("aria-pressed", "true");
    expect(screen.getByRole("button", { name: /dark theme/i })).toHaveAttribute("aria-pressed", "false");
  });
});
