import { describe, it, expect, beforeEach } from "vitest";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { FileOpErrorModal } from "@/components/FileOpErrorModal";
import { useFileOpErrorStore } from "@/lib/file-op";

beforeEach(() => useFileOpErrorStore.setState({ message: null }));

describe("FileOpErrorModal", () => {
  it("is hidden when there is no message", () => {
    render(<FileOpErrorModal />);
    expect(screen.queryByText(/couldn't/i)).toBeNull();
  });

  it("shows the message and clears on OK", async () => {
    useFileOpErrorStore.setState({ message: "Couldn't save the file." });
    render(<FileOpErrorModal />);
    expect(screen.getByText("Couldn't save the file.")).toBeTruthy();
    await userEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(useFileOpErrorStore.getState().message).toBeNull();
  });
});
