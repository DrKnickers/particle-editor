// Vitest render tests for the EmitterTree multi-selection drag preview
// (Tasks 6 + 7). With a multi-root selection, a pointer drag past the
// activation threshold over another root must:
//   - dispatch emitters/reorder-many on release (Task 6),
//   - render a make-room gap at the drop point + a cursor chip following the
//     pointer (Task 7 — preview D).
//
// The single-drag path (emitters/drop) is covered by EmitterTree.test.tsx;
// here we only exercise the additive multi-drag branch.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor, act } from "@testing-library/react";
import type { Bridge, EmitterTreeDto } from "@particle-editor/bridge-schema";
import { EmitterTree } from "../EmitterTree";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";

function fixtureTree(): EmitterTreeDto {
  return {
    root: {
      id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true,
      children: [
        {
          id: 0, stableId: 100, name: "Smoke", role: "root", linkGroup: 0, visible: true,
          children: [],
        },
        {
          id: 3, stableId: 103, name: "Sparks", role: "root", linkGroup: 0, visible: true,
          children: [],
        },
        {
          id: 5, stableId: 105, name: "Flash", role: "root", linkGroup: 0, visible: true,
          children: [],
        },
      ],
    },
  };
}

function makeStubBridge(tree: EmitterTreeDto = fixtureTree()) {
  const snapshot = { selectedEmitterId: null };
  return {
    request: vi.fn().mockImplementation((req: { kind: string; params?: unknown }) => {
      if (req.kind === "emitters/list") return Promise.resolve(tree);
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "emitters/select") return Promise.resolve({});
      // reorder-many resolves with the block's new contiguous indices so
      // applyNewSelection (in reorderManyEmitters) doesn't throw.
      if (req.kind === "emitters/reorder-many") {
        const ids = (req.params as { ids: number[] }).ids;
        return Promise.resolve({ ok: true, newIds: ids });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
}

/** Stub a row button's rect so the drop-zone third math is deterministic. */
function stubRect(el: HTMLElement, top: number, height: number) {
  Object.defineProperty(el, "getBoundingClientRect", {
    configurable: true,
    writable: true,
    value: () => ({
      top, bottom: top + height, left: 0, right: 200, width: 200, height,
      x: 0, y: top, toJSON: () => "{}",
    }),
  });
}

beforeEach(() => {
  useEmitterSelectionStore.getState().clear();
});

describe("EmitterTree multi-drag preview", () => {
  it("dragging a multi-root selection renders the destination band + cursor chip and commits reorder-many", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    // Multi-root selection: Smoke(0) + Flash(5) — two roots, non-contiguous.
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Flash"), { ctrlKey: true });
    expect(useEmitterSelectionStore.getState().ids).toEqual([0, 5]);

    // Stub the full row geometry — the multi-drag resolver snapshots every
    // root block's rect at activation and works in content space (the jsdom
    // scroll container's rect is all-zero, so content Y == stubbed rect Y).
    const smokeBtn  = screen.getByText("Smoke").closest("button")!;
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(smokeBtn, 0, 24);
    stubRect(sparksBtn, 24, 24);
    stubRect(flashBtn, 48, 24);

    // pointerdown at y=0, then a move to y=20 (past the 4px threshold):
    // block midpoints are 12/36/60, so y=20 is past Smoke's midpoint only →
    // gap index 1 (between Smoke and Sparks).
    fireEvent.pointerDown(flashBtn, { button: 0, clientX: 0, clientY: 0 });
    fireEvent.pointerMove(sparksBtn, { button: 0, clientX: 40, clientY: 20 });

    // Preview: cursor chip (vertical name list) + a make-room gap at the
    // resolved gap index.
    const chip = screen.getByTestId("drag-chip");
    expect(chip).toBeInTheDocument();
    expect(chip.textContent).toContain("Smoke");
    expect(chip.textContent).toContain("Flash");
    expect(screen.getByTestId("drop-gap-at-1")).toBeInTheDocument();
    // The single-drag insertion line must NOT render for a multi-drag.
    expect(screen.queryByTestId("drop-indicator-above-3")).toBeNull();

    // Release commits the block reorder via the reorder-many bridge message
    // (NOT emitters/drop, which is the single-drag path).
    fireEvent.pointerUp(sparksBtn, { button: 0, clientX: 40, clientY: 20 });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const reorder = calls.find((c) => c.kind === "emitters/reorder-many");
    expect(reorder).toBeDefined();
    expect(reorder.params).toEqual({ ids: [0, 5], rootIndex: 1 });
    expect(calls.find((c) => c.kind === "emitters/drop")).toBeUndefined();

    // The gap clears immediately; the chip enters its despawn flight (flying
    // toward the landing gap, data-exiting) and unmounts when it lands.
    expect(screen.queryByTestId("drop-gap-at-1")).toBeNull();
    expect(screen.getByTestId("drag-chip").getAttribute("data-exiting")).toBe("true");
    await waitFor(() => expect(screen.queryByTestId("drag-chip")).toBeNull());
  });

  it("a single-root drag also shows the make-room gap + chip and reorders via reorder-many", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    // Single selection of the dragged root — NOT a multi-drag, but now the
    // same gap+chip affordance and the selection-following reorder-many path.
    fireEvent.click(screen.getByText("Flash"));

    const smokeBtn  = screen.getByText("Smoke").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    stubRect(smokeBtn, 0, 24);
    stubRect(sparksBtn, 24, 24);
    stubRect(flashBtn, 48, 24);

    // y=26 is Sparks' upper third [24,32) → reorder gap 1 (before Sparks).
    fireEvent.pointerDown(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(flashBtn, { pointerType: "mouse", clientX: 40, clientY: 26 });

    // Single drag now gets the gap + a 1-name chip — no 2px insertion line.
    expect(screen.getByTestId("drop-gap-at-1")).toBeInTheDocument();
    expect(screen.getByTestId("drag-chip").textContent).toContain("Flash");
    expect(screen.queryByTestId("drop-indicator-above-3")).toBeNull();

    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 40, clientY: 26 });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const reorder = calls.find((c) => c.kind === "emitters/reorder-many");
    expect(reorder?.params).toEqual({ ids: [5], rootIndex: 1 });
    expect(calls.find((c) => c.kind === "emitters/drop")).toBeUndefined();
  });

  it("hovering the block's own footprint clears the gap and a release there is a no-op (no wire call)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    // Contiguous block: Smoke(0) + Sparks(3) at root indices 0,1 → noop gaps 0..2.
    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Sparks"), { ctrlKey: true });

    const smokeBtn  = screen.getByText("Smoke").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    stubRect(smokeBtn, 0, 24);
    stubRect(sparksBtn, 24, 24);
    stubRect(flashBtn, 48, 24);

    // y=30 → past mid 12 only → gap 1 → inside the footprint → no gap shown.
    fireEvent.pointerDown(smokeBtn, { button: 0, clientX: 0, clientY: 0 });
    fireEvent.pointerMove(smokeBtn, { button: 0, clientX: 40, clientY: 30 });

    expect(screen.getByTestId("drag-chip")).toBeInTheDocument();
    expect(document.querySelector('[data-testid^="drop-gap-at-"]')).toBeNull();

    fireEvent.pointerUp(smokeBtn, { button: 0, clientX: 40, clientY: 30 });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    expect(calls.find((c) => c.kind === "emitters/reorder-many")).toBeUndefined();
    expect(calls.find((c) => c.kind === "emitters/drop")).toBeUndefined();
  });

  it("dragging below every block resolves the END gap (after the whole list)", async () => {
    const bridge = makeStubBridge();
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Sparks"), { ctrlKey: true });

    const smokeBtn  = screen.getByText("Smoke").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    stubRect(smokeBtn, 0, 24);
    stubRect(sparksBtn, 24, 24);
    stubRect(flashBtn, 48, 24);

    // y=70 → past all midpoints (12/36/60) → gap 3 = N (end of list).
    fireEvent.pointerDown(smokeBtn, { button: 0, clientX: 0, clientY: 0 });
    fireEvent.pointerMove(flashBtn, { button: 0, clientX: 40, clientY: 70 });

    expect(screen.getByTestId("drop-gap-at-3")).toBeInTheDocument();

    fireEvent.pointerUp(flashBtn, { button: 0, clientX: 40, clientY: 70 });
    const reorder = (bridge.request as ReturnType<typeof vi.fn>).mock.calls
      .map((c) => c[0]).find((c) => c.kind === "emitters/reorder-many");
    expect(reorder.params).toEqual({ ids: [0, 3], rootIndex: 3 });
  });

  // --- Preview polish (session 32) ---

  /** Roots A(0, child A1=1), B(3), C(5) — for subtree-dim assertions. */
  function fixtureWithChildren(): EmitterTreeDto {
    return {
      root: {
        id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true,
        children: [
          {
            id: 0, stableId: 100, name: "Smoke", role: "root", linkGroup: 0, visible: true,
            children: [
              { id: 1, stableId: 101, name: "SmokeLife", role: "lifetime", linkGroup: 0, visible: true, children: [] },
            ],
          },
          { id: 3, stableId: 103, name: "Sparks", role: "root", linkGroup: 0, visible: true, children: [] },
          { id: 5, stableId: 105, name: "Flash", role: "root", linkGroup: 0, visible: true, children: [] },
        ],
      },
    };
  }

  it("a multi-drag dims the dragged roots' children too (whole subtree lifts)", async () => {
    const bridge = makeStubBridge(fixtureWithChildren());
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("SmokeLife")).toBeInTheDocument());

    fireEvent.click(screen.getByText("Smoke"));
    fireEvent.click(screen.getByText("Flash"), { ctrlKey: true });

    const smokeBtn  = screen.getByText("Smoke").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(sparksBtn, 100, 30);

    fireEvent.pointerDown(smokeBtn, { button: 0, clientX: 0, clientY: 0 });
    fireEvent.pointerMove(sparksBtn, { button: 0, clientX: 40, clientY: 103 });

    // Both selected roots AND Smoke's child row dim; the non-dragged root
    // doesn't. (Query rows by data-emitter-id — the chip duplicates names.)
    const row = (id: number) => document.querySelector(`button[data-emitter-id="${id}"]`)!;
    expect(row(0)).toHaveAttribute("data-dragging", "true");
    expect(row(1)).toHaveAttribute("data-dragging", "true");
    expect(row(5)).toHaveAttribute("data-dragging", "true");
    expect(row(3)).toHaveAttribute("data-dragging", "false");

    fireEvent.pointerUp(sparksBtn, { button: 0, clientX: 40, clientY: 103 });
  });

  it("a single drag dims the grabbed root's subtree too", async () => {
    const bridge = makeStubBridge(fixtureWithChildren());
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("SmokeLife")).toBeInTheDocument());

    const smokeBtn  = screen.getByText("Smoke").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    stubRect(sparksBtn, 100, 30);

    fireEvent.pointerDown(smokeBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(smokeBtn, { pointerType: "mouse", clientX: 0, clientY: 103 });

    // Query rows by id — the chip duplicates names, so getByText is ambiguous.
    const row = (id: number) => document.querySelector(`button[data-emitter-id="${id}"]`)!;
    expect(row(0)).toHaveAttribute("data-dragging", "true");   // Smoke (root)
    expect(row(1)).toHaveAttribute("data-dragging", "true");   // SmokeLife (child)
    expect(row(3)).toHaveAttribute("data-dragging", "false");  // Sparks (not dragged)

    fireEvent.pointerUp(smokeBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 103 });
  });

  it("cancels an in-flight drag when the host mutates the tree mid-gesture — no stale-id commit, no stale preview [audit A1/A2/A3]", async () => {
    // A structural change can land mid-drag: undo/redo/paste reach the app
    // accelerators focus-independently, or another pane mutates. The drag's
    // closures captured POSITIONAL ids + geometry at pointerdown, so the
    // gesture must abort on emitters/tree/changed rather than commit stale ids
    // (A1) or paint a stale gap/dim (A2/A3). This bridge records the
    // tree/changed subscriber so the test can fire it mid-gesture.
    const tree = fixtureTree();
    const handlers = new Map<string, (e: unknown) => void>();
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string; params?: unknown }) => {
        if (req.kind === "emitters/list") return Promise.resolve(tree);
        if (req.kind === "engine/state/snapshot") return Promise.resolve({ selectedEmitterId: null });
        if (req.kind === "emitters/reorder-many") {
          const ids = (req.params as { ids: number[] }).ids;
          return Promise.resolve({ ok: true, newIds: ids });
        }
        return Promise.resolve({});
      }),
      on: vi.fn().mockImplementation((kind: string, h: (e: unknown) => void) => {
        handlers.set(kind, h);
        return () => handlers.delete(kind);
      }),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };

    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    fireEvent.click(screen.getByText("Flash"));
    const smokeBtn  = screen.getByText("Smoke").closest("button")!;
    const sparksBtn = screen.getByText("Sparks").closest("button")!;
    const flashBtn  = screen.getByText("Flash").closest("button")!;
    stubRect(smokeBtn, 0, 24);
    stubRect(sparksBtn, 24, 24);
    stubRect(flashBtn, 48, 24);

    // Activate a drag: gap + chip + the dragged row dims.
    fireEvent.pointerDown(flashBtn, { button: 0, pointerType: "mouse", clientX: 0, clientY: 0 });
    fireEvent.pointerMove(flashBtn, { pointerType: "mouse", clientX: 40, clientY: 26 });
    expect(screen.getByTestId("drop-gap-at-1")).toBeInTheDocument();
    expect(document.querySelector('button[data-emitter-id="5"]')).toHaveAttribute("data-dragging", "true");

    // A structural mutation lands mid-drag (e.g. Ctrl+Z) → tree/changed fires.
    await act(async () => {
      handlers.get("emitters/tree/changed")?.({ payload: tree });
    });

    // The gesture is aborted: the make-room gap is gone (A2) and the dim is
    // cleared (A3) — no stale preview against the reshuffled tree.
    expect(screen.queryByTestId("drop-gap-at-1")).toBeNull();
    expect(document.querySelector('button[data-emitter-id="5"]')).toHaveAttribute("data-dragging", "false");

    // Releasing now commits NOTHING — no stale-id reorder/drop (A1).
    fireEvent.pointerUp(flashBtn, { button: 0, pointerType: "mouse", clientX: 40, clientY: 26 });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    expect(calls.find((c) => c.kind === "emitters/reorder-many")).toBeUndefined();
    expect(calls.find((c) => c.kind === "emitters/drop")).toBeUndefined();
  });

  it("the cursor chip caps at 4 names + a '+k more' line for big selections", async () => {
    const manyRoots: EmitterTreeDto = {
      root: {
        id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true,
        children: [0, 1, 2, 3, 4, 5].map((i) => ({
          id: i, stableId: 100 + i, name: `R${i}`, role: "root" as const, linkGroup: 0, visible: true, children: [],
        })),
      },
    };
    const bridge = makeStubBridge(manyRoots);
    render(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("R0")).toBeInTheDocument());

    fireEvent.click(screen.getByText("R0"));
    for (const n of ["R1", "R2", "R3", "R4", "R5"]) {
      fireEvent.click(screen.getByText(n), { ctrlKey: true });
    }

    const r0Btn = screen.getByText("R0").closest("button")!;
    const r5Btn = screen.getByText("R5").closest("button")!;
    stubRect(r5Btn, 100, 30);

    fireEvent.pointerDown(r0Btn, { button: 0, clientX: 0, clientY: 0 });
    fireEvent.pointerMove(r5Btn, { button: 0, clientX: 40, clientY: 103 });

    const chip = screen.getByTestId("drag-chip");
    expect(chip.textContent).toContain("R0");
    expect(chip.textContent).toContain("R3");
    expect(chip.textContent).not.toContain("R4");
    expect(chip.textContent).not.toContain("R5");
    expect(chip.textContent).toContain("+2 more");

    fireEvent.pointerUp(r5Btn, { button: 0, clientX: 40, clientY: 103 });
  });
});
