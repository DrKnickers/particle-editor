// Vitest unit tests for ReferenceObjectPicker. Driven by the real
// MockBridge so the canned reference-object-list + the set handlers (including
// the skinned-status path) are exercised end-to-end against the schema.
//
// Covered: object enumeration + category grouping, selection dispatch, the
// "skinned — not supported" status note, the visibility toggle, and a numeric
// transform commit (position X via the Spinner). (The unit-grid toggle moved to
// the Ground panel in S48 — see GroundTexturePanel.test.tsx.)

import { describe, it, expect } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { ReferenceObjectPicker } from "../ReferenceObjectPicker";
import { MockBridge } from "@/bridge/mock";
import type { Bridge } from "@particle-editor/bridge-schema";

describe("ReferenceObjectPicker — selection + status", () => {
  it("enumerates objects grouped by category and dispatches on select", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const select = await screen.findByRole("combobox", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("option", { name: "AT_AT_Walker" })).toBeInTheDocument();
    });
    // Category <optgroup>s render as groups.
    expect(screen.getByRole("group", { name: "Vehicle" })).toBeInTheDocument();
    expect(screen.getByRole("group", { name: "Turret" })).toBeInTheDocument();

    fireEvent.change(select, { target: { value: "AT_AT_Walker" } });
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectName).toBe("AT_AT_Walker");
      expect(snap.referenceObjectStatus).toBe("ok");
    });
  });

  it("shows the skinned-unsupported note for a skinned object", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const select = await screen.findByRole("combobox", { name: "Reference object" });
    await waitFor(() => {
      expect(screen.getByRole("option", { name: "Stormtrooper_Squad" })).toBeInTheDocument();
    });
    fireEvent.change(select, { target: { value: "Stormtrooper_Squad" } });

    const note = await screen.findByRole("alert");
    expect(note.textContent).toMatch(/skinned/i);
  });

  it("commits a position-X change through the transform dispatch", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);

    const posX = await screen.findByLabelText("Position X");
    fireEvent.change(posX, { target: { value: "12.5" } });
    fireEvent.blur(posX);  // Spinner commits on blur
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectPosition[0]).toBe(12.5);
    });
  });

  it("surfaces a selected name absent from the enumerated list (mod-only object)", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByRole("combobox", { name: "Reference object" });
    // Select a Name NOT in the canned list — the case exists for (a mod
    // object absent from a partial/async list). The picker must show it as the
    // active selection via the standalone <option>, not snap back to "None".
    await bridge.request({ kind: "engine/set/reference-object", params: { name: "Mod_Only_Object" } });
    const opt = await screen.findByRole("option", { name: "Mod_Only_Object" });
    expect((opt as HTMLOptionElement).selected).toBe(true);
  });

  it("a single-axis transform edit preserves the other axes + rotation", async () => {
    const bridge = new MockBridge();
    render(<ReferenceObjectPicker bridge={bridge as unknown as Bridge} onClose={() => {}} />);
    await screen.findByLabelText("Position X");
    // Seed a known transform, then edit only Position Y.
    await bridge.request({
      kind: "engine/set/reference-object-transform",
      params: { position: [1, 2, 3], rotation: [10, 20, 30] },
    });
    const posY = await screen.findByLabelText("Position Y");
    await waitFor(() => expect((posY as HTMLInputElement).value).toBe("2.0"));
    fireEvent.change(posY, { target: { value: "99" } });
    fireEvent.blur(posY);
    await waitFor(async () => {
      const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
      expect(snap.referenceObjectPosition).toEqual([1, 99, 3]);   // X + Z preserved
      expect(snap.referenceObjectRotation).toEqual([10, 20, 30]); // rotation untouched
    });
  });
});
