// Vitest unit tests for the EmitterTree sidebar. Verifies the
// fixture tree renders 3 roots with their
// children at the right indentation and that clicking a row fires
// emitters/select with the row's id.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor, act } from "@testing-library/react";
import type { ReactElement } from "react";
import * as Tooltip from "@radix-ui/react-tooltip";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import type { Bridge, EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { EmitterTree } from "../EmitterTree";
import { MockBridge } from "@/bridge/mock";
import { useMockEmitterProperties } from "@/bridge/mock-state";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";
import { useEmitterTreeStore } from "@/lib/emitter-tree";
import { useDeleteConfirmStore, requestDeleteEmitters } from "@/lib/delete-emitters";
import { writeOverloadGuard } from "@/lib/overload-guard";

// EmitterTree mounts Tips (Radix Tooltip.Root) on the per-row eye
// toggles and the footer toolbar, which requires the Tooltip.Provider that
// App.tsx supplies in production — this helper stands in for it here.
const renderWithTooltips = (ui: ReactElement) =>
  render(<Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{ui}</Tooltip.Provider>);

function fixtureTree(): EmitterTreeDto {
  return {
    root: {
      id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
      children: [
        {
          id: 0, stableId: 100, name: "Smoke", role: "root", linkGroup: 1, visible: true, spawn: ZERO_SPAWN,
          children: [
            { id: 1, stableId: 101, name: "Smoke embers", role: "lifetime", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
            { id: 2, stableId: 102, name: "Smoke puff",   role: "death",    linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          ],
        },
        {
          id: 3, stableId: 103, name: "Sparks", role: "root", linkGroup: 1, visible: true, spawn: ZERO_SPAWN,
          children: [
            { id: 4, stableId: 104, name: "Spark trail", role: "lifetime", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          ],
        },
        {
          id: 5, stableId: 105, name: "Flash", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
          children: [],
        },
      ],
    },
  };
}

function makeStubBridge() {
  const tree = fixtureTree();
  const snapshot = { selectedEmitterId: null };
  return {
    request: vi.fn().mockImplementation((req: { kind: string; params?: unknown }) => {
      if (req.kind === "emitters/list") return Promise.resolve(tree);
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "emitters/select") return Promise.resolve({});
      if (req.kind === "emitters/drop") return Promise.resolve({ ok: true });
      if (req.kind === "emitters/reorder-many") {
        const ids = (req.params as { ids: number[] }).ids;
        return Promise.resolve({ ok: true, newIds: ids });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
}

beforeEach(() => {
  // The multi-select atom is module-scoped; reset between tests so
  // state mutations from prior cases don't leak.
  useEmitterSelectionStore.getState().clear();
});

/** Stub bridge whose served tree can be swapped and whose tree/changed
 *  subscription is capturable — for tests that simulate a HOST-side reorder
 *  (positional ids reshuffle, stableIds follow the emitters). */
function makeMutableTreeBridge(initial: EmitterTreeDto) {
  let tree = initial;
  const subs: Array<() => void> = [];
  const bridge = {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "emitters/list") return Promise.resolve(tree);
      if (req.kind === "engine/state/snapshot") {
        return Promise.resolve({ selectedEmitterId: null });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockImplementation((kind: string, cb: () => void) => {
      if (kind === "emitters/tree/changed") subs.push(cb);
      return () => {};
    }),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
  const pushTree = (next: EmitterTreeDto) => {
    tree = next;
    subs.forEach((cb) => cb());
  };
  return { bridge, pushTree };
}

describe("EmitterTree", () => {
  it("renders 3 root rows with their lifetime/death children", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);

    // Wait for the async emitters/list to resolve.
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    expect(screen.getByText("Sparks")).toBeInTheDocument();
    expect(screen.getByText("Flash")).toBeInTheDocument();
    // Smoke's children render.
    expect(screen.getByText("Smoke embers")).toBeInTheDocument();
    expect(screen.getByText("Smoke puff")).toBeInTheDocument();
    // Sparks' single lifetime child.
    expect(screen.getByText("Spark trail")).toBeInTheDocument();

    // Tree wrapper exists with the correct role.
    expect(screen.getByRole("tree", { name: "Emitters" })).toBeInTheDocument();

    // Six total emitter rows (treeitem each). Synthetic root is NOT
    // rendered as a row.
    const items = screen.getAllByRole("treeitem");
    expect(items).toHaveLength(6);
  });

  // ─── Reorder glide — stableId keying ─────────────────────────────

  it("a host-side reorder (positional ids reshuffled, stableIds follow) MOVES row elements instead of remounting them", async () => {
    // Two roots; the host reorders them: after the swap, positional ids are
    // reassigned by position (Beta is now id 0!) but each emitter keeps its
    // stableId. Rows are keyed by stableId, so the <li> elements must be the
    // SAME DOM nodes after the update — that element identity is what lets
    // the FLIP pass glide them. (Keying by the positional id would remount:
    // brand-new elements, snap instead of glide — the pre-glide behavior.)
    const before: EmitterTreeDto = {
      root: {
        id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
        children: [
          { id: 0, stableId: 501, name: "Alpha", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          { id: 1, stableId: 502, name: "Beta",  role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
        ],
      },
    };
    const after: EmitterTreeDto = {
      root: {
        id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
        children: [
          { id: 0, stableId: 502, name: "Beta",  role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
          { id: 1, stableId: 501, name: "Alpha", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [] },
        ],
      },
    };
    const { bridge, pushTree } = makeMutableTreeBridge(before);
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Alpha")).toBeInTheDocument());

    const alphaLi = screen.getByText("Alpha").closest("li")!;
    const betaLi  = screen.getByText("Beta").closest("li")!;
    expect(alphaLi.getAttribute("data-stable-id")).toBe("501");

    pushTree(after);
    await waitFor(() => {
      const rows = [...document.querySelectorAll("li[data-stable-id]")];
      expect(rows.map((r) => r.getAttribute("data-stable-id"))).toEqual(["502", "501"]);
    });

    // Same DOM elements, new order — moved, not remounted.
    expect(screen.getByText("Alpha").closest("li")).toBe(alphaLi);
    expect(screen.getByText("Beta").closest("li")).toBe(betaLi);
  });

  it("clicking a row fires emitters/select with the row's id", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke embers")).toBeInTheDocument();
    });

    // Click Smoke embers (id=1).
    fireEvent.click(screen.getByText("Smoke embers"));

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const selectCall = calls.find((c) => c.kind === "emitters/select");
    expect(selectCall).toBeDefined();
    expect(selectCall.params).toEqual({ id: 1 });
  });

  // ─── multi-select via Ctrl/Cmd + Shift modifiers ─────────────────

  it("Ctrl+click on an unselected row toggles it into the multi-selection (primary updates)", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Establish a primary by clicking Smoke (id=0) first.
    fireEvent.click(screen.getByText("Smoke"));
    // Ctrl+click Sparks (id=3) — should add it without dropping Smoke.
    fireEvent.click(screen.getByText("Sparks"), { ctrlKey: true });

    const sel = useEmitterSelectionStore.getState();
    expect(sel.ids).toEqual([0, 3]);
    expect(sel.primary).toBe(3);

    // data-selected-count is visible on the container.
    const tree = screen.getByTestId("emitter-tree");
    expect(tree.getAttribute("data-selected-count")).toBe("2");
    expect(tree.getAttribute("data-primary-id")).toBe("3");
  });

  it("right-click → Delete on a multi-selection deletes the WHOLE selection (regression)", async () => {
    localStorage.removeItem("alo:confirm-delete"); // confirm-before-delete on (default)
    useDeleteConfirmStore.setState({ pending: null });
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    // Multi-select Smoke(0) + the childless leaf Flash(5).
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Flash"), { ctrlKey: true });
    expect(useEmitterSelectionStore.getState().ids).toEqual([0, 5]);

    // Right-click the SELECTED leaf and choose Delete. Pre-fix this deleted
    // only Flash (a leaf → immediate, no confirm); post-fix it confirms the
    // whole selection because handleDelete uses resolveTargetIds().
    fireEvent.contextMenu(screen.getByText("Flash"));
    fireEvent.click(await screen.findByText("Delete"));

    const pending = useDeleteConfirmStore.getState().pending;
    expect(pending).not.toBeNull();
    expect([...(pending?.ids ?? [])].sort((a, b) => a - b)).toEqual([0, 5]);
  });

  it("the trash button deletes the WHOLE multi-selection (confirms)", async () => {
    localStorage.removeItem("alo:confirm-delete"); // confirm-before-delete on (default)
    useDeleteConfirmStore.setState({ pending: null });
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    // Multi-select Smoke(0) + the childless leaf Flash(5).
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Flash"), { ctrlKey: true });
    expect(useEmitterSelectionStore.getState().ids).toEqual([0, 5]);

    // Toolbar trash deletes the whole selection (pre-fix: only the primary
    // Flash, a leaf → immediate, no confirm).
    fireEvent.click(screen.getByLabelText("Delete emitter"));

    const pending = useDeleteConfirmStore.getState().pending;
    expect(pending).not.toBeNull();
    expect([...(pending?.ids ?? [])].sort((a, b) => a - b)).toEqual([0, 5]);
  });

  // ─── drag/drop reorder + reparent ────────────────────────────────

  /** Stub the row's getBoundingClientRect so the drop-zone math has a
   *  predictable rectangle. The component reads clientY relative to
   *  the rect; pass an explicit clientY in the event payload. */
  function stubRect(el: HTMLElement, top: number, height: number) {
    Object.defineProperty(el, "getBoundingClientRect", {
      configurable: true,
      writable: true,
      value: () => ({
        top,
        bottom: top + height,
        left: 0,
        right: 200,
        width: 200,
        height,
        x: 0,
        y: top,
        toJSON: () => "{}",
      }),
    });
  }

  /** Stub all six fixture rows as a contiguous 24px-tall stack, in DOM order.
   *  The drag controller snapshots row + root-block geometry at activation
   *  (the first threshold-crossing move) and resolves the drop purely from it
   *  — so the FULL geometry must be stubbed, and the drag's clientY maps
   *  directly to content space (jsdom's scroll container rect is all-zero).
   *  Returns the 24px row height so callers can target a row's thirds.
   *
   *  Rows:  Smoke[0,24) embers[24,48) puff[48,72) Sparks[72,96) trail[96,120) Flash[120,144)
   *  Root blocks (root + subtree): Smoke[0,72) Sparks[72,120) Flash[120,144); mids 36 / 96 / 132. */
  function stubAllRows(): number {
    const H = 24;
    [0, 1, 2, 3, 4, 5].forEach((id, i) => {
      const el = document.querySelector(`button[data-emitter-id="${id}"]`) as HTMLElement | null;
      if (el) stubRect(el, i * H, H);
    });
    return H;
  }

  it("single-drag reorder above a root fires reorder-many (selection follows) + shows the make-room gap", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });

    // Single-select + drag Flash (id=5, root idx 2). Pointer in Sparks' UPPER
    // third (Sparks row [72,96), upper third [72,80)): content y=75 → past
    // Smoke block mid (36), before Sparks mid (96) → gap 1 (before Sparks).
    // Single-root reorder commits via reorder-many (a size-1 block), so the
    // highlight follows the moved root.
    fireEvent.click(screen.getByText("Flash"));
    stubAllRows();
    const flashBtn = screen.getByText("Flash").closest("button")!;

    fireEvent.pointerDown(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(flashBtn, { pointerType: "mouse", clientX: 0, clientY: 75 });
    // The make-room gap (not the old 2px line) previews the drop.
    expect(screen.getByTestId("drop-gap-at-1")).toBeInTheDocument();

    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 75 });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const reorder = calls.find((c) => c.kind === "emitters/reorder-many");
    expect(reorder).toBeDefined();
    expect(reorder.params).toEqual({ ids: [5], rootIndex: 1 });
    // NOT the single-emitter emitters/drop path anymore.
    expect(calls.find((c) => c.kind === "emitters/drop")).toBeUndefined();
  });

  it("single-drag onto the middle third of a row fires emitters/drop reparent with auto-picked slot", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Flash")).toBeInTheDocument();
    });

    // Drag Flash (id=5) onto Sparks (id=3) middle third → reparent under
    // Sparks. Sparks has a lifetime child only, so the auto-pick is "death".
    // Sparks row is [72,96); middle third [80,88) → content y=84.
    fireEvent.click(screen.getByText("Flash"));
    stubAllRows();
    const flashBtn = screen.getByText("Flash").closest("button")!;

    fireEvent.pointerDown(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(flashBtn, { pointerType: "mouse", clientX: 0, clientY: 84 });
    // Reparent shows the onto-ring, never a make-room gap.
    expect(document.querySelector('[data-testid^="drop-gap-at-"]')).toBeNull();

    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 84 });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const dropCall = calls.find((c) => c.kind === "emitters/drop");
    expect(dropCall).toBeDefined();
    expect(dropCall.params).toEqual({
      mode: "reparent",
      id: 5,
      targetId: 3,
      slot: "death",
    });
    expect(calls.find((c) => c.kind === "emitters/reorder-many")).toBeUndefined();
  });

  it("leaving the onto zone clears the latched reparent — a release on the no-op footprint commits NOTHING", async () => {
    // Regression: once the onto ring was acquired, moving to a dead/no-op
    // zone left lastParams latched (setReorderGap's idempotence check can't
    // see onto state), so releasing over the block's own footprint silently
    // committed the stale reparent.
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Sparks")).toBeInTheDocument());
    stubAllRows();
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;

    // Drag Flash; hover Sparks' middle third (y=84 in [80,88)) → onto ring.
    fireEvent.pointerDown(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(flashBtn, { pointerType: "mouse", clientX: 0, clientY: 84 });
    expect(sparksBtn.className).toContain("ring-accent");

    // Move onto Flash's own footprint (y=135 → past all block mids → gap 3,
    // inside footprint [2,3] → no-op). Ring must clear; nothing latched.
    fireEvent.pointerMove(flashBtn, { pointerType: "mouse", clientX: 0, clientY: 135 });
    expect(sparksBtn.className).not.toContain("ring-accent");

    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 135 });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    expect(calls.find((c) => c.kind === "emitters/drop")).toBeUndefined();
    expect(calls.find((c) => c.kind === "emitters/reorder-many")).toBeUndefined();
  });

  // ─── cancel an in-progress reorder drag ──────────────────────────

  /** Start a single-root drag and leave it ACTIVE (past the threshold) without
   *  releasing, so the cancel paths can be exercised. clientY 75 (Sparks' upper
   *  third) resolves reorder gap 1, so any commit WOULD dispatch reorder-many. */
  function startActiveDrag(sourceBtn: HTMLElement) {
    fireEvent.pointerDown(sourceBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(sourceBtn, { pointerType: "mouse", clientX: 0, clientY: 75 });
  }

  /** Any drop/reorder commit (single drag now commits reorder via reorder-many,
   *  reparent via emitters/drop). */
  function commitCalls(bridge: ReturnType<typeof makeStubBridge>) {
    return (bridge.request as ReturnType<typeof vi.fn>).mock.calls
      .map((c) => c[0])
      .filter((c) => c.kind === "emitters/drop" || c.kind === "emitters/reorder-many");
  }

  it("Escape cancels an in-progress reorder drag without committing", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });
    stubAllRows();
    const flashBtn = screen.getByText("Flash").closest("button")!;

    startActiveDrag(flashBtn); // active, valid reorder (gap 1) pending
    expect(screen.getByTestId("drop-gap-at-1")).toBeInTheDocument();
    fireEvent.keyDown(document, { key: "Escape" });
    // A trailing pointerup (as a real release would deliver) must NOT commit.
    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 80 });

    expect(commitCalls(bridge)).toHaveLength(0);
  });

  it("right-click cancels an in-progress reorder drag and suppresses the menu", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });
    stubAllRows();
    const flashBtn = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;

    startActiveDrag(flashBtn);
    // A right-click during the drag cancels it. (Menu suppression — our
    // capture-phase handler stops the event reaching Radix — is verified live
    // in the browser; Radix preventDefaults contextmenu regardless, so a
    // defaultPrevented assertion here would be vacuous.)
    fireEvent.contextMenu(sparksBtn, { clientX: 0, clientY: 80 });
    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 80 });

    // The drag is cancelled, so no reorder is committed.
    expect(commitCalls(bridge)).toHaveLength(0);
  });

  it("a completed drag swallows the trailing click so the row isn't re-selected", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });
    stubAllRows();
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;

    // A real (threshold-crossing) single-root drag → reorder gap 1.
    fireEvent.pointerDown(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(flashBtn, { pointerType: "mouse", clientX: 0, clientY: 75 });
    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 75 });
    (bridge.request as ReturnType<typeof vi.fn>).mockClear();

    // In a browser, pointerup on the same element synthesises a click; that
    // click must NOT also fire row selection (the draggedRef suppression).
    fireEvent.click(flashBtn);
    const selectCalls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls
      .map((c) => c[0])
      .filter((c) => c.kind === "emitters/select");
    expect(selectCalls).toHaveLength(0);

    // A subsequent plain click selects normally (suppression is one-shot).
    fireEvent.click(sparksBtn);
    const selectAfter = (bridge.request as ReturnType<typeof vi.fn>).mock.calls
      .map((c) => c[0])
      .filter((c) => c.kind === "emitters/select");
    expect(selectAfter.length).toBeGreaterThan(0);
  });

  it("Shift+click on a downstream row selects the range from primary to clicked", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Primary = Smoke (id=0).
    fireEvent.click(screen.getByText("Smoke"));
    // Shift+click Spark trail (id=4). Tree order:
    //   Smoke(0), Smoke embers(1), Smoke puff(2), Sparks(3), Spark trail(4), Flash(5).
    fireEvent.click(screen.getByText("Spark trail"), { shiftKey: true });

    const sel = useEmitterSelectionStore.getState();
    expect(sel.ids).toEqual([0, 1, 2, 3, 4]);
    expect(sel.primary).toBe(4);
  });

  // ─── inline rename ───────────────────────────────────────────────

  it("F2 on the focused row enters inline rename mode (input renders with current name)", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Focus the Smoke row (id=0) via click.
    fireEvent.click(screen.getByText("Smoke"));
    // Fire F2 on the tree container (the keydown handler is attached
    // to the container, not the row).
    const tree = screen.getByTestId("emitter-tree");
    fireEvent.keyDown(tree, { key: "F2" });

    const input = screen.getByTestId("emitter-rename-input-0") as HTMLInputElement;
    expect(input).toBeInTheDocument();
    expect(input.value).toBe("Smoke");
  });

  it("Enter on the inline-rename input commits via emitters/rename", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.keyDown(screen.getByTestId("emitter-tree"), { key: "F2" });

    const input = screen.getByTestId("emitter-rename-input-0") as HTMLInputElement;
    fireEvent.change(input, { target: { value: "Smoke 2" } });
    fireEvent.keyDown(input, { key: "Enter" });

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const renameCall = calls.find((c) => c.kind === "emitters/rename");
    expect(renameCall).toBeDefined();
    expect(renameCall.params).toEqual({ id: 0, name: "Smoke 2" });
  });

  it("Esc on the inline-rename input cancels (no emitters/rename dispatched)", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.keyDown(screen.getByTestId("emitter-tree"), { key: "F2" });

    const input = screen.getByTestId("emitter-rename-input-0") as HTMLInputElement;
    fireEvent.change(input, { target: { value: "Smoke 2" } });
    fireEvent.keyDown(input, { key: "Escape" });

    // The input should be gone (cancel returns to label view).
    expect(screen.queryByTestId("emitter-rename-input-0")).toBeNull();
    // No rename request fired.
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    expect(calls.find((c) => c.kind === "emitters/rename")).toBeUndefined();
  });

  // ─── keyboard nav ────────────────────────────────────────────────

  it("ArrowDown on the tree shifts primary to the next row in flat order", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Click Smoke first (focus + primary = 0). Tree order:
    //   Smoke(0), Smoke embers(1), Smoke puff(2), Sparks(3), Spark trail(4), Flash(5).
    fireEvent.click(screen.getByText("Smoke"));
    const tree = screen.getByTestId("emitter-tree");
    // Stub the row button focus, since we don't actually need DOM
    // focus to change — the handler updates the React-side primary.
    fireEvent.keyDown(tree, { key: "ArrowDown" });
    expect(useEmitterSelectionStore.getState().primary).toBe(1);
  });

  // ─── Ctrl+C clipboard dispatch ───────────────────────────────────

  it("Ctrl+C on the tree dispatches emitters/copy with the current selection ids", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Select Smoke (id=0) + Sparks (id=3) via plain + Ctrl+click.
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Sparks"), { ctrlKey: true });
    const tree = screen.getByTestId("emitter-tree");
    fireEvent.keyDown(tree, { key: "c", ctrlKey: true });

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const copyCall = calls.find((c) => c.kind === "emitters/copy");
    expect(copyCall).toBeDefined();
    expect(copyCall.params).toEqual({ ids: [0, 3] });
  });

  // ─── per-row visibility eye button ───────────────────────────────

  it("each row renders a per-row visibility eye button", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // 6 emitter rows in the fixture → 6 eye buttons.
    expect(screen.getByTestId("emitter-vis-0")).toBeInTheDocument();  // Smoke
    expect(screen.getByTestId("emitter-vis-1")).toBeInTheDocument();  // Smoke embers
    expect(screen.getByTestId("emitter-vis-3")).toBeInTheDocument();  // Sparks
  });

  it("clicking the per-row eye dispatches emitters/set-visible with toggled state", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Smoke (id=0) is `visible: true` in the fixture → click should
    // dispatch { visible: false }.
    fireEvent.click(screen.getByTestId("emitter-vis-0"));

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setVis = calls.find((c) => c.kind === "emitters/set-visible");
    expect(setVis).toBeDefined();
    expect(setVis!.params).toEqual({ id: 0, visible: false });
  });

  it("clicking the per-row eye does NOT change selection", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Before click: selection is empty (beforeEach clears the store).
    expect(useEmitterSelectionStore.getState().primary).toBeNull();

    fireEvent.click(screen.getByTestId("emitter-vis-1"));  // Smoke embers

    // Selection still empty — no emitters/select dispatched as a side effect.
    expect(useEmitterSelectionStore.getState().primary).toBeNull();
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    expect(calls.find((c) => c.kind === "emitters/select")).toBeUndefined();
  });

  it("no element carries aria-label='Link group N' (badge + spine are aria-hidden)", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Badge and spine are both aria-hidden — nothing should match the
    // accessible pattern "Link group N".
    const labeled = screen.queryAllByLabelText(/^Link group \d+$/);
    expect(labeled).toHaveLength(0);
  });

  it("row uses a 3-column grid (eye / role-glyph / name) — visual reorder", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Each row's outer button carries the grid template via inline style.
    // Columns are [eye | role-glyph | name]: the glyph sits in col 2, the
    // name in col 3. The link dot no longer reserves its own column —
    // it's appended INLINE to the END of the name cell (reclaiming the old
    // 10px col 3) and is aria-hidden, so the a11y tree / goldens are
    // unchanged. DOM order stays [eye, name(+dot), role].
    const rowButton = screen.getByText("Smoke").closest("button")!;
    expect(rowButton.style.gridTemplateColumns).toBe("18px 18px 1fr");

    // The visibility toggle is the FIRST DOM child (auto-placed in column 1).
    expect(rowButton.firstElementChild).toBe(
      rowButton.querySelector('[data-testid="emitter-vis-0"]'),
    );
    // Root rows no longer render a role glyph; children show theirs
    // (lifetime ↻ / on-death ✕) on the right.
    expect(screen.queryByLabelText("root emitter")).toBeNull();
    // Fixture has 2 lifetime children (Smoke embers, Spark trail) + 1
    // death child (Smoke puff), each rendering its spawn-role glyph.
    expect(screen.getAllByLabelText("lifetime child")).toHaveLength(2);
    expect(screen.getAllByLabelText("death child")).toHaveLength(1);
  });

  // ─── toolbar moves below the tree, restyles to .tree-actions ──────

  it("toolbar renders AFTER the tree's <ul> in DOM order", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    const tree    = screen.getByRole("tree", { name: "Emitters" });
    const toolbar = screen.getByTestId("emitter-tree-toolbar");

    // The toolbar comes after the tree's <ul> in document order.
    const cmp = tree.compareDocumentPosition(toolbar);
    // DOCUMENT_POSITION_FOLLOWING === 4
    expect(cmp & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });

  it("toolbar uses the .tree-actions class for design-aligned chrome", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    const toolbar = screen.getByTestId("emitter-tree-toolbar");
    expect(toolbar.className).toContain("tree-actions");
  });

  it("toolbar no longer has the eye-toggle button (per-row eyes replace it)", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // The legacy eye-toggle button used aria-label "Toggle emitter visibility".
    expect(screen.queryByLabelText("Toggle emitter visibility")).toBeNull();
  });

  it("toolbar renders a Duplicate button between New and Delete in DOM order", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    const newBtn = screen.getByLabelText("New Emitter");
    const dupBtn = screen.getByLabelText("Duplicate emitter");
    const delBtn = screen.getByLabelText("Delete emitter");

    // newBtn comes before dupBtn comes before delBtn.
    expect(newBtn.compareDocumentPosition(dupBtn) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(dupBtn.compareDocumentPosition(delBtn) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });

  it("clicking Duplicate dispatches emitters/duplicate-many with the selection", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Select Smoke (id=0) first.
    fireEvent.click(screen.getByText("Smoke"));
    await waitFor(() => {
      expect(useEmitterSelectionStore.getState().primary).toBe(0);
    });

    fireEvent.click(screen.getByLabelText("Duplicate emitter"));

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const dup = calls.find((c) => c.kind === "emitters/duplicate-many");
    expect(dup).toBeDefined();
    expect(dup!.params).toEqual({ ids: [0] });
  });

  it("Move Up/Down disabled state is selection-aware and symmetric at both edges", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());
    const up = () => screen.getByLabelText("Move emitter up") as HTMLButtonElement;
    const down = () => screen.getByLabelText("Move emitter down") as HTMLButtonElement;

    // Top root (Smoke) selected → can't move up, can move down.
    fireEvent.click(screen.getByText("Smoke"));
    expect(up().disabled).toBe(true);
    expect(down().disabled).toBe(false);

    // Bottom root (Flash) selected → can't move down, can move up.
    fireEvent.click(screen.getByText("Flash"));
    expect(down().disabled).toBe(true);
    expect(up().disabled).toBe(false);

    // Non-contiguous selection pinned at BOTH edges (Smoke + Flash) → both
    // disabled. Under the old primary-position logic this leaked the primary's
    // position and could leave one button wrongly enabled.
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Flash"), { ctrlKey: true });
    expect(up().disabled).toBe(true);
    expect(down().disabled).toBe(true);
  });

  it("Duplicate button is disabled when no emitter is selected", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // No selection at this point (beforeEach clears the store).
    const dupBtn = screen.getByLabelText("Duplicate emitter");
    expect(dupBtn).toBeDisabled();
  });

  it("Show All / Hide All render as icon buttons (no SHOW / HIDE text)", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Tooltips / aria-labels still find the buttons.
    expect(screen.getByLabelText("Show all emitters")).toBeInTheDocument();
    expect(screen.getByLabelText("Hide all emitters")).toBeInTheDocument();

    // The literal text "SHOW" and "HIDE" no longer appears in the toolbar.
    // (The legacy implementation rendered uppercase letter-spaced spans.)
    const toolbar = screen.getByTestId("emitter-tree-toolbar");
    expect(toolbar.textContent).not.toMatch(/SHOW/);
    expect(toolbar.textContent).not.toMatch(/HIDE/);

    // The Eye / EyeOff Lucide icons should be present inside the
    // Show All / Hide All buttons (svg elements).
    const showAll = screen.getByLabelText("Show all emitters");
    const hideAll = screen.getByLabelText("Hide all emitters");
    expect(showAll.querySelector("svg")).not.toBeNull();
    expect(hideAll.querySelector("svg")).not.toBeNull();
  });

  // ─── per-row link indicators (badge + spine) ─────────────────────

  it("link dot test ids no longer exist — emitter-link-dot-* is gone", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Old dot affordance fully removed.
    expect(screen.queryByTestId("emitter-link-dot-0")).toBeNull();
    expect(screen.queryByTestId("emitter-link-dot-3")).toBeNull();
    expect(screen.queryByTestId("emitter-link-dot-1")).toBeNull();
    expect(screen.queryByTestId("emitter-link-dot-5")).toBeNull();
  });

  it("linked row renders a badge with text G{n}; unlinked row renders no badge", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Smoke (id=0, linkGroup=1) → badge present with text "G1".
    const badge0 = screen.getByTestId("emitter-link-badge-0");
    expect(badge0).toBeInTheDocument();
    expect(badge0.textContent).toBe("G1");
    // Badge is decorative — aria-hidden keeps a11y goldens stable.
    expect(badge0.getAttribute("aria-hidden")).toBe("true");

    // Smoke embers (id=1, linkGroup=0) → no badge.
    expect(screen.queryByTestId("emitter-link-badge-1")).toBeNull();
  });

  it("linked row renders a spine hit-strip; unlinked row renders none", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Smoke (id=0, linkGroup=1) → spine present.
    expect(screen.getByTestId("emitter-link-spine-0")).toBeInTheDocument();
    // Sparks (id=3, linkGroup=1) → spine present.
    expect(screen.getByTestId("emitter-link-spine-3")).toBeInTheDocument();
    // Flash (id=5, linkGroup=0) → no spine.
    expect(screen.queryByTestId("emitter-link-spine-5")).toBeNull();
    // Smoke embers (id=1, linkGroup=0) → no spine.
    expect(screen.queryByTestId("emitter-link-spine-1")).toBeNull();
  });

  // ─── badge / spine click selects the whole group ─────────────────

  it("clicking the link badge selects all members of that group", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Pre-select Flash (id=5, ungrouped).
    fireEvent.click(screen.getByText("Flash"));
    expect(useEmitterSelectionStore.getState().ids).toEqual([5]);

    // Click the badge on Smoke (id=0, group 1) → selection = all group-1 members.
    fireEvent.click(screen.getByTestId("emitter-link-badge-0"));
    const sel = useEmitterSelectionStore.getState();
    expect([...sel.ids].sort((a, b) => a - b)).toEqual([0, 3]);
    expect(sel.primary).toBe(0);
    // Primary synced to the host.
    expect(
      bridge.request.mock.calls.some((c) => {
        const req = c[0] as { kind: string; params?: { id?: number } };
        return req.kind === "emitters/select" && req.params?.id === 0;
      }),
    ).toBe(true);
  });

  it("clicking the spine hit-strip selects all members of that group", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Pre-select Flash (id=5, ungrouped).
    fireEvent.click(screen.getByText("Flash"));
    expect(useEmitterSelectionStore.getState().ids).toEqual([5]);

    // Click the spine on Smoke (id=0, group 1) → selection = all group-1 members.
    fireEvent.click(screen.getByTestId("emitter-link-spine-0"));
    const sel = useEmitterSelectionStore.getState();
    expect([...sel.ids].sort((a, b) => a - b)).toEqual([0, 3]);
    expect(sel.primary).toBe(0);
    // Primary synced to the host.
    expect(
      bridge.request.mock.calls.some((c) => {
        const req = c[0] as { kind: string; params?: { id?: number } };
        return req.kind === "emitters/select" && req.params?.id === 0;
      }),
    ).toBe(true);
  });

  it("hovering a LINKED row tints its whole group (data-link-hover), cleared on leave", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    const rowFor = (id: number) =>
      document.querySelector(`[data-emitter-id="${id}"]`) as HTMLElement;

    // Hover Smoke (id=0, group 1) → both group-1 members tint.
    fireEvent.pointerEnter(rowFor(0));
    expect(rowFor(0).getAttribute("data-link-hover")).toBe("true");
    expect(rowFor(3).getAttribute("data-link-hover")).toBe("true");
    // A non-member row stays un-tinted.
    expect(rowFor(5).getAttribute("data-link-hover")).toBe("false");

    fireEvent.pointerLeave(rowFor(0));
    expect(rowFor(0).getAttribute("data-link-hover")).toBe("false");
  });

  it("hovering an UNLINKED row tints nothing", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Flash")).toBeInTheDocument();
    });
    const rowFor = (id: number) =>
      document.querySelector(`[data-emitter-id="${id}"]`) as HTMLElement;

    fireEvent.pointerEnter(rowFor(5)); // Flash, linkGroup 0
    expect(rowFor(5).getAttribute("data-link-hover")).toBe("false");
    expect(rowFor(0).getAttribute("data-link-hover")).toBe("false");
  });

  // ─── Dissolve link group ─────────────────────────────────────────

  it("Dissolve Link Group on a linked row unlinks every member at once", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Open the context menu on Smoke (id=0, group 1 with Sparks id=3).
    const smokeRow = document.querySelector('[data-emitter-id="0"]') as HTMLElement;
    fireEvent.contextMenu(smokeRow);

    const dissolve = await screen.findByText("Dissolve Link Group");
    fireEvent.click(dissolve);

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const sm = calls.find((c) => c.kind === "linkGroups/set-membership");
    expect(sm).toBeDefined();
    // Every group-1 member dissolved in one call, groupId null = unlink.
    expect(sm.params.ids.sort((a: number, b: number) => a - b)).toEqual([0, 3]);
    expect(sm.params.groupId).toBeNull();
  });

  it("Dissolve Link Group is disabled on an unlinked row", async () => {
    const bridge = makeStubBridge();
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Flash")).toBeInTheDocument();
    });

    // Flash (id=5) is unlinked (linkGroup 0).
    const flashRow = document.querySelector('[data-emitter-id="5"]') as HTMLElement;
    fireEvent.contextMenu(flashRow);

    const dissolve = await screen.findByText("Dissolve Link Group");
    expect(dissolve.closest("[role='menuitem']")?.getAttribute("data-disabled")).not.toBeNull();
  });
});

