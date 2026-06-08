// Vitest test for ModNicknameDialog (Phase 3 Screen 8 Batch 4).
//
// Coverage:
//   1. Renders the labelled text input; OK is disabled while empty,
//      and enables once the user types into it.

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { ModNicknameDialog } from "../ModNicknameDialog";
import { useModNicknameStore } from "@/lib/mod-nickname";

beforeEach(() => {
  // Reset the store so each test starts with the dialog closed.
  useModNicknameStore.setState({ open: false, resolver: null });
});

describe("ModNicknameDialog", () => {
  it("renders the text input and OK is disabled while empty, then enables on typing", () => {
    // Open the dialog by setting the atom directly (mirrors what
    // promptModNickname() does at runtime).
    useModNicknameStore.setState({ open: true, resolver: () => {} });

    render(<ModNicknameDialog />);

    const input = screen.getByLabelText("Mod nickname") as HTMLInputElement;
    expect(input).toBeInTheDocument();

    const ok = screen.getByRole("button", { name: /OK/ });
    expect(ok).toBeDisabled();

    fireEvent.change(input, { target: { value: "MyMod" } });
    expect(ok).not.toBeDisabled();
  });
});
