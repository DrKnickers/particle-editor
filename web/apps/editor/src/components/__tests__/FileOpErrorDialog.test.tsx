import { describe, it, expect, beforeEach } from "vitest";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { FileOpErrorDialog } from "@/components/FileOpErrorDialog";
import { useFileOpErrorStore } from "@/lib/file-op";
import { makeBridgeStub } from "@/test/bridge-stub";

// Only the dialog backdrop snapshot reaches the bridge, and only with a
// quadrant viewport mounted — a plain stub satisfies the prop.
const bridge = makeBridgeStub();

beforeEach(() => useFileOpErrorStore.setState({ message: null, title: null }));

describe("FileOpErrorDialog", () => {
  it("is hidden when there is no message", () => {
    render(<FileOpErrorDialog bridge={bridge} />);
    expect(screen.queryByText(/couldn't/i)).toBeNull();
  });

  it("shows the message and clears on OK", async () => {
    useFileOpErrorStore.setState({ message: "Couldn't save the file." });
    render(<FileOpErrorDialog bridge={bridge} />);
    expect(screen.getByText("Couldn't save the file.")).toBeTruthy();
    await userEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(useFileOpErrorStore.getState().message).toBeNull();
  });
});