const node = (id: number, name: string, children: EmitterTreeNode[] = []) =>
  ({ id, name, role: "root", visible: true, spawn: ZERO_SPAWN, children } as unknown as EmitterTreeNode);

describe("EmitterTree delete gating (helper-level)", () => {
  beforeEach(() => {
    useEmitterTreeStore.setState({ tree: { root: node(-1, "root", [node(0, "a", [node(1, "a1")])]) } as unknown as EmitterTreeDto });
    useDeleteConfirmStore.setState({ pending: null });
    localStorage.clear();
  });
  it("deleting a parent opens the confirm", () => {
    const calls: number[] = [];
    const bridge = { request: (r: { kind: string; params: { id?: number } }) => { if (r.kind === "emitters/delete") calls.push(r.params.id!); return Promise.resolve({}); }, on: () => () => {} } as unknown as Bridge;
    requestDeleteEmitters(bridge, [0]);
    expect(calls).toEqual([]);
    expect(useDeleteConfirmStore.getState().pending?.ids).toEqual([0]);
  });
});

// ─── chain-load warning glyph ────────────────────────────────────────
//
// These render against the REAL MockBridge (not the stub) because the
// mock decorates tree payloads with live spawn values from the
// properties overlay (`decorateSpawn` in bridge/mock.ts) — patching the
// overlay then rendering exercises the same data path the browser-mode
// editor uses. Every render goes through renderWithTooltips
// (defined near the imports) because EmitterTree now mounts Tips
// unconditionally (per-row eye toggles + footer toolbar).
describe("chain-load warning glyph", () => {
  beforeEach(() => {
    // The overlay is module-scoped; reset so a patched spawn value can't
    // leak between tests.
    useMockEmitterProperties.getState().reset();
  });

  it("renders no glyph at fixture-default spawn values", async () => {
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Fixture defaults (10/s × 1-5 s lifetime) estimate far below the
    // 10,000 threshold — no row warns.
    expect(screen.queryAllByTestId(/^emitter-chain-warning-/)).toHaveLength(0);
  });

  it("shows the glyph with a breakdown tooltip when an emitter crosses the threshold", async () => {
    // Smoke is id 0 in the mock fixture; 20,000/s × 1 s = 20,000 > 10,000.
    useMockEmitterProperties.getState().patch(0, { nParticlesPerSecond: 20_000, lifetime: 1 });
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    const glyph = await screen.findByTestId("emitter-chain-warning-0");
    // the native `title` is gone (the rich Tip replaced it); the
    // aria-label now carries the FULL formatChainWarning breakdown.
    expect(glyph.getAttribute("title")).toBeNull();
    expect(glyph.getAttribute("aria-label")).toContain("20,000");
    expect(glyph.getAttribute("aria-label")).toContain("may spawn too many particles");
    // Only the offending chain glyphs: Smoke + its two children (the
    // cumulative product carries down), NOT the sane siblings Sparks (3)
    // and Flash (5). Pins the per-row prop wiring, not just presence.
    expect(screen.queryAllByTestId(/^emitter-chain-warning-/)).toHaveLength(3);
    expect(screen.queryByTestId("emitter-chain-warning-3")).toBeNull();
    expect(screen.queryByTestId("emitter-chain-warning-5")).toBeNull();
  });

  it("marks ancestors when a CHILD's edit makes the chain offend (multi-line tooltip)", async () => {
    // Root Smoke stays at fixture defaults (10/s × 1 s → E = 10); child
    // "Smoke embers" (id 1) patched to 2,000/s × 1 s → cumulative
    // 10 × 2,000 = 20,000 > 10,000. The root glyphs as an ancestor and
    // the child's tooltip carries the per-generation breakdown.
    useMockEmitterProperties.getState().patch(1, { nParticlesPerSecond: 2_000, lifetime: 1 });
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    const childGlyph = await screen.findByTestId("emitter-chain-warning-1");
    expect(childGlyph.getAttribute("aria-label")).toContain("→ Smoke embers");
    expect(screen.getByTestId("emitter-chain-warning-0")).toBeInTheDocument();
  });
});

