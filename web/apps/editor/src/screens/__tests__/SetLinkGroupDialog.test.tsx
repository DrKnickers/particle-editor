// Vitest unit test for the SetLinkGroupDialog (Screen 4 Batch B2).
// Verifies that opening the modal renders both radios, that "Join
// existing group" is disabled when no groups exist in the fetched
// tree, and that OK with the default "Create new" radio fires
// linkGroups/set-membership with the captured selection ids and
// groupId: -1 (the host-side sentinel for "pick smallest unused").

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import type { Bridge, EmitterTreeDto } from "@particle-editor/bridge-schema";
import { SetLinkGroupDialog } from "../SetLinkGroupDialog";
import { useTreeContextStore } from "@/lib/tree-context";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";

function fixtureTree(): EmitterTreeDto {
  return {
    root: {
      id: -1, name: "", role: "root", linkGroup: 0, visible: true,
      children: [
        { id: 0, name: "A", role: "root", linkGroup: 0, visible: true, children: [] },
        { id: 1, name: "B", role: "root", linkGroup: 0, visible: true, children: [] },
      ],
    },
  };
}

function makeStubBridge(tree: EmitterTreeDto): Bridge & { request: ReturnType<typeof vi.fn> } {
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "emitters/list") return Promise.resolve(tree);
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

beforeEach(() => {
  useTreeContextStore.getState().close();
  useEmitterSelectionStore.getState().clear();
});

