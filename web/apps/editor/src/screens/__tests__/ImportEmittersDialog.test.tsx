// Vitest tests for ImportEmittersDialog (Phase 3 Screen 8 Batch 4).
//
// Coverage:
//   1. Browse… fires file/open; OK starts disabled with the "Import"
//      placeholder.
//   2. Select all / Clear bulk controls.
//   3. Branch-select model on a nested tree: ticking a child makes the
//      parent indeterminate; clicking a partial/empty parent selects the
//      whole branch; clicking a full one clears it; collapse hides
//      children without dropping their selection.

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

/** Bridge whose Browse→preview resolves to a flat two-leaf tree. */
function makeTreeBridge(): Bridge & { request: ReturnType<typeof vi.fn> } {
  const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
  return makeBridgeFor(tree);
}

/** Bridge resolving to a nested tree: Parent(1)→Child(3), plus Sibling(2). */
function makeNestedBridge(): Bridge & { request: ReturnType<typeof vi.fn> } {
  const tree = node(0, "(root)", [
    node(1, "Parent", [node(3, "Child")]),
    node(2, "Sibling"),
  ]);
  return makeBridgeFor(tree);
}

function makeBridgeFor(tree: EmitterTreeNode): Bridge & { request: ReturnType<typeof vi.fn> } {
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "file/open") return Promise.resolve({ ok: true, path: "C:/x.alo" });
      if (req.kind === "emitters/preview-from-file") return Promise.resolve({ ok: true, tree });
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

/** Browse and wait for the preview tree to render (first leaf present). */
async function browseAndWait(label: string) {
  fireEvent.click(screen.getByRole("button", { name: /Browse/ }));
  await waitFor(() => expect(screen.getByLabelText(`Select ${label}`)).toBeInTheDocument());
}

const cb = (name: string) => screen.getByLabelText(`Select ${name}`) as HTMLInputElement;

/** Like makeTreeBridge, but import-from-file *resolves* `{ imported }`
 *  instead of throwing — exercising the native success path (which the
 *  mock can't, since it throws "not implemented"). Lets us assert the
 *  zero / partial / full outcomes. */
