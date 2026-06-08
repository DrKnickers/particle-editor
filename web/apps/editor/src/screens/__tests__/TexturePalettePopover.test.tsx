// Vitest specs for TexturePalettePopover — the frequently-used texture
// palette popup (sub-feature B). Mirrors the GroundDropdown/BackgroundDropdown
// Radix-Popover test pattern: build a fake bridge, click the trigger, then
// assert against the portaled content.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { TexturePalettePopover } from "../TexturePalettePopover";
import type { Bridge, PaletteEntry } from "@particle-editor/bridge-schema";

type ListResp = {
  hasMod: boolean;
  filter: "color" | "bump";
  pins: PaletteEntry[];
  recents: PaletteEntry[];
};

function makeBridge(opts?: {
  list?: Partial<ListResp>;
  thumbnail?: string | null;
  thumbnailStatus?: "ok" | "missing" | "broken";
  togglePin?: { ok: true; pinned: boolean } | { ok: false; reason: "pins-full" };
}) {
  const listResp: ListResp = {
    hasMod: true,
    filter: "color",
    pins: [],
    recents: [],
    ...opts?.list,
  };
  const request = vi
    .fn()
    .mockImplementation((req: { kind: string; params?: Record<string, unknown> }) => {
      switch (req.kind) {
        case "textures/palette/list":
          return Promise.resolve({ ...listResp, filter: req.params!.slot });
        case "textures/palette/thumbnail":
          return Promise.resolve({
            dataUri: opts?.thumbnail ?? null,
            status: opts?.thumbnailStatus ?? (opts?.thumbnail ? "ok" : "missing"),
          });
        case "textures/palette/toggle-pin":
          return Promise.resolve(opts?.togglePin ?? { ok: true, pinned: true });
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

const pin: PaletteEntry = {
  filename: "p_explosion_atlas.dds",
  pinned: true,
  slotMask: 1,
};
const recent: PaletteEntry = {
  filename: "p_smoke_atlas.dds",
  pinned: false,
  slotMask: 1,
};

function open() {
  fireEvent.click(screen.getByRole("button", { name: "Palette" }));
}

describe("TexturePalettePopover", () => {
  it("renders pinned and recent textures from the list response", async () => {
    const b = makeBridge({ list: { pins: [pin], recents: [recent] } });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    expect(
      await screen.findByRole("button", { name: `Apply ${pin.filename}` }),
    ).toBeInTheDocument();
    expect(
      screen.getByRole("button", { name: `Apply ${recent.filename}` }),
    ).toBeInTheDocument();
    expect(screen.getByText("Pinned")).toBeInTheDocument();
    expect(screen.getByText("Recent")).toBeInTheDocument();
  });

  it("requests the palette list for the opened slot (slot-aware)", async () => {
    const b = makeBridge();
    render(
      <TexturePalettePopover bridge={b} slot="bump" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({
        kind: "textures/palette/list",
        params: { slot: "bump" },
      });
    });
  });

  it("toggling the filter re-queries the list with the other slot", async () => {
    const b = makeBridge();
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    fireEvent.click(await screen.findByRole("button", { name: "Bump" }));
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({
        kind: "textures/palette/list",
        params: { slot: "bump" },
      });
    });
  });

  it("clicking a thumbnail applies it and closes the popover", async () => {
    const onApply = vi.fn();
    const b = makeBridge({ list: { pins: [pin], recents: [] } });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={onApply}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    fireEvent.click(
      await screen.findByRole("button", { name: `Apply ${pin.filename}` }),
    );
    expect(onApply).toHaveBeenCalledWith(pin.filename);
    await waitFor(() => {
      expect(
        screen.queryByRole("button", { name: `Apply ${pin.filename}` }),
      ).not.toBeInTheDocument();
    });
  });

  it("clicking the star toggles the pin", async () => {
    const b = makeBridge({ list: { pins: [], recents: [recent] } });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    fireEvent.click(
      await screen.findByRole("button", { name: `Pin ${recent.filename}` }),
    );
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({
        kind: "textures/palette/toggle-pin",
        params: { filename: recent.filename },
      });
    });
  });

  it("shows a status message when pinning is rejected as full", async () => {
    const b = makeBridge({
      list: { pins: [], recents: [recent] },
      togglePin: { ok: false, reason: "pins-full" },
    });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    fireEvent.click(
      await screen.findByRole("button", { name: `Pin ${recent.filename}` }),
    );
    expect(await screen.findByText(/pins full/i)).toBeInTheDocument();
  });

  it("renders a placeholder when a thumbnail is null", async () => {
    const b = makeBridge({ list: { pins: [pin], recents: [] }, thumbnail: null });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    expect(
      await screen.findByTestId(`palette-thumb-placeholder-${pin.filename}`),
    ).toBeInTheDocument();
  });

  it("distinguishes a broken thumbnail (decode-failed) from missing ()", async () => {
    const b = makeBridge({
      list: { pins: [pin], recents: [] },
      thumbnail: null,
      thumbnailStatus: "broken",
    });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    const ph = await screen.findByTestId(`palette-thumb-placeholder-${pin.filename}`);
    expect(ph).toHaveAttribute("data-thumb-status", "broken");
    expect(ph).toHaveTextContent(/broken/i);
  });

  it("marks a missing thumbnail (file-not-found) as missing ()", async () => {
    const b = makeBridge({
      list: { pins: [pin], recents: [] },
      thumbnail: null,
      thumbnailStatus: "missing",
    });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    const ph = await screen.findByTestId(`palette-thumb-placeholder-${pin.filename}`);
    expect(ph).toHaveAttribute("data-thumb-status", "missing");
    expect(ph).toHaveTextContent(/missing/i);
  });

  it("renders the decoded image when status is ok", async () => {
    const b = makeBridge({
      list: { pins: [pin], recents: [] },
      thumbnail: "data:image/png;base64,AAAA",
      thumbnailStatus: "ok",
    });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    const applyBtn = await screen.findByRole("button", { name: `Apply ${pin.filename}` });
    await waitFor(() =>
      expect(applyBtn.querySelector("img")).toHaveAttribute(
        "src",
        "data:image/png;base64,AAAA",
      ),
    );
    expect(
      screen.queryByTestId(`palette-thumb-placeholder-${pin.filename}`),
    ).not.toBeInTheDocument();
  });

  it("shows the no-mod hint when no mod is active", async () => {
    const b = makeBridge({ list: { hasMod: false, pins: [], recents: [] } });
    render(
      <TexturePalettePopover bridge={b} slot="color" onApply={() => {}}>
        <button>Palette</button>
      </TexturePalettePopover>,
    );
    open();
    expect(await screen.findByText(/no mod selected/i)).toBeInTheDocument();
  });
});