describe("SetLinkGroupDialog", () => {
  it("renders Create new + Join existing radios; existing is disabled when no groups exist", async () => {
    const bridge = makeStubBridge(fixtureTree());
    useEmitterSelectionStore.getState().setIds([0, 1], 0);
    useTreeContextStore.getState().openDialog("set-link-group", 0);
    render(<SetLinkGroupDialog bridge={bridge} />);

    // Both radios render.
    const radioNew = await screen.findByTestId("set-link-group-radio-new");
    const radioExisting = screen.getByTestId("set-link-group-radio-existing");
    expect(radioNew).toBeChecked();
    // Wait for the emitters/list response to flow in, then the "Join
    // existing" radio should be disabled because the fixture has no
    // groups > 0.
    await waitFor(() => {
      expect(radioExisting).toBeDisabled();
    });
  });

  it("OK with the default Create new radio fires linkGroups/set-membership with groupId: -1", async () => {
    const bridge = makeStubBridge(fixtureTree());
    useEmitterSelectionStore.getState().setIds([0, 1], 0);
    useTreeContextStore.getState().openDialog("set-link-group", 0);
    render(<SetLinkGroupDialog bridge={bridge} />);

    // Wait for the dialog to capture selection + finish the list fetch.
    await screen.findByTestId("set-link-group-radio-new");

    fireEvent.click(screen.getByRole("button", { name: "OK" }));

    // OK now diffs first (), so the join lands a microtask later.
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
      expect(calls.find((c) => c.kind === "linkGroups/set-membership")).toBeDefined();
    });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const membership = calls.find((c) => c.kind === "linkGroups/set-membership");
    expect(membership.params).toEqual({ ids: [0, 1], groupId: -1 });
  });

  it("disables OK with a hint when fewer than 2 emitters are selected for a new group", async () => {
    const bridge = makeStubBridge(fixtureTree());
    // Only ONE emitter selected — a group needs >= 2 (host CreateLinkGroup
    // silently no-ops below 2). The dialog must not let OK fire a no-op.
    useEmitterSelectionStore.getState().setIds([0], 0);
    useTreeContextStore.getState().openDialog("set-link-group", 0);
    render(<SetLinkGroupDialog bridge={bridge} />);
    await screen.findByTestId("set-link-group-radio-new");

    const ok = screen.getByRole("button", { name: "OK" });
    expect(ok).toBeDisabled();
    expect(screen.getByText(/at least 2 emitters/i)).toBeInTheDocument();

    fireEvent.click(ok);
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    expect(calls.find((c) => c.kind === "linkGroups/set-membership")).toBeUndefined();
  });

  it("allows OK for 'Join existing' with a single selected emitter", async () => {
    // Joining ONE emitter to an existing group is valid (>= 2 members result).
    const tree: EmitterTreeDto = {
      root: {
        id: -1, name: "", role: "root", linkGroup: 0, visible: true,
        children: [
          { id: 0, name: "A", role: "root", linkGroup: 0, visible: true, children: [] },
          { id: 1, name: "B", role: "root", linkGroup: 3, visible: true, children: [] },
        ],
      },
    };
    const bridge = makeStubBridge(tree);
    useEmitterSelectionStore.getState().setIds([0], 0);
    useTreeContextStore.getState().openDialog("set-link-group", 0);
    render(<SetLinkGroupDialog bridge={bridge} />);
    await screen.findByTestId("set-link-group-radio-existing");
    // Switch to join mode (group 3 exists).
    fireEvent.click(screen.getByTestId("set-link-group-radio-existing"));

    const ok = screen.getByRole("button", { name: "OK" });
    await waitFor(() => expect(ok).not.toBeDisabled());
  });

  // ─── — inline join-conflict note (one-click join) ─────────

  function makeConflictBridge(
    tree: EmitterTreeDto,
    conflicts: { id: number; fields: string[] }[],
  ): Bridge & { request: ReturnType<typeof vi.fn> } {
    return {
      request: vi.fn().mockImplementation((req: { kind: string }) => {
        if (req.kind === "emitters/list") return Promise.resolve(tree);
        if (req.kind === "linkGroups/diff-membership")
          return Promise.resolve({ conflicts });
        return Promise.resolve({});
      }),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
  }

  it("shows an INLINE note listing the fields a join would overwrite (no separate confirm)", async () => {
    const bridge = makeConflictBridge(fixtureTree(), [
      { id: 1, fields: ["lifetime", "gravity"] },
    ]);
    useEmitterSelectionStore.getState().setIds([0, 1], 0);
    useTreeContextStore.getState().openDialog("set-link-group", 0);
    render(<SetLinkGroupDialog bridge={bridge} />);
    await screen.findByTestId("set-link-group-radio-new");

    // The note appears reactively (no OK click needed) and lists the fields.
    await screen.findByTestId("link-conflict-inline");
    expect(screen.getByText(/lifetime/)).toBeInTheDocument();
    expect(screen.getByText(/gravity/)).toBeInTheDocument();

    // The diff was requested with the SAME params the join will use, and no
    // join has fired yet (the user is just viewing the form).
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const diff = calls.find((c) => c.kind === "linkGroups/diff-membership");
    expect(diff.params).toEqual({ ids: [0, 1], groupId: -1 });
    expect(calls.find((c) => c.kind === "linkGroups/set-membership")).toBeUndefined();
  });

  it("OK joins in a SINGLE click even when fields disagree", async () => {
    const bridge = makeConflictBridge(fixtureTree(), [
      { id: 1, fields: ["lifetime"] },
    ]);
    useEmitterSelectionStore.getState().setIds([0, 1], 0);
    useTreeContextStore.getState().openDialog("set-link-group", 0);
    render(<SetLinkGroupDialog bridge={bridge} />);
    await screen.findByTestId("set-link-group-radio-new");
    await screen.findByTestId("link-conflict-inline"); // note is showing

    // One click → join fires immediately (handleOk is synchronous now).
    fireEvent.click(screen.getByRole("button", { name: "OK" }));

    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const membership = calls.find((c) => c.kind === "linkGroups/set-membership");
    expect(membership).toBeDefined();
    expect(membership.params).toEqual({ ids: [0, 1], groupId: -1 });
  });

  it("shows no inline note when the join overwrites nothing", async () => {
    const bridge = makeConflictBridge(fixtureTree(), []);
    useEmitterSelectionStore.getState().setIds([0, 1], 0);
    useTreeContextStore.getState().openDialog("set-link-group", 0);
    render(<SetLinkGroupDialog bridge={bridge} />);
    await screen.findByTestId("set-link-group-radio-new");

    // Let the reactive diff resolve.
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
      expect(calls.find((c) => c.kind === "linkGroups/diff-membership")).toBeDefined();
    });
    expect(screen.queryByTestId("link-conflict-inline")).toBeNull();

    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    expect(calls.find((c) => c.kind === "linkGroups/set-membership")).toBeDefined();
  });
});
