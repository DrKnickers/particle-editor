// Vitest tests for ImportEmittersDialog.
//
// The tree is a WAI-ARIA multi-select tree: rows are `role="treeitem"` with
// `aria-checked` ("true" / "false" / "mixed"), roving tabindex, and arrow-key
// navigation. Selection is asserted via aria-checked (not a nested input).

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
      // to mock the preview round-trip.
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
  return makeBridgeFor(node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]));
}

/** Bridge resolving to a nested tree: Parent(1)→Child(3), plus Sibling(2). */
function makeNestedBridge(): Bridge & { request: ReturnType<typeof vi.fn> } {
  return makeBridgeFor(
    node(0, "(root)", [node(1, "Parent", [node(3, "Child")]), node(2, "Sibling")]),
  );
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

// ── treeitem helpers ────────────────────────────────────────────────
/** A row by its emitter name (aria-label). */
const row = (name: string) => screen.getByRole("treeitem", { name });
/** aria-checked of a row: "true" | "false" | "mixed". */
const checked = (name: string) => row(name).getAttribute("aria-checked");
/** data-selstate of a row: "none" | "partial" | "all". */
const selstate = (name: string) => row(name).getAttribute("data-selstate");

async function browseAndWait(label: string) {
  fireEvent.click(screen.getByRole("button", { name: /Browse/ }));
  await waitFor(() => expect(screen.getByRole("treeitem", { name: label })).toBeInTheDocument());
}

/** Like makeTreeBridge, but import-from-file *resolves* `{ imported }`. */
function makeImportBridge(imported: number): Bridge & { request: ReturnType<typeof vi.fn> } {
  const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "file/open") return Promise.resolve({ ok: true, path: "C:/x.alo" });
      if (req.kind === "emitters/preview-from-file") return Promise.resolve({ ok: true, tree });
      if (req.kind === "emitters/import-from-file") return Promise.resolve({ imported });
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

/** Render the dialog, load the preview tree, select both emitters. Returns OK. */
async function renderAndSelectAll(
  bridge: Bridge,
  onOpenChange: (open: boolean) => void,
): Promise<HTMLElement> {
  render(<ImportEmittersDialog bridge={bridge} open onOpenChange={onOpenChange} />);
  fireEvent.click(screen.getByRole("button", { name: /Browse/ }));
  await waitFor(() => expect(screen.getByRole("treeitem", { name: "Alpha" })).toBeInTheDocument());
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
    expect(screen.getByRole("button", { name: /^Import$/ })).toBeDisabled();
  });

  it("Clear button deselects every emitter (legacy IDC_IMPORT_CLEAR)", async () => {
    const bridge = makeTreeBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Alpha");
    fireEvent.click(screen.getByRole("button", { name: /Select all emitters/ }));
    expect(checked("Alpha")).toBe("true");
    expect(checked("Beta")).toBe("true");

    fireEvent.click(screen.getByRole("button", { name: /Clear selection/ }));
    expect(checked("Alpha")).toBe("false");
    expect(checked("Beta")).toBe("false");
    expect(screen.getByRole("button", { name: /^Import$/ })).toBeDisabled();
  });

  it("Clear button is disabled when nothing is selected", async () => {
    const bridge = makeTreeBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Alpha");
    expect(screen.getByRole("button", { name: /Clear selection/ })).toBeDisabled();

    fireEvent.click(row("Alpha"));
    expect(screen.getByRole("button", { name: /Clear selection/ })).not.toBeDisabled();
  });

  it("ticking a child makes the parent mixed (branch partial)", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(row("Child"));

    expect(checked("Child")).toBe("true");
    expect(checked("Parent")).toBe("mixed");
    expect(screen.getByText(/1 of 3 selected/)).toBeInTheDocument();
    // A partial branch carries no "selected" row fill — only the dash.
    expect(selstate("Parent")).toBe("partial");
  });

  it("a fully-selected branch marks its rows data-selstate=all", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(row("Child")); // parent partial …
    fireEvent.click(row("Parent")); // … now fill the branch
    expect(selstate("Parent")).toBe("all");
    expect(selstate("Child")).toBe("all");
    expect(selstate("Sibling")).toBe("none");
  });

  it("clicking a partial parent selects its whole branch", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(row("Child")); // → parent partial
    fireEvent.click(row("Parent")); // → fill branch

    expect(checked("Parent")).toBe("true");
    expect(checked("Child")).toBe("true");
    expect(screen.getByText(/2 of 3 selected/)).toBeInTheDocument();
  });

  it("clicking a fully-selected parent clears its branch", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(row("Parent")); // empty → select whole branch
    expect(checked("Parent")).toBe("true");
    expect(checked("Child")).toBe("true");

    fireEvent.click(row("Parent")); // all → clear branch
    expect(checked("Parent")).toBe("false");
    expect(checked("Child")).toBe("false");
  });

  // ── keyboard (WAI-ARIA Tree View) ─────────────────────────────────
  it("renders a WAI-ARIA multi-select tree with one tab stop", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    const tree = screen.getByRole("tree", { name: "Emitters" });
    expect(tree).toHaveAttribute("aria-multiselectable", "true");
    // Exactly one tabbable treeitem (roving tabindex).
    const tabbable = screen.getAllByRole("treeitem").filter((el) => el.getAttribute("tabindex") === "0");
    expect(tabbable).toHaveLength(1);
  });

  it("ArrowDown / ArrowUp move roving focus over visible rows", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    row("Parent").focus();
    fireEvent.keyDown(row("Parent"), { key: "ArrowDown" });
    expect(row("Child")).toHaveFocus();
    fireEvent.keyDown(row("Child"), { key: "ArrowDown" });
    expect(row("Sibling")).toHaveFocus();
    fireEvent.keyDown(row("Sibling"), { key: "ArrowUp" });
    expect(row("Child")).toHaveFocus();
  });

  it("Home / End jump to the first / last visible row", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    row("Child").focus();
    fireEvent.keyDown(row("Child"), { key: "End" });
    expect(row("Sibling")).toHaveFocus();
    fireEvent.keyDown(row("Sibling"), { key: "Home" });
    expect(row("Parent")).toHaveFocus();
  });

  it("Space toggles selection of the focused row", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    row("Sibling").focus();
    fireEvent.keyDown(row("Sibling"), { key: " " });
    expect(checked("Sibling")).toBe("true");
    expect(row("Sibling")).toHaveFocus(); // focus stays put after toggling
    fireEvent.keyDown(row("Sibling"), { key: " " });
    expect(checked("Sibling")).toBe("false");
  });

  it("mouse-collapsing a parent while a child is focused keeps focus in the tree", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    // Reach the child via the keyboard (sets roving active + DOM focus).
    row("Parent").focus();
    fireEvent.keyDown(row("Parent"), { key: "ArrowDown" });
    expect(row("Child")).toHaveFocus();

    // Click the Parent's chevron (the only mouse collapse affordance).
    const chevron = row("Parent").querySelector('[data-testid="row-chevron"]') as HTMLElement;
    fireEvent.click(chevron);

    // The focused child unmounted; focus must move to the parent, not <body>.
    expect(screen.queryByRole("treeitem", { name: "Child" })).not.toBeInTheDocument();
    expect(row("Parent")).toHaveFocus();
  });

  it("ArrowLeft collapses a parent and steps to parent; ArrowRight expands", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    fireEvent.click(row("Child")); // select a child first

    // ArrowLeft on the (expanded) parent collapses it; the child is removed.
    fireEvent.keyDown(row("Parent"), { key: "ArrowLeft" });
    expect(row("Parent")).toHaveAttribute("aria-expanded", "false");
    expect(screen.queryByRole("treeitem", { name: "Child" })).not.toBeInTheDocument();

    // ArrowRight re-expands; the child returns with its selection intact.
    fireEvent.keyDown(row("Parent"), { key: "ArrowRight" });
    expect(row("Parent")).toHaveAttribute("aria-expanded", "true");
    expect(checked("Child")).toBe("true");
  });

  it("ArrowLeft on a child steps focus to its parent", async () => {
    const bridge = makeNestedBridge();
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);

    await browseAndWait("Parent");
    row("Child").focus();
    fireEvent.keyDown(row("Child"), { key: "ArrowLeft" });
    expect(row("Parent")).toHaveFocus();
  });

  it("a source with no emitters shows a notice, not an empty tree", async () => {
    const bridge = makeBridgeFor(node(0, "(root)", []));
    render(<ImportEmittersDialog bridge={bridge} open onOpenChange={() => {}} />);
    fireEvent.click(screen.getByRole("button", { name: /Browse/ }));

    await waitFor(() =>
      expect(screen.getByText(/No particle emitters in this file/i)).toBeInTheDocument(),
    );
    expect(screen.queryByRole("tree")).not.toBeInTheDocument();
    expect(screen.getByRole("button", { name: /^Import$/ })).toBeDisabled();
  });

  // ── import outcome (post-#348) ────────────────────────────────────
  it("imported:0 keeps the modal open, shows an error, and keeps the tree", async () => {
    const bridge = makeImportBridge(0);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() => expect(screen.getByText(/No emitters were imported/i)).toBeInTheDocument());
    expect(onOpenChange).not.toHaveBeenCalledWith(false);
    expect(screen.getByRole("treeitem", { name: "Alpha" })).toBeInTheDocument();
  });

  it("partial import keeps the modal open, names unreadable data, keeps the tree", async () => {
    const bridge = makeImportBridge(1);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() => expect(screen.getByText(/Imported 1 of 2/i)).toBeInTheDocument());
    expect(screen.getByText(/unreadable data/i)).toBeInTheDocument();
    expect(onOpenChange).not.toHaveBeenCalledWith(false);
    expect(screen.getByRole("treeitem", { name: "Alpha" })).toBeInTheDocument();
  });

  it("full import closes the modal", async () => {
    const bridge = makeImportBridge(2);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);
    fireEvent.click(ok);
    await waitFor(() => expect(onOpenChange).toHaveBeenCalledWith(false));
  });

  it("an over-count (imported > requested) is treated as full success", async () => {
    const bridge = makeImportBridge(3);
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);
    fireEvent.click(ok);
    await waitFor(() => expect(onOpenChange).toHaveBeenCalledWith(false));
  });

  it("a rejected import keeps the modal open and surfaces the error", async () => {
    const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) => {
        if (req.kind === "file/open") return Promise.resolve({ ok: true, path: "C:/x.alo" });
        if (req.kind === "emitters/preview-from-file") return Promise.resolve({ ok: true, tree });
        if (req.kind === "emitters/import-from-file") return Promise.reject(new Error("could not load file"));
        return Promise.resolve({});
      }),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
    const onOpenChange = vi.fn();
    const ok = await renderAndSelectAll(bridge, onOpenChange);

    fireEvent.click(ok);

    await waitFor(() => expect(screen.getByText(/could not load file/i)).toBeInTheDocument());
    expect(onOpenChange).not.toHaveBeenCalledWith(false);
    expect(screen.getByRole("treeitem", { name: "Alpha" })).toBeInTheDocument();
  });

  it("ignores a second OK click while an import is in flight", async () => {
    const tree = node(0, "(root)", [node(1, "Alpha"), node(2, "Beta")]);
    let resolveImport: (v: { imported: number }) => void = () => {};
    const importPromise = new Promise<{ imported: number }>((res) => { resolveImport = res; });
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) => {
        if (req.kind === "file/open") return Promise.resolve({ ok: true, path: "C:/x.alo" });
        if (req.kind === "emitters/preview-from-file") return Promise.resolve({ ok: true, tree });
        if (req.kind === "emitters/import-from-file") return importPromise;
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

    resolveImport({ imported: 2 });
    await waitFor(() => expect(onOpenChange).toHaveBeenCalledWith(false));
  });
});