// ── Cap-tracking glyph ──
// The glyph threshold follows the configurable guard cap when the guard
// is enabled, and falls back to the advisory 10k when disabled.
//
// Fixture math (all three tests below patch Smoke id 0 to 200/s × 1 s =
// 200 own particles). Chain load multiplies down each generation:
// A(child) = A(parent) × E(child) (see lib/chain-load.ts). Smoke's death
// child "Smoke puff" (id 2) drives the worst chain: it keeps its fixture
// defaults — nParticlesPerSecond 10 (makeFixtureProperties) × lifetime 3 s
// (lifetimeSeed = (|2| % 5) + 1 = 3) = E 30. So worst chain = 200 × 30 =
// 6,000. That 6,000 sits BELOW the fixed 10k advisory (no glyph when the
// guard is off / cap ≥ 10k) but the row's OWN 200 sits ABOVE a 100 cap
// (glyph fires when the guard is on at cap 100).
describe("chain-warning glyph tracks the configurable guard cap", () => {
  beforeEach(() => {
    useMockEmitterProperties.getState().reset();
    localStorage.clear();
  });

  it("fires at the guard cap, below the fixed 10k advisory", async () => {
    // 200/s × 1 s = 200 own particles: above cap 100, but the worst chain
    // product (200 × 30 = 6,000, see the describe-block fixture-math note)
    // is below the 10k advisory — so the glyph fires only because of the
    // guard cap, not the advisory threshold.
    useMockEmitterProperties.getState().patch(0, { nParticlesPerSecond: 200, lifetime: 1 });
    writeOverloadGuard({ enabled: true, maxParticles: 100 });
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    await screen.findByTestId("emitter-chain-warning-0");
  });

  it("falls back to the 10k advisory when the guard is disabled", async () => {
    // Same 200/s × 1 s = 200; chain max 6,000 (describe-block fixture-math
    // note) — well below the 10k advisory, so no glyph appears when the
    // guard is disabled.
    useMockEmitterProperties.getState().patch(0, { nParticlesPerSecond: 200, lifetime: 1 });
    writeOverloadGuard({ enabled: false, maxParticles: 100 });
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());
    expect(screen.queryAllByTestId(/^emitter-chain-warning-/)).toHaveLength(0);
  });

  it("reacts live to a cap change (Preferences edit, no reload)", async () => {
    // Start with cap 10,000 (chain max 6,000 < 10,000 → no glyph; see the
    // describe-block fixture-math note), then lower cap to 100 (own 200 >
    // 100 → glyph appears without a remount).
    useMockEmitterProperties.getState().patch(0, { nParticlesPerSecond: 200, lifetime: 1 });
    writeOverloadGuard({ enabled: true, maxParticles: 10_000 });
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());
    expect(screen.queryAllByTestId(/^emitter-chain-warning-/)).toHaveLength(0);
    act(() => {
      writeOverloadGuard({ enabled: true, maxParticles: 100 });
    });
    await screen.findByTestId("emitter-chain-warning-0");
  });

  it("mounts the system-load chip when the effect exceeds the cap (system view)", async () => {
    useMockEmitterProperties.getState().patch(0, { nParticlesPerSecond: 2_000, lifetime: 1 });
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    const chip = await screen.findByTestId("system-load-chip");
    expect(chip.textContent).toContain("preview limit");
    expect(chip.textContent).toContain("1,000"); // the configured cap, formatted
  });

  it("no chip at fixture-default spawn values (a11y-stability guard)", async () => {
    renderWithTooltips(<EmitterTree bridge={new MockBridge()} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
  });
});
