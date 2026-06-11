// Vitest specs for the B1.3-P5 "Always face camera" semantic-flip
// cascade.
//
// The legacy `World Oriented` checkbox stored `isWorldOriented` and
// displayed it directly. P5 renames the field to `Always face camera`
// and inverts the semantic — checkbox checked = "always face camera"
// = `isWorldOriented === false`. When `blendMode === BLEND_BUMP`
// (==11) the camera-facing orientation is forced, so the checkbox
// renders checked + disabled regardless of the stored
// `isWorldOriented` value.
//
// The checkbox is a Radix `<Checkbox.Root>` (per `FieldCheckbox` in
// `EmitterPropertyTabs.tsx`); Radix surfaces state via the
// `data-state="checked" | "unchecked"` attribute, not the native
// `checked` property. Disabled state surfaces via the `data-disabled`
// attribute.

import { describe, it, expect, vi } from "vitest";
import { render as rtlRender, screen, fireEvent } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import { AppearanceTab } from "../EmitterPropertyTabs";
import { makeFixtureProperties } from "@/bridge/mock-state";

//: AppearanceTab mounts Tips (Radix Tooltip.Root) on the form-row
// labels, which require the app-level Tooltip.Provider — wrapper stands in
// for it (precedent: renderWithTooltips in EmitterTree.test.tsx).
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

describe("AppearanceTab — Always face camera semantic flip", () => {
  it("displays unchecked when isWorldOriented=true (and blendMode != BLEND_BUMP)", () => {
    const props = {
      ...makeFixtureProperties(0),
      blendMode: 1,
      isWorldOriented: true,
    };
    render(<AppearanceTab properties={props} onCommit={vi.fn()} />);
    const cb = screen.getByLabelText("Always face camera");
    expect(cb.getAttribute("data-state")).toBe("unchecked");
  });

  it("displays checked when isWorldOriented=false (and blendMode != BLEND_BUMP)", () => {
    const props = {
      ...makeFixtureProperties(0),
      blendMode: 1,
      isWorldOriented: false,
    };
    render(<AppearanceTab properties={props} onCommit={vi.fn()} />);
    const cb = screen.getByLabelText("Always face camera");
    expect(cb.getAttribute("data-state")).toBe("checked");
  });

  it("forced checked + disabled when blendMode = BLEND_BUMP", () => {
    // BLEND_BUMP (==11) forces face-camera orientation regardless of
    // the stored isWorldOriented value — even an isWorldOriented=true
    // emitter (which under non-BUMP modes would display the checkbox
    // as unchecked) shows checked + disabled here.
    const props = {
      ...makeFixtureProperties(0),
      blendMode: 11,
      isWorldOriented: true,
    };
    render(<AppearanceTab properties={props} onCommit={vi.fn()} />);
    const cb = screen.getByLabelText("Always face camera");
    expect(cb.getAttribute("data-state")).toBe("checked");
    expect(cb.getAttribute("data-disabled")).not.toBeNull();
  });

  it("clicking the checkbox commits the negation of the displayed state", () => {
    // isWorldOriented=true → displays unchecked → click → display
    // becomes checked → commits isWorldOriented=false (the negation).
    const onCommit = vi.fn();
    const props = {
      ...makeFixtureProperties(0),
      blendMode: 1,
      isWorldOriented: true,
    };
    render(<AppearanceTab properties={props} onCommit={onCommit} />);
    fireEvent.click(screen.getByLabelText("Always face camera"));
    expect(onCommit).toHaveBeenCalledWith({ isWorldOriented: false });
  });

  it("clicking the checkbox when it's checked commits isWorldOriented=true", () => {
    // Inverse of the prior case — isWorldOriented=false → displays
    // checked → click → display becomes unchecked → commits
    // isWorldOriented=true.
    const onCommit = vi.fn();
    const props = {
      ...makeFixtureProperties(0),
      blendMode: 1,
      isWorldOriented: false,
    };
    render(<AppearanceTab properties={props} onCommit={onCommit} />);
    fireEvent.click(screen.getByLabelText("Always face camera"));
    expect(onCommit).toHaveBeenCalledWith({ isWorldOriented: true });
  });
});
