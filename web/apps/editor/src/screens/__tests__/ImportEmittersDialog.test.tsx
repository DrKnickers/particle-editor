// Vitest tests for ImportEmittersDialog (Phase 3 Screen 8 Batch 4).
//
// Coverage:
//   1. Clicking "Browse…" fires `file/open`.
//   2. OK button starts disabled (no selection); rendered label is
//      the plain "Import" placeholder while picks.size is 0.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import type { Bridge, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { ImportEmittersDialog } from "../ImportEmittersDialog";

function makeStubBridge(): Bridge & {
  request: ReturnType<typeof vi.fn>;
  on: ReturnType<typeof vi.fn>;
} {
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      // Default: file/open returns ok:false so the test doesn't need
      // to mock the preview round-trip. Tests that need preview can
      // override the mock per-call.
      if (req.kind === "file/open") {
        return Promise.resolve({ ok: false, error: "browser-mode" });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
    on: ReturnType<typeof vi.fn>;
  };
}

function node(id: number, name: string, children: EmitterTreeNode[] = []): EmitterTreeNode {
  return { id, stableId: 100 + id, name, role: "root", linkGroup: -1, visible: true, spawn: ZERO_SPAWN, children };
}

/** A bridge whose Browse→preview round-trip resolves to a small tree
 *  (synthetic root id 0 with two selectable children), so selection
 *  buttons have something to act on. */
function makeTreeBridge(): Bridge & { request: ReturnType<typeof vi.fn> } {
  const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "file/open") {
        return Promise.resolve({ ok: true, path: "C:/x.alo" });
      }
      if (req.kind === "emitters/preview-from-file") {
        return Promise.resolve({ ok: true, tree });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

describe("ImportEmittersDialog", () => {
  it("Browse… click fires file/open", async () => {
    const bridge = makeStubBridge();
    render(
      <ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />,
    );

    fireEvent.click(screen.getByRole("button", { name: /Browse/ }));

    await waitFor(() => {
      const calls = bridge.request.mock.calls.map((c) => c[0] as { kind: string });
      expect(calls.some((c) => c.kind === "file/open")).toBe(true);
    });
  });

  it("OK button is disabled when 0 emitters are selected", () => {
    const bridge = makeStubBridge();
    render(
      <ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />,
    );

    // The OK button's accessible name is the dynamic "Import" label.
    const ok = screen.getByRole("button", { name: /^Import$/ });
    expect(ok).toBeDisabled();
  });

  it("Clear button deselects every emitter (legacy IDC_IMPORT_CLEAR)", async () => {
    const bridge = makeTreeBridge();
    render(
      <ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />,
    );

    // Load the preview tree, then select everything.
    fireEvent.click(screen.getByRole("button", { name: /Browse/ }));
    await waitFor(() =>
      expect(screen.getByLabelText("Select Alpha")).toBeInTheDocument(),
    );
    fireEvent.click(screen.getByRole("button", { name: /Select all emitters/ }));

    const alpha = screen.getByLabelText("Select Alpha") as HTMLInputElement;
    const beta = screen.getByLabelText("Select Beta") as HTMLInputElement;
    expect(alpha.checked).toBe(true);
    expect(beta.checked).toBe(true);

    // Clear deselects all.
    fireEvent.click(screen.getByRole("button", { name: /Clear selection/ }));
    expect(alpha.checked).toBe(false);
    expect(beta.checked).toBe(false);
    expect(screen.getByRole("button", { name: /^Import$/ })).toBeDisabled();
  });

  it("Clear button is disabled when nothing is selected", async () => {
    const bridge = makeTreeBridge();
    render(
      <ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />,
    );

    fireEvent.click(screen.getByRole("button", { name: /Browse/ }));
    await waitFor(() =>
      expect(screen.getByLabelText("Select Alpha")).toBeInTheDocument(),
    );

    // Nothing ticked yet → Clear is disabled.
    expect(screen.getByRole("button", { name: /Clear selection/ })).toBeDisabled();

    // Tick one → Clear enables.
    fireEvent.click(screen.getByLabelText("Select Alpha"));
    expect(
      screen.getByRole("button", { name: /Clear selection/ }),
    ).not.toBeDisabled();
  });
});
