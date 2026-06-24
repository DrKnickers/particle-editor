import { render, screen, waitFor, fireEvent } from "@testing-library/react";
import { describe, it, expect } from "vitest";
import { MockBridge } from "@/bridge/mock";
import { ViewportToggleOverlay } from "@/components/ViewportToggleOverlay";

function setup() {
  const bridge = new MockBridge();
  render(<ViewportToggleOverlay bridge={bridge} />);
  return bridge;
}

describe("ViewportToggleOverlay", () => {
  it("renders the four toggles", async () => {
    setup();
    await waitFor(() =>
      expect(screen.getByRole("button", { name: /show ground/i })).toBeInTheDocument(),
    );
    expect(screen.getByRole("button", { name: /show grid/i })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: /toggle bloom/i })).toBeInTheDocument();
    // lock label is conditional; /lock/i matches both "Lock…" and "…to lock it"
    expect(screen.getByRole("button", { name: /lock/i })).toBeInTheDocument();
  });

  it("reflects ground state via aria-pressed and toggles it", async () => {
    setup();
    const btn = await screen.findByRole("button", { name: /show ground/i });
    const before = btn.getAttribute("aria-pressed");
    fireEvent.click(btn);
    await waitFor(() =>
      expect(
        screen.getByRole("button", { name: /show ground/i }).getAttribute("aria-pressed"),
      ).not.toBe(before),
    );
  });

  it("disables the lock when no reference object is loaded", async () => {
    setup();
    const lock = await screen.findByRole("button", { name: /lock/i });
    expect(lock).toBeDisabled();
  });

  it("enables + toggles the lock once a reference object is loaded", async () => {
    const bridge = setup();
    await bridge.request({ kind: "engine/set/reference-object", params: { name: "AT_ST_Walker" } });
    const lock = await screen.findByRole("button", { name: /lock/i });
    await waitFor(() => expect(lock).not.toBeDisabled());
    fireEvent.click(lock);
    await waitFor(() =>
      expect(screen.getByRole("button", { name: /lock/i }).getAttribute("aria-pressed")).toBe("true"),
    );
  });
});
