// Vitest unit tests for the TexturePalette primitive.
// Exercises: selection border, right-click menu items, empty-state placeholder.

import { describe, it, expect, vi } from "vitest";
import { render as rtlRender, screen, fireEvent, waitFor } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import { TexturePalette } from "../TexturePalette";
import type { TextureItem } from "../TexturePalette";

//: each palette cell mounts a Tip (Radix Tooltip.Root), which
// requires the app-level Tooltip.Provider — wrapper stands in for it
// (precedent: renderToolbar in Toolbar.test.tsx).
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

const ITEMS: TextureItem[] = [
  { path: "a.tga", label: "alpha", thumbnailSrc: "data:image/png;base64,iVBOR" },
  { path: "b.tga", label: "beta",  thumbnailSrc: null },
];

describe("TexturePalette", () => {
  it("selected cell renders with aria-selected=true", () => {
    render(
      <TexturePalette items={ITEMS} value="a.tga" onChange={() => {}} />
    );
    // getAllByRole handles Radix potentially rendering duplicate elements.
    const alphaBtns = screen.getAllByRole("option", { name: /alpha/i });
    // At least one of the rendered elements should be selected.
    const selectedAlpha = alphaBtns.find((el) => el.getAttribute("aria-selected") === "true");
    expect(selectedAlpha).toBeDefined();
    const betaBtns = screen.getAllByRole("option", { name: /beta/i });
    const selectedBeta = betaBtns.find((el) => el.getAttribute("aria-selected") === "true");
    expect(selectedBeta).toBeUndefined();
  });

  it("right-clicking an item opens context menu with Browse/Clear/Open Folder", async () => {
    const onBrowse = vi.fn();
    const onClear = vi.fn();
    const onReveal = vi.fn();
    render(
      <TexturePalette
        items={ITEMS}
        value={null}
        onChange={() => {}}
        onBrowse={onBrowse}
        onClear={onClear}
        onReveal={onReveal}
      />
    );
    // Use getAllByRole to handle Radix duplicating the trigger in the portal.
    const alphaBtns = screen.getAllByRole("option", { name: /alpha/i });
    // Radix ContextMenu opens on contextmenu event.
    fireEvent.contextMenu(alphaBtns[0]);
    await waitFor(() => {
      expect(screen.getByText("Browse for file…")).toBeInTheDocument();
      expect(screen.getByText("Clear")).toBeInTheDocument();
      expect(screen.getByText("Open texture folder")).toBeInTheDocument();
    });
  });

  it("empty items array renders '(no textures)' placeholder", () => {
    render(
      <TexturePalette items={[]} value={null} onChange={() => {}} />
    );
    expect(screen.getByText("(no textures)")).toBeInTheDocument();
  });
});
