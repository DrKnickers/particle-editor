// Vitest unit tests for the SaveChangesPrompt modal.
//
// Three buttons: Save / Don't Save / Cancel.
// Open state is driven by `pendingAction` in the file-state atom — set
// it to a sentinel closure and assert the prompt renders + the right
// callback fires.
//
// Radix-in-jsdom note: Modal uses Radix Dialog, which mounts
// into a portal. Buttons are reachable via `screen.getByRole("button",
// { name: ... })` thanks to the aria-label on each footer button.

import { describe, it, expect, beforeEach, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { SaveChangesPrompt } from "../SaveChangesPrompt";
import { useFileStateStore } from "@/lib/file-state";
import { useFileOpErrorStore } from "@/lib/file-op";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeStubBridge(saveOk = true): Bridge & { request: ReturnType<typeof vi.fn> } {
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "file/save") {
        return Promise.resolve(
          saveOk
            ? { ok: true, path: "C:/tmp/test.alo" }
            : { ok: false, error: "user-cancelled" },
        );
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

beforeEach(() => {
  // Reset the file-state atom so leftovers from another test don't
  // surface as an already-open prompt.
  useFileStateStore.setState({
    currentFilePath: null,
    dirty: false,
    recentFiles: [],
    pendingAction: null,
  });
  useFileOpErrorStore.setState({ message: null, title: null });
});

describe("SaveChangesPrompt", () => {
  it("renders Save / Don't Save / Cancel buttons when pendingAction is set", () => {
    const bridge = makeStubBridge();
    // Seed the atom with a pending action — the prompt observes this
    // slot and renders the modal when it's non-null.
    useFileStateStore.getState().setPendingAction(() => {});
    render(<SaveChangesPrompt bridge={bridge} />);

    expect(screen.getByRole("button", { name: "Save" })).toBeTruthy();
    expect(screen.getByRole("button", { name: "Don't Save" })).toBeTruthy();
    expect(screen.getByRole("button", { name: "Cancel" })).toBeTruthy();
  });

  it("clicking Save fires file/save and then runs the pending action", async () => {
    const bridge = makeStubBridge(true);
    const action = vi.fn().mockResolvedValue(undefined);
    useFileStateStore.getState().setPendingAction(action);
    render(<SaveChangesPrompt bridge={bridge} />);

    const saveBtn = screen.getByRole("button", { name: "Save" });
    fireEvent.click(saveBtn);

    // file/save dispatched first
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith({
        kind: "file/save",
        params: {},
      });
    });
    // pending action ran after the successful save
    await waitFor(() => {
      expect(action).toHaveBeenCalled();
    });
    // pendingAction slot cleared
    expect(useFileStateStore.getState().pendingAction).toBeNull();
  });

  it("clicking Cancel clears the pending action without dispatching file/save", () => {
    const bridge = makeStubBridge();
    const action = vi.fn();
    useFileStateStore.getState().setPendingAction(action);
    render(<SaveChangesPrompt bridge={bridge} />);

    const cancelBtn = screen.getByRole("button", { name: "Cancel" });
    fireEvent.click(cancelBtn);

    expect(bridge.request).not.toHaveBeenCalled();
    expect(action).not.toHaveBeenCalled();
    expect(useFileStateStore.getState().pendingAction).toBeNull();
  });

  it("a FAILED save surfaces the error modal, keeps the prompt open, and does not run the pending action", async () => {
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) =>
        req.kind === "file/save"
          ? Promise.resolve({ ok: false, error: "C:/x.alo is read-only" })
          : Promise.resolve({}),
      ),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge;
    const action = vi.fn();
    useFileStateStore.getState().setPendingAction(action);
    render(<SaveChangesPrompt bridge={bridge} />);

    fireEvent.click(screen.getByRole("button", { name: "Save" }));

    // The failure is surfaced (not silently swallowed), the pending New/Open is
    // NOT run, and the prompt STAYS OPEN (pendingAction preserved) so the user can
    // retry / Don't Save / Cancel — the unsaved work survives (release-audit #11).
    await waitFor(() => {
      expect(useFileOpErrorStore.getState().message).toContain("read-only");
    });
    expect(action).not.toHaveBeenCalled();
    expect(useFileStateStore.getState().pendingAction).not.toBeNull();
  });

  it("a CANCELLED save stays silent (no error modal) and keeps the prompt open", async () => {
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) =>
        req.kind === "file/save"
          ? Promise.resolve({ ok: false, error: "user-cancelled" })
          : Promise.resolve({}),
      ),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge;
    const action = vi.fn();
    useFileStateStore.getState().setPendingAction(action);
    render(<SaveChangesPrompt bridge={bridge} />);

    fireEvent.click(screen.getByRole("button", { name: "Save" }));

    // A user-cancelled save is not an error and must not run the pending op; the
    // prompt stays open so the user can choose again (release-audit #11).
    await waitFor(() => {
      expect(bridge.request).toHaveBeenCalledWith(expect.objectContaining({ kind: "file/save" }));
    });
    expect(action).not.toHaveBeenCalled();
    expect(useFileOpErrorStore.getState().message).toBeNull(); // cancel is not an error
    expect(useFileStateStore.getState().pendingAction).not.toBeNull();
  });

  it("a REJECTED save surfaces the error, keeps the prompt open, and does not run the pending action (#11)", async () => {
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) =>
        req.kind === "file/save"
          ? Promise.reject(new Error("bridge offline"))
          : Promise.resolve({}),
      ),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge;
    const action = vi.fn();
    useFileStateStore.getState().setPendingAction(action);
    render(<SaveChangesPrompt bridge={bridge} />);

    fireEvent.click(screen.getByRole("button", { name: "Save" }));

    // A rejected (thrown) save is surfaced via the error store and the prompt
    // stays open — the destructive pending op must never run (release-audit #11).
    await waitFor(() => {
      expect(useFileOpErrorStore.getState().message).toContain("bridge offline");
    });
    expect(action).not.toHaveBeenCalled();
    expect(useFileStateStore.getState().pendingAction).not.toBeNull();
  });
});
