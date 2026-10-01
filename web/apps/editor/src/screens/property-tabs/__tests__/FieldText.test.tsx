// Vitest specs for FieldText — the commit-on-blur/Enter text field.
//
// Escape cancels (no commit); Enter commits. An external value change is
// shown while the user hasn't typed, but never clobbers an in-progress draft.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { FieldText } from "../fields";

describe("FieldText", () => {
  it("Escape cancels the edit without committing", async () => {
    const onCommit = vi.fn();
    render(<FieldText label="Name" value="Smoke" onCommit={onCommit} />);
    const input = screen.getByRole("textbox") as HTMLInputElement;
    await userEvent.click(input);
    await userEvent.type(input, "XYZ");
    await userEvent.keyboard("{Escape}");
    expect(onCommit).not.toHaveBeenCalled();
    expect(input.value).toBe("Smoke");
  });

  it("Enter commits the edited text", async () => {
    const onCommit = vi.fn();
    render(<FieldText label="Name" value="Smoke" onCommit={onCommit} />);
    const input = screen.getByRole("textbox");
    await userEvent.click(input);
    await userEvent.type(input, "XYZ");
    await userEvent.keyboard("{Enter}");
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith("SmokeXYZ");
  });

  it("external change while focused and untyped shows the new value and blur commits nothing", async () => {
    const onCommit = vi.fn();
    const { rerender } = render(<FieldText label="Name" value="Smoke" onCommit={onCommit} />);
    const input = screen.getByRole("textbox") as HTMLInputElement;
    await userEvent.click(input);
    rerender(<FieldText label="Name" value="Fire" onCommit={onCommit} />);
    expect(input.value).toBe("Fire");
    fireEvent.blur(input);
    expect(onCommit).not.toHaveBeenCalled();
    expect(input.value).toBe("Fire");
  });

  it("external change while typing keeps the draft, and blur commits the draft", async () => {
    const onCommit = vi.fn();
    const { rerender } = render(<FieldText label="Name" value="Smoke" onCommit={onCommit} />);
    const input = screen.getByRole("textbox") as HTMLInputElement;
    await userEvent.click(input);
    await userEvent.type(input, "XYZ");
    rerender(<FieldText label="Name" value="Fire" onCommit={onCommit} />);
    expect(input.value).toBe("SmokeXYZ");
    fireEvent.blur(input);
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith("SmokeXYZ");
  });
});
