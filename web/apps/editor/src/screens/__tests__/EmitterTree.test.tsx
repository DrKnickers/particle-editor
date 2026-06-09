// Vitest unit tests for the EmitterTree sidebar (Phase 3 Screen 4
// Batch A). Verifies the fixture tree renders 3 roots with their
// children at the right indentation and that clicking a row fires
// emitters/select with the row's id.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import type { Bridge, EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { EmitterTree } from "../EmitterTree";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";
import { useEmitterTreeStore } from "@/lib/emitter-tree";
import { useDeleteConfirmStore, requestDeleteEmitters } from "@/lib/delete-emitters";

function fixtureTree(): EmitterTreeDto {
  return {
    root: {
      id: -1, name: "", role: "root", linkGroup: 0, visible: true,
      children: [
        {
          id: 0, name: "Smoke", role: "root", linkGroup: 1, visible: true,
          children: [
            { id: 1, name: "Smoke embers", role: "lifetime", linkGroup: 0, visible: true, children: [] },
            { id: 2, name: "Smoke puff",   role: "death",    linkGroup: 0, visible: true, children: [] },
          ],
        },
        {
          id: 3, name: "Sparks", role: "root", linkGroup: 1, visible: true,
          children: [
            { id: 4, name: "Spark trail", role: "lifetime", linkGroup: 0, visible: true, children: [] },
          ],
        },
        {
          id: 5, name: "Flash", role: "root", linkGroup: 0, visible: true,
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
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "emitters/list") return Promise.resolve(tree);
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "emitters/select") return Promise.resolve({});
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

describe("EmitterTree", () => {
  it("renders 3 root rows with their lifetime/death children", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);

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

  it("clicking a row fires emitters/select with the row's id", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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

  // ─── Batch B2 — multi-select via Ctrl/Cmd + Shift modifiers ──────

  it("Ctrl+click on an unselected row toggles it into the multi-selection (primary updates)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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

  // ─── Batch B3 — drag/drop reorder + reparent ─────────────────────

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

  /** Drive a pointer-based drag from `sourceBtn`, releasing over
   *  `targetBtn` at `clientY` within the target's stubbed rect.
   *
   *  The drag is owned by the parent's pointer-drag controller, whose
   *  move/up listeners live on `document`. pointermove/up dispatched on
   *  the target bubble there, and the controller reads the move event's
   *  target (its `[data-emitter-id]`) to find the hovered row. The
   *  pointerdown is at clientY 0; the first move (clientY≠0) crosses the
   *  drag threshold and resolves + stores the intent, which pointerup
   *  commits. (HTML5 DnD was replaced by pointer events because dragstart
   *  never fires under composition hosting.) */
  function pointerDrag(
    sourceBtn: HTMLElement,
    targetBtn: HTMLElement,
    clientY: number,
  ) {
    fireEvent.pointerDown(sourceBtn, { button: 0, clientX: 0, clientY: 0 });
    fireEvent.pointerMove(targetBtn, { button: 0, clientX: 0, clientY });
    fireEvent.pointerUp(targetBtn, { button: 0, clientX: 0, clientY });
  }

  it("dropping in the upper third of a root fires emitters/drop reorder above", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });

    // Drag Flash (id=5) onto Sparks (id=3) upper third → reorder ABOVE
    // Sparks (gap index = Sparks' position in roots = 1).
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(sparksBtn, 100, 30);  // y range [100, 130), thirds at 10

    pointerDrag(flashBtn, sparksBtn, 103);  // y=3 within row → reorder above

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const dropCall = calls.find((c) => c.kind === "emitters/drop");
    expect(dropCall).toBeDefined();
    expect(dropCall.params).toEqual({
      mode: "reorder",
      id: 5,
      rootIndex: 1,  // gap before Sparks (Sparks is at root idx 1)
    });
  });

  it("dropping in the middle third of a root fires emitters/drop reparent with auto-picked slot", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Flash")).toBeInTheDocument();
    });

    // Drag Flash (id=5) onto Sparks (id=3) middle third → reparent
    // under Sparks. Sparks has a lifetime child only, so the auto-pick
    // resolves to "death".
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(sparksBtn, 100, 30);  // middle third is [10, 20)

    pointerDrag(flashBtn, sparksBtn, 115);  // y=15 within row → reparent

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const dropCall = calls.find((c) => c.kind === "emitters/drop");
    expect(dropCall).toBeDefined();
    expect(dropCall.params).toEqual({
      mode: "reparent",
      id: 5,
      targetId: 3,
      slot: "death",
    });
  });

  // ─── — cancel an in-progress reorder drag ─────────────────

  /** Start a reorder drag and leave it ACTIVE (past the threshold) without
   *  releasing, so the cancel paths can be exercised. The pending intent is a
   *  valid reorder, so any commit would dispatch emitters/drop. */
  function startActiveDrag(
    sourceBtn: HTMLElement,
    targetBtn: HTMLElement,
    clientY: number,
  ) {
    fireEvent.pointerDown(sourceBtn, { button: 0, clientX: 0, clientY: 0 });
    fireEvent.pointerMove(targetBtn, { button: 0, clientX: 0, clientY });
  }

  function dropCalls(bridge: ReturnType<typeof makeStubBridge>) {
    return (bridge.request as ReturnType<typeof vi.fn>).mock.calls
      .map((c) => c[0])
      .filter((c) => c.kind === "emitters/drop");
  }

  it("Escape cancels an in-progress reorder drag without dropping", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });
    const flashBtn = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(sparksBtn, 100, 30);

    startActiveDrag(flashBtn, sparksBtn, 103); // active, valid reorder pending
    fireEvent.keyDown(document, { key: "Escape" });
    // A trailing pointerup (as a real release would deliver) must NOT drop.
    fireEvent.pointerUp(sparksBtn, { button: 0, clientX: 0, clientY: 103 });

    expect(dropCalls(bridge)).toHaveLength(0);
  });

  it("right-click cancels an in-progress reorder drag and suppresses the menu", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });
    const flashBtn = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(sparksBtn, 100, 30);

    startActiveDrag(flashBtn, sparksBtn, 103);
    // A right-click during the drag cancels it. (Menu suppression — our
    // capture-phase handler stops the event reaching Radix — is verified live
    // in the browser; Radix preventDefaults contextmenu regardless, so a
    // defaultPrevented assertion here would be vacuous.)
    fireEvent.contextMenu(sparksBtn, { clientX: 0, clientY: 103 });
    fireEvent.pointerUp(sparksBtn, { button: 0, clientX: 0, clientY: 103 });

    // The drag is cancelled, so no reorder is committed.
    expect(dropCalls(bridge)).toHaveLength(0);
  });

  it("a completed drag swallows the trailing click so the row isn't re-selected", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Sparks")).toBeInTheDocument();
    });
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(sparksBtn, 100, 30);

    pointerDrag(flashBtn, sparksBtn, 103);  // a real (threshold-crossing) drag
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
    render(<EmitterTree bridge={bridge} />);
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

  // ─── Batch C — link-group brackets ───────────────────────────────

  it("renders link-group brackets in the right gutter for grouped rows", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });
    // Fixture: Smoke (id=0) + Sparks (id=3) share linkGroup=1. Flash
    // (id=5) is unlinked. We expect exactly one bracket descriptor
    // (group 1) in the gutter.
    const gutter = screen.getByTestId("link-group-bracket-gutter");
    expect(gutter).toBeInTheDocument();
    const bracket1 = screen.getByTestId("link-group-bracket-1");
    expect(bracket1).toBeInTheDocument();
    expect(bracket1.getAttribute("data-link-group")).toBe("1");
    // No bracket for group 0 (unlinked) or any other group.
    expect(screen.queryByTestId("link-group-bracket-0")).toBeNull();
    expect(screen.queryByTestId("link-group-bracket-2")).toBeNull();
    // Per-member stubs: group 1 spans Smoke (flat row 0) + Sparks (flat
    // row 3); a stub is drawn at EACH member row, not just the ends.
    expect(screen.getByTestId("link-group-stub-1-0")).toBeInTheDocument();
    expect(screen.getByTestId("link-group-stub-1-3")).toBeInTheDocument();
    // No stub on a non-member row (Smoke embers at flat row 1).
    expect(screen.queryByTestId("link-group-stub-1-1")).toBeNull();
  });

  it("bracket layer is absolutely positioned to hug the names (no fixed gutter width)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // The gutter no longer reserves a fixed flex-column width; it's an
    // absolute layer positioned at (measured longest-name right + gap).
    // jsdom returns 0-size rects, so the measured right is 0 and left
    // collapses to the gap constant (BRACKET_NAME_GAP_PX = 16px,
    // widened from 8 so the bracket no longer hugs the name).
    const gutter = screen.getByTestId("link-group-bracket-gutter");
    expect(gutter).toBeInTheDocument();
    expect(gutter.className).toContain("absolute");
    expect(gutter.style.width).toBe("");      // no fixed width anymore
    expect(gutter.style.left).toBe("16px");   // hug position (gap only in jsdom)
  });

  it("rendered brackets carry a data-lane attribute matching their assigned lane", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Group 1 spans Smoke (row 0) → Sparks (row 3). Only one group
    // visible in the fixture → lane 0.
    const bracket = screen.getByTestId("link-group-bracket-1");
    expect(bracket).toHaveAttribute("data-lane", "0");
  });

  // ─── Batch C — inline rename ─────────────────────────────────────

  it("F2 on the focused row enters inline rename mode (input renders with current name)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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

  // ─── Batch C — keyboard nav ──────────────────────────────────────

  it("ArrowDown on the tree shifts primary to the next row in flat order", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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

  // ─── Batch C — Ctrl+C clipboard dispatch ─────────────────────────

  it("Ctrl+C on the tree dispatches emitters/copy with the current selection ids", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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

  // ─── Task 5 — per-row visibility eye button ──────────────────────

  it("each row renders a per-row visibility eye button", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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

  it("per-row link-group dot is no longer rendered (gutter brackets are the only affordance)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // The legacy per-row dot used `aria-label="Link group N"`. No
    // element should match that pattern any more.
    const dots = screen.queryAllByLabelText(/^Link group \d+$/);
    expect(dots).toHaveLength(0);
  });

  it("row uses a 3-column grid (eye / role-glyph / name) — visual reorder", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Each row's outer button carries the grid template via inline style.
    // Columns are [eye | role-glyph | link-dot | name]: the glyph sits in
    // col 2, the dot in col 3 (reserved on every row). DOM order stays
    // [eye, label, role, dot] (all re-placed visually via grid-column; the
    // dot is aria-hidden) so the a11y tree / goldens are unchanged — assert
    // that DOM order explicitly below.
    const rowButton = screen.getByText("Smoke").closest("button")!;
    expect(rowButton.style.gridTemplateColumns).toBe("18px 18px 10px 1fr");

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

  // ─── Task 7 — toolbar moves below the tree, restyles to .tree-actions ──

  it("toolbar renders AFTER the tree's <ul> in DOM order", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    const toolbar = screen.getByTestId("emitter-tree-toolbar");
    expect(toolbar.className).toContain("tree-actions");
  });

  it("toolbar no longer has the eye-toggle button (per-row eyes replace it)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // The legacy eye-toggle button used aria-label "Toggle emitter visibility".
    expect(screen.queryByLabelText("Toggle emitter visibility")).toBeNull();
  });

  it("toolbar renders a Duplicate button between New and Delete in DOM order", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // No selection at this point (beforeEach clears the store).
    const dupBtn = screen.getByLabelText("Duplicate emitter");
    expect(dupBtn).toBeDisabled();
  });

  it("Show All / Hide All render as icon buttons (no SHOW / HIDE text)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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

  // ─── P7 — per-row link dot ─────────────────────────────────

  it("renders a decorative link dot on rows whose linkGroup !== 0", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Smoke (id=0) and Sparks (id=3) are both in linkGroup 1.
    const dot0 = screen.getByTestId("emitter-link-dot-0");
    const dot3 = screen.getByTestId("emitter-link-dot-3");
    expect(dot0).toBeInTheDocument();
    expect(dot3).toBeInTheDocument();

    // The dot is decorative — it must NOT add to the accessible tree
    // (keeps the a11y goldens stable,).
    expect(dot0.getAttribute("aria-hidden")).toBe("true");
  });

  it("does NOT render a link dot on unlinked rows (linkGroup === 0)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke embers")).toBeInTheDocument();
    });

    // Smoke embers (id=1), Smoke puff (id=2), Spark trail (id=4), Flash
    // (id=5) all have linkGroup 0 → no dot.
    expect(screen.queryByTestId("emitter-link-dot-1")).toBeNull();
    expect(screen.queryByTestId("emitter-link-dot-5")).toBeNull();
  });

  // ─── P7 — bracket hover-tint (brackets are visual-only) ─────

  it("clicking the link-group bracket selects all members of that group", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Smoke")).toBeInTheDocument();
    });

    // Pre-select Flash (id=5, ungrouped) so we can see the selection change.
    fireEvent.click(screen.getByText("Flash"));
    expect(useEmitterSelectionStore.getState().ids).toEqual([5]);

    // Click the group-1 bracket → selection becomes ALL group-1 members
    // (Smoke id=0 and Sparks id=3), primary = the first (top-most) member.
    // The hit-zone has its own pointer-events (the gutter stays inert); a
    // real-browser check that it doesn't steal ROW clicks is done live
    // (jsdom has no hit-testing/z-order —).
    fireEvent.click(screen.getByTestId("link-group-bracket-1"));
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
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByText("Flash")).toBeInTheDocument();
    });
    const rowFor = (id: number) =>
      document.querySelector(`[data-emitter-id="${id}"]`) as HTMLElement;

    fireEvent.pointerEnter(rowFor(5)); // Flash, linkGroup 0
    expect(rowFor(5).getAttribute("data-link-hover")).toBe("false");
    expect(rowFor(0).getAttribute("data-link-hover")).toBe("false");
  });

  // ─── P7 — Dissolve link group ──────────────────────────────

  it("Dissolve Link Group on a linked row unlinks every member at once", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
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
    render(<EmitterTree bridge={bridge} />);
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
  ({ id, name, role: "root", visible: true, children } as unknown as EmitterTreeNode);

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
