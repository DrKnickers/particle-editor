// Vitest unit tests for the SaveChangesPrompt modal.
//
// Phase 3 Screen 8 Batch 3. Three buttons: Save / Don't Save / Cancel.
// Open state is driven by `pendingAction` in the file-state atom — set
// it to a sentinel closure and assert the prompt renders + the right
// callback fires.
//
// / Radix-in-jsdom note: Modal uses Radix Dialog, which mounts
// into a portal. Buttons are reachable via `screen.getByRole("button",
// { name: ... })` thanks to the aria-label on each footer button.

import { describe, it, expect, beforeEach, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { SaveChangesPrompt } from "../SaveChangesPrompt";
import { useFileStateStore } from "@/lib/file-state";
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
});
