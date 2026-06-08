// Vitest unit test for the LinkGroupSettingsDialog (Screen 4 Batch B1).
// Verifies that:
//   1. The dialog mounts and renders checkboxes from the mock fixture
//      after `linkGroups/list-exempt-fields` resolves.
//   2. Reset All clears all checkboxes (in local state) — clicking OK
//      after Reset fires `linkGroups/set-exempt-fields` with the empty
//      set.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, act, waitFor } from "@testing-library/react";
import { LinkGroupSettingsDialog } from "../LinkGroupSettingsDialog";
import { useTreeContextStore } from "@/lib/tree-context";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeStubBridge(opts?: {
  conflicts?: { id: number; fields: string[] }[];
}): Bridge & {
  request: ReturnType<typeof vi.fn>;
} {
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "linkGroups/list-exempt-fields") {
        return Promise.resolve({
          fields: ["colorTexture", "normalTexture", "trackIndex"],
        });
      }
      if (req.kind === "linkGroups/diff-exempt-change") {
        return Promise.resolve({ conflicts: opts?.conflicts ?? [] });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

beforeEach(() => {
  useTreeContextStore.getState().close();
});

describe("LinkGroupSettingsDialog", () => {
  it("renders exempt-field checkboxes and Reset All + OK commits an empty exempt set", async () => {
    const bridge = makeStubBridge();
    await act(async () => {
      useTreeContextStore.getState().openDialog("link-group", 0, 1);
    });
    render(<LinkGroupSettingsDialog bridge={bridge} />);

    // Wait for the async fetch to resolve and the checkboxes to render.
    await waitFor(() => {
      expect(screen.getByLabelText("Color texture")).toBeTruthy();
    });

    // Default exempts are colorTexture / normalTexture / trackIndex —
    // those checkboxes should be UNCHECKED (exempt = unchecked / per-emitter).
    const colorTexture = screen.getByLabelText("Color texture") as HTMLInputElement;
    expect(colorTexture.checked).toBe(false);

    // A non-exempt field (e.g. Lifetime) should be CHECKED (shared).
    const lifetime = screen.getByLabelText("Lifetime") as HTMLInputElement;
    expect(lifetime.checked).toBe(true);

    // Reset All — every field becomes SHARED (no exempts).
    fireEvent.click(screen.getByRole("button", { name: "Reset All" }));
    expect(colorTexture.checked).toBe(true);

    // OK commits.
    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "linkGroups/set-exempt-fields",
      params: { groupId: 1, fields: [] },
    });
  });

  it("groups fields into categories with a per-category 'share all' toggle", async () => {
    const bridge = makeStubBridge();
    await act(async () => {
      useTreeContextStore.getState().openDialog("link-group", 0, 1);
    });
    render(<LinkGroupSettingsDialog bridge={bridge} />);

    await waitFor(() => {
      expect(screen.getByText("Curves")).toBeTruthy();
    });

    // The four category headers render (Weather folded into Appearance).
    for (const cat of ["Curves", "Basic", "Appearance", "Physics"]) {
      expect(screen.getByText(cat)).toBeTruthy();
    }

    // Fixture exempts trackIndex (atlas) but not the color curves, so the
    // Curves category is MIXED → its header toggle reads unchecked.
    const atlas = screen.getByLabelText("Atlas index curve") as HTMLInputElement;
    const red = screen.getByLabelText("Red curve") as HTMLInputElement;
    expect(atlas.checked).toBe(false);
    expect(red.checked).toBe(true);
    const shareCurves = screen.getByLabelText("Share all Curves") as HTMLInputElement;
    expect(shareCurves.checked).toBe(false);

    // Clicking it shares the whole category — atlas becomes shared.
    fireEvent.click(shareCurves);
    expect(atlas.checked).toBe(true);

    // OK commits an exempt set that no longer contains trackIndex.
    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    const call = bridge.request.mock.calls.find(
      (c) => (c[0] as { kind: string }).kind === "linkGroups/set-exempt-fields",
    );
    const committed = (call![0] as { params: { fields: string[] } }).params.fields;
    expect(committed).not.toContain("trackIndex");
    expect(committed).toContain("colorTexture");
  });

  it("shows an inline disagreement warning when sharing a field members disagree on", async () => {
    const bridge = makeStubBridge({
      conflicts: [{ id: 2, fields: ["lifetime"] }],
    });
    await act(async () => {
      useTreeContextStore.getState().openDialog("link-group", 0, 1);
    });
    render(<LinkGroupSettingsDialog bridge={bridge} />);
    await waitFor(() => screen.getByLabelText("Color texture"));

    const warning = await screen.findByTestId("link-settings-conflict-inline");
    expect(warning).toHaveTextContent(/lifetime/i);
    // One dissenting emitter.
    expect(warning).toHaveTextContent(/1 emitter/i);
  });

  it("shows no warning when members agree (no conflicts)", async () => {
    const bridge = makeStubBridge({ conflicts: [] });
    await act(async () => {
      useTreeContextStore.getState().openDialog("link-group", 0, 1);
    });
    render(<LinkGroupSettingsDialog bridge={bridge} />);
    await waitFor(() => screen.getByLabelText("Color texture"));
    // Let the reactive diff effect settle.
    await act(async () => {});
    expect(screen.queryByTestId("link-settings-conflict-inline")).not.toBeInTheDocument();
  });

  it("re-queries diff-exempt-change with the proposed exempt set when a field is toggled", async () => {
    const bridge = makeStubBridge({ conflicts: [] });
    await act(async () => {
      useTreeContextStore.getState().openDialog("link-group", 0, 1);
    });
    render(<LinkGroupSettingsDialog bridge={bridge} />);
    await waitFor(() => screen.getByLabelText("Color texture"));

    // Share colorTexture (un-exempt it) → the next diff call's exempt set
    // should no longer contain it.
    fireEvent.click(screen.getByLabelText("Color texture"));
    await waitFor(() => {
      const diffCalls = bridge.request.mock.calls.filter(
        (c) => (c[0] as { kind: string }).kind === "linkGroups/diff-exempt-change",
      );
      const last = diffCalls.at(-1)![0] as { params: { groupId: number; exempt: string[] } };
      expect(last.params.groupId).toBe(1);
      expect(last.params.exempt).not.toContain("colorTexture");
    });
  });
});