function makeImportBridge(imported: number): Bridge & { request: ReturnType<typeof vi.fn> } {
  const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "file/open") {
        return Promise.resolve({ ok: true, path: "C:/x.alo" });
      }
      if (req.kind === "emitters/preview-from-file") {
        return Promise.resolve({ ok: true, tree });
      }
      if (req.kind === "emitters/import-from-file") {
        return Promise.resolve({ imported });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

/** Render the dialog, load the preview tree, and select both emitters
 *  (picks.size === 2). Returns the OK button so the caller can click it. */
async function renderAndSelectAll(
  bridge: Bridge,
  onOpenChange: (open: boolean) => void,
): Promise<HTMLElement> {
  render(
    <ImportEmittersDialog bridge={bridge} open onOpenChange={onOpenChange} />,
  );
  fireEvent.click(screen.getByRole("button", { name: /Browse/ }));
  await waitFor(() =>
    expect(screen.getByLabelText("Select Alpha")).toBeInTheDocument(),
  );
  fireEvent.click(screen.getByRole("button", { name: /Select all emitters/ }));
  return screen.getByRole("button", { name: /Import 2 selected/ });
}

describe("ImportEmittersDialog", () => {
  it("Browse… click fires file/open", async () => {
    const bridge = makeStubBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    fireEvent.click(screen.getByRole("button", { name: /Browse/ }));

    await waitFor(() => {
      const calls = bridge.request.mock.calls.map((c) => c[0] as { kind: string });
      expect(calls.some((c) => c.kind === "file/open")).toBe(true);
    });
  });

  it("OK button is disabled when 0 emitters are selected", () => {
    const bridge = makeStubBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    // The OK button's accessible name is the dynamic "Import" label.
    const ok = screen.getByRole("button", { name: /^Import$/ });
    expect(ok).toBeDisabled();
  });

  it("Clear button deselects every emitter (legacy IDC_IMPORT_CLEAR)", async () => {
    const bridge = makeTreeBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Alpha");
    fireEvent.click(screen.getByRole("button", { name: /Select all emitters/ }));

    expect(cb("Alpha").checked).toBe(true);
    expect(cb("Beta").checked).toBe(true);

    fireEvent.click(screen.getByRole("button", { name: /Clear selection/ }));
    expect(cb("Alpha").checked).toBe(false);
    expect(cb("Beta").checked).toBe(false);
    expect(screen.getByRole("button", { name: /^Import$/ })).toBeDisabled();
  });

  it("Clear button is disabled when nothing is selected", async () => {
    const bridge = makeTreeBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Alpha");

    expect(screen.getByRole("button", { name: /Clear selection/ })).toBeDisabled();

    fireEvent.click(cb("Alpha"));
    expect(screen.getByRole("button", { name: /Clear selection/ })).not.toBeDisabled();
  });

  it("ticking a child makes the parent indeterminate (branch partial)", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(cb("Child"));

    expect(cb("Child").checked).toBe(true);
    expect(cb("Parent").checked).toBe(false);
    expect(cb("Parent").indeterminate).toBe(true);
    expect(screen.getByText(/1 of 3 selected/)).toBeInTheDocument();

    // A partial branch carries no "selected" row fill — only the dash.
    const parentRow = cb("Parent").closest("[data-selstate]");
    expect(parentRow?.getAttribute("data-selstate")).toBe("partial");
  });

  it("a fully-selected branch marks its rows data-selstate=all", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(cb("Child")); // parent partial …
    fireEvent.click(cb("Parent")); // … now fill the branch
    expect(cb("Parent").closest("[data-selstate]")?.getAttribute("data-selstate")).toBe("all");
    expect(cb("Child").closest("[data-selstate]")?.getAttribute("data-selstate")).toBe("all");
    expect(cb("Sibling").closest("[data-selstate]")?.getAttribute("data-selstate")).toBe("none");
  });

  it("clicking a partial parent selects its whole branch", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(cb("Child")); // → parent partial
    fireEvent.click(cb("Parent")); // → fill branch

    expect(cb("Parent").checked).toBe(true);
    expect(cb("Parent").indeterminate).toBe(false);
    expect(cb("Child").checked).toBe(true);
    expect(screen.getByText(/2 of 3 selected/)).toBeInTheDocument();
  });

  it("clicking a fully-selected parent clears its branch", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(cb("Parent")); // empty → select whole branch
    expect(cb("Parent").checked).toBe(true);
    expect(cb("Child").checked).toBe(true);

    fireEvent.click(cb("Parent")); // all → clear branch
    expect(cb("Parent").checked).toBe(false);
    expect(cb("Child").checked).toBe(false);
  });

  it("collapsing a parent hides its children but keeps their selection", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(cb("Child"));

    fireEvent.click(screen.getByRole("button", { name: "Collapse Parent" }));
    expect(screen.queryByLabelText("Select Child")).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole("button", { name: "Expand Parent" }));
    expect(cb("Child").checked).toBe(true);
  });

  it("renders a group of checkboxes, not a tree", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    expect(screen.getByRole("group", { name: "Emitters" })).toBeInTheDocument();
    expect(screen.queryByRole("tree")).not.toBeInTheDocument();
  });

  it("imported:0 keeps the modal open, shows an error, and keeps the tree", async () => {
    const bridge = makeImportBridge(0);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() =>
      expect(screen.getByText(/No emitters were imported/i)).toBeInTheDocument(),
    );
    expect(onOpenChange).not.toHaveBeenCalledWith(false);
    // The tree must remain so the user can re-pick and retry — the
    // message renders alongside it, not in its place.
    expect(screen.getByLabelText("Select Alpha")).toBeInTheDocument();
  });

  it("partial import keeps the modal open, shows a partial message, and keeps the tree", async () => {
    const bridge = makeImportBridge(1);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() =>
      expect(screen.getByText(/Imported 1 of 2/i)).toBeInTheDocument(),
    );
    expect(onOpenChange).not.toHaveBeenCalledWith(false);
    expect(screen.getByLabelText("Select Alpha")).toBeInTheDocument();
  });

  it("full import closes the modal", async () => {
    const bridge = makeImportBridge(2);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() => expect(onOpenChange).toHaveBeenCalledWith(false));
  });

  it("an over-count (imported > requested) is treated as full success", async () => {
    // The host can never return more than requested, but the `>=` guard
    // must close rather than mislabel an impossible over-count as partial.
    const bridge = makeImportBridge(3);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() => expect(onOpenChange).toHaveBeenCalledWith(false));
  });

  it("a rejected import keeps the modal open and surfaces the error", async () => {
    // Native rejects on every hard failure (no system, out-of-range
    // picks, unreadable file); the browser mock throws. Both land here.
    const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) => {
        if (req.kind === "file/open") {
          return Promise.resolve({ ok: true, path: "C:/x.alo" });
        }
        if (req.kind === "emitters/preview-from-file") {
          return Promise.resolve({ ok: true, tree });
        }
        if (req.kind === "emitters/import-from-file") {
          return Promise.reject(new Error("could not load file"));
        }
        return Promise.resolve({});
      }),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() =>
      expect(screen.getByText(/could not load file/i)).toBeInTheDocument(),
    );
    expect(onOpenChange).not.toHaveBeenCalledWith(false);
    expect(screen.getByLabelText("Select Alpha")).toBeInTheDocument();
  });

  it("ignores a second OK click while an import is in flight", async () => {
    // The native import has no timeout (slow disk read); a second click
    // before the first resolves would duplicate-import. Use a deferred
    // import promise so the request stays in flight across two clicks.
    const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
    let resolveImport: (v: { imported: number }) => void = () => {};
    const importPromise = new Promise<{ imported: number }>((res) => {
      resolveImport = res;
    });
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) => {
        if (req.kind === "file/open") {
          return Promise.resolve({ ok: true, path: "C:/x.alo" });
        }
        if (req.kind === "emitters/preview-from-file") {
          return Promise.resolve({ ok: true, tree });
        }
        if (req.kind === "emitters/import-from-file") {
          return importPromise;
        }
        return Promise.resolve({});
      }),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok); // first click — import now in flight
    fireEvent.click(ok); // second click — must be ignored

    const importCalls = bridge.request.mock.calls.filter(
      (c) => (c[0] as { kind: string }).kind === "emitters/import-from-file",
    );
    expect(importCalls).toHaveLength(1);

    // Let the in-flight import finish so the pending promise doesn't leak.
    resolveImport({ imported: 2 });
    await waitFor(() => expect(onOpenChange).toHaveBeenCalledWith(false));
  });
});
