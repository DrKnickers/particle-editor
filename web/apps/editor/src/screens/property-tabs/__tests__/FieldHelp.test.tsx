import { describe, it, expect, vi } from "vitest";
import { render as rtlRender, screen, fireEvent } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import { FieldCheckbox, FieldSelect, FieldSpinner, EMIT_FROM_MESH_OPTIONS } from "../fields";

const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

describe("Field help", () => {
  it("describes the actual controls with unique persistent hidden nodes", () => {
    render(
      <>
        <FieldSpinner label="Lifetime:" help="First lifespan." value={1} onCommit={vi.fn()} />
        <FieldSpinner label="Lifetime:" help="Second lifespan." value={2} onCommit={vi.fn()} />
        <FieldCheckbox label="Linked" help="Follow the instance." checked={false} onCheckedChange={vi.fn()} />
        <FieldSelect label="Emit mode:" help="Select a mode." value={0} options={EMIT_FROM_MESH_OPTIONS} onCommit={vi.fn()} />
      </>,
    );
    const controls = [
      ...screen.getAllByRole("textbox", { name: "Lifetime:" }),
      screen.getByRole("checkbox", { name: "Linked" }),
      screen.getByRole("combobox", { name: "Emit mode:" }),
    ];
    const ids = controls.map((control) => control.getAttribute("aria-describedby"));
    expect(new Set(ids).size).toBe(4);
    for (const [index, help] of ["First lifespan.", "Second lifespan.", "Follow the instance.", "Select a mode."].entries()) {
      expect(ids[index]).toBeTruthy();
      const description = document.getElementById(ids[index]!);
      expect(description).toHaveAttribute("hidden");
      expect(description).toHaveTextContent(help);
    }
    expect(screen.queryByRole("tooltip")).toBeNull();
  });

  it("keeps fields without help free of descriptions and extra tab stops", () => {
    render(
      <>
        <FieldSpinner label="Number:" value={1} onCommit={vi.fn()} />
        <FieldCheckbox label="Flag" checked={false} onCheckedChange={vi.fn()} />
        <FieldSelect label="Mode:" value={0} options={EMIT_FROM_MESH_OPTIONS} onCommit={vi.fn()} />
      </>,
    );
    for (const label of ["Number:", "Flag", "Mode:"]) {
      expect(screen.getByLabelText(label)).not.toHaveAttribute("aria-describedby");
      expect(screen.getByText(label)).not.toHaveAttribute("tabindex");
    }
  });

  it("makes disabled reasons reachable by Tab without enabling or committing controls", async () => {
    const user = userEvent.setup();
    const onCommit = vi.fn();
    render(
      <>
        <FieldSpinner label="Number:" help="Number unavailable." value={1} disabled onCommit={onCommit} />
        <FieldCheckbox label="Flag" help="Flag unavailable." checked={false} disabled onCheckedChange={onCommit} />
        <FieldSelect label="Mode:" help="Mode unavailable." value={0} options={EMIT_FROM_MESH_OPTIONS} disabled onCommit={onCommit} />
      </>,
    );
    const labels = ["Number:", "Flag", "Mode:"].map((label) => screen.getByText(label));
    const controls = ["Number:", "Flag", "Mode:"].map((label) => screen.getByLabelText(label));
    for (const [index, label] of labels.entries()) {
      await user.tab();
      expect(label).toHaveFocus();
      expect(label).toHaveClass("lbl", "focus-ring");
      expect(label).toHaveAttribute("aria-describedby", controls[index]!.getAttribute("aria-describedby"));
      expect(controls[index]).toBeDisabled();
      await user.keyboard("{Enter} ");
    }
    expect(onCommit).not.toHaveBeenCalled();
    expect(await screen.findByRole("tooltip")).toHaveTextContent("Mode unavailable.");
  });

  it("preserves numeric and checkbox commits while help is present", async () => {
    const user = userEvent.setup();
    const onNumber = vi.fn();
    const onFlag = vi.fn();
    render(
      <>
        <FieldSpinner label="Minimum lifetime:" help="Percent of maximum." value={0.5} displayInvertedPercent onCommit={onNumber} />
        <FieldCheckbox label="Linked" help="Follow the instance." checked={false} onCheckedChange={onFlag} />
      </>,
    );
    const input = screen.getByRole("textbox", { name: "Minimum lifetime:" });
    fireEvent.focus(input);
    fireEvent.change(input, { target: { value: "30" } });
    fireEvent.blur(input);
    expect(onNumber).toHaveBeenCalledWith(0.7);
    await user.click(screen.getByRole("checkbox", { name: "Linked" }));
    expect(onFlag).toHaveBeenCalledWith(true);
  });
});
