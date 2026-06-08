// Vitest specs for TexturePickerField — the color/bump texture field with
// a Browse button (sub-feature A) and a frequently-used palette button
// (sub-feature B).
//
// Browse: calls `onBrowse(slot)` and commits the returned basename via
// `onCommit` — but only when non-empty (a cancelled dialog must not clear
// the existing value).
//
// Palette (B): a palette button opens the TexturePalettePopover. Every
// non-empty commit — manual blur, Browse, or palette apply — also fires
// `textures/palette/touch-recent` so recents stay warm (legacy parity).

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { TexturePickerField } from "../EmitterPropertyTabs";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeBridge() {
  const request = vi
    .fn()
    .mockImplementation((req: { kind: string; params?: Record<string, unknown> }) => {
      switch (req.kind) {
        case "textures/palette/list":
          return Promise.resolve({
            hasMod: true,
            filter: req.params!.slot,
            pins: [],
            recents: [],
          });
        case "textures/palette/thumbnail":
          return Promise.resolve({ dataUri: null });
        case "textures/palette/toggle-pin":
          return Promise.resolve({ ok: true, pinned: true });
        case "textures/palette/touch-recent":
          return Promise.resolve({ ok: true });
        default:
          return Promise.resolve({});
      }
    });
  const on = vi.fn().mockReturnValue(() => {});
  return { request, on } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
  };
}

describe("TexturePickerField — Browse button", () => {
  it("renders the label, the text input (bound to value), and a Browse button", () => {
    render(
      <TexturePickerField
        label="Color texture:"
        slot="color"
        value="p_smoke_atlas_02.dds"
        onCommit={vi.fn()}
        onBrowse={vi.fn(async () => "")}
        bridge={makeBridge()}
      />,
    );
    const input = screen.getByRole("textbox", { name: "Color texture:" });
    expect((input as HTMLInputElement).value).toBe("p_smoke_atlas_02.dds");
    expect(
      screen.getByRole("button", { name: "Browse for Color texture:" }),
    ).toBeTruthy();
  });

  it("commits the picked basename when Browse resolves non-empty, passing the slot", async () => {
    const onCommit = vi.fn();
    const onBrowse = vi.fn(async () => "p_explosion_atlas_02.dds");
    render(
      <TexturePickerField
        label="Color texture:"
        slot="color"
        value=""
        onCommit={onCommit}
        onBrowse={onBrowse}
        bridge={makeBridge()}
      />,
    );
    fireEvent.click(screen.getByRole("button", { name: "Browse for Color texture:" }));
    await waitFor(() =>
      expect(onCommit).toHaveBeenCalledWith("p_explosion_atlas_02.dds"),
    );
    expect(onBrowse).toHaveBeenCalledWith("color");
  });

  it("does NOT commit when Browse resolves empty (cancelled / browser-mode)", async () => {
    const onCommit = vi.fn();
    const onBrowse = vi.fn(async () => "");
    render(
      <TexturePickerField
        label="Bump texture:"
        slot="bump"
        value="existing.dds"
        onCommit={onCommit}
        onBrowse={onBrowse}
        bridge={makeBridge()}
      />,
    );
    fireEvent.click(screen.getByRole("button", { name: "Browse for Bump texture:" }));
    await waitFor(() => expect(onBrowse).toHaveBeenCalledWith("bump"));
    expect(onCommit).not.toHaveBeenCalled();
  });
});

describe("TexturePickerField — palette button + usage tracking (B)", () => {
  it("renders a palette button that opens the palette popover", async () => {
    const b = makeBridge();
    render(
      <TexturePickerField
        label="Color texture:"
        slot="color"
        value=""
        onCommit={vi.fn()}
        onBrowse={vi.fn(async () => "")}
        bridge={b}
      />,
    );
    fireEvent.click(
      screen.getByRole("button", { name: "Open texture palette for Color texture:" }),
    );
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({
        kind: "textures/palette/list",
        params: { slot: "color" },
      });
    });
  });

  it("fires touch-recent with the slot when a value is committed manually", async () => {
    const b = makeBridge();
    const onCommit = vi.fn();
    render(
      <TexturePickerField
        label="Color texture:"
        slot="color"
        value=""
        onCommit={onCommit}
        onBrowse={vi.fn(async () => "")}
        bridge={b}
      />,
    );
    const input = screen.getByRole("textbox", { name: "Color texture:" });
    fireEvent.change(input, { target: { value: "p_new_atlas.dds" } });
    fireEvent.blur(input);
    expect(onCommit).toHaveBeenCalledWith("p_new_atlas.dds");
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({
        kind: "textures/palette/touch-recent",
        params: { filename: "p_new_atlas.dds", slot: "color" },
      });
    });
  });

  it("fires touch-recent when Browse commits a file", async () => {
    const b = makeBridge();
    render(
      <TexturePickerField
        label="Bump texture:"
        slot="bump"
        value=""
        onCommit={vi.fn()}
        onBrowse={vi.fn(async () => "p_bump.dds")}
        bridge={b}
      />,
    );
    fireEvent.click(screen.getByRole("button", { name: "Browse for Bump texture:" }));
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({
        kind: "textures/palette/touch-recent",
        params: { filename: "p_bump.dds", slot: "bump" },
      });
    });
  });

  it("does NOT fire touch-recent on an empty (cancelled) commit", async () => {
    const b = makeBridge();
    const onBrowse = vi.fn(async () => "");
    render(
      <TexturePickerField
        label="Bump texture:"
        slot="bump"
        value="existing.dds"
        onCommit={vi.fn()}
        onBrowse={onBrowse}
        bridge={b}
      />,
    );
    fireEvent.click(screen.getByRole("button", { name: "Browse for Bump texture:" }));
    await waitFor(() => expect(onBrowse).toHaveBeenCalled());
    const touched = b.request.mock.calls.some(
      (c) => (c[0] as { kind?: string })?.kind === "textures/palette/touch-recent",
    );
    expect(touched).toBe(false);
  });
});
