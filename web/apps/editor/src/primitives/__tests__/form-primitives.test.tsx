// Vitest unit tests for the shared form primitives:
// SegmentedControl (radio-group keyboard model), Checkbox (native input +
// data-state mirror), IconButton (one label → aria-label + tooltip,
// tipWhenDisabled wrapper, toggle aria-pressed) and cn()'s knowledge of the
// text-2xs / 3xs / 4xs font-size tokens.

import { describe, it, expect, vi } from "vitest";
import { render as rtlRender, screen, fireEvent, act } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import * as Tooltip from "@radix-ui/react-tooltip";
import { useState, type ReactElement, type ReactNode } from "react";
import { cn } from "@/lib/utils";
import { SegmentedControl } from "../SegmentedControl";
import { Checkbox } from "../Checkbox";
import { IconButton } from "../IconButton";

// IconButton mounts a Tip (Radix Tooltip.Root), which requires the
// Tooltip.Provider App.tsx supplies in production — this wrapper stands in.
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

const MODES = [
  { value: "dark", label: "Dark" },
  { value: "light", label: "Light" },
  { value: "system", label: "System" },
] as const;
type Mode = (typeof MODES)[number]["value"];

function ThemeSwitch({ onChange }: { onChange?: (m: Mode) => void }) {
  const [mode, setMode] = useState<Mode>("dark");
  return (
    <SegmentedControl<Mode>
      aria-label="Theme"
      value={mode}
      onValueChange={(m) => { setMode(m); onChange?.(m); }}
      options={MODES}
    />
  );
}

describe("SegmentedControl", () => {
  it("is a radiogroup whose checked radio is the only Tab stop", () => {
    render(<ThemeSwitch />);
    expect(screen.getByRole("radiogroup", { name: "Theme" })).toBeInTheDocument();
    const radios = screen.getAllByRole("radio");
    expect(radios.map((r) => r.getAttribute("aria-checked"))).toEqual(["true", "false", "false"]);
    expect(radios.map((r) => r.tabIndex)).toEqual([0, -1, -1]);
  });

  it("arrow keys move AND select, wrapping; Home / End jump to the ends", () => {
    const onChange = vi.fn();
    render(<ThemeSwitch onChange={onChange} />);
    const dark = screen.getByRole("radio", { name: "Dark" });
    dark.focus();
    fireEvent.keyDown(dark, { key: "ArrowRight" });
    const light = screen.getByRole("radio", { name: "Light" });
    expect(light).toHaveAttribute("aria-checked", "true");
    expect(light).toHaveFocus();
    expect(light.tabIndex).toBe(0);
    fireEvent.keyDown(light, { key: "ArrowLeft" });
    fireEvent.keyDown(dark, { key: "ArrowLeft" }); // wraps to the last
    expect(screen.getByRole("radio", { name: "System" })).toHaveAttribute("aria-checked", "true");
    fireEvent.keyDown(screen.getByRole("radio", { name: "System" }), { key: "Home" });
    expect(dark).toHaveAttribute("aria-checked", "true");
    fireEvent.keyDown(dark, { key: "End" });
    expect(screen.getByRole("radio", { name: "System" })).toHaveFocus();
    expect(onChange.mock.calls.map((c) => c[0])).toEqual(["light", "dark", "system", "dark", "system"]);
  });

  it("click selects a segment", () => {
    render(<ThemeSwitch />);
    fireEvent.click(screen.getByRole("radio", { name: "Light" }));
    expect(screen.getByRole("radio", { name: "Light" })).toHaveAttribute("aria-checked", "true");
  });
});

describe("Checkbox", () => {
  it("is a native checkbox that Space toggles, mirroring data-state", async () => {
    const user = userEvent.setup();
    function Box() {
      const [on, setOn] = useState(false);
      return <Checkbox aria-label="Show ground" checked={on} onChange={(e) => setOn(e.target.checked)} />;
    }
    render(<Box />);
    const cb = screen.getByRole("checkbox", { name: "Show ground" });
    expect(cb.tagName).toBe("INPUT");
    expect(cb).toHaveAttribute("data-state", "unchecked");
    cb.focus();
    await user.keyboard(" ");
    expect(cb).toBeChecked();
    expect(cb).toHaveAttribute("data-state", "checked");
  });

  it("applies the shared .checkbox look and the md size", () => {
    render(<Checkbox aria-label="big" size="md" checked={false} onChange={() => {}} />);
    const cb = screen.getByRole("checkbox", { name: "big" });
    expect(cb).toHaveClass("checkbox", "checkbox-md");
  });
});

describe("IconButton", () => {
  it("one label feeds the accessible name and the tooltip", async () => {
    render(<IconButton label="Undo" onClick={() => {}}>U</IconButton>);
    const btn = screen.getByRole("button", { name: "Undo" });
    expect(btn).not.toHaveAttribute("title");
    act(() => btn.focus());
    expect(await screen.findByRole("tooltip")).toHaveTextContent("Undo");
  });

  it("tip={null} renders no tooltip; pressed sets aria-pressed + the toggle's accent", () => {
    render(<IconButton label="Snap" tip={null} variant="toggle" pressed>S</IconButton>);
    const btn = screen.getByRole("button", { name: "Snap" });
    expect(btn).toHaveAttribute("aria-pressed", "true");
    expect(btn).toHaveClass("border-accent", "bg-accent-soft");
    act(() => btn.focus());
    expect(screen.queryByRole("tooltip")).toBeNull();
  });

  it("tipWhenDisabled wraps the button in the same span whether enabled or not", () => {
    const { rerender } = render(
      <IconButton label="Delete" tipWhenDisabled disabled>D</IconButton>,
    );
    const wrapper = () => screen.getByRole("button", { name: "Delete" }).parentElement!;
    expect(wrapper().tagName).toBe("SPAN");
    rerender(<IconButton label="Delete" tipWhenDisabled>D</IconButton>);
    expect(wrapper().tagName).toBe("SPAN");
  });
});

describe("cn() and the font-size tokens", () => {
  it("treats text-2xs / 3xs / 4xs as font sizes, not colours", () => {
    // A colour utility must NOT evict the size token (stock tailwind-merge
    // would read `text-2xs` as an unknown text-* colour and drop one).
    expect(cn("text-2xs", "text-text-2")).toBe("text-2xs text-text-2");
    // …and a later size still wins over an earlier one.
    expect(cn("text-2xs", "text-xs")).toBe("text-xs");
    expect(cn("text-xs", "text-3xs")).toBe("text-3xs");
  });
});
