// Vitest tests for CurveEditorPanel.
//
// Covered:
//   - Renders the panel chrome + 7 channel rows (one per CHANNELS
//     entry) regardless of selection.
//   - R / G / B default ON; Scale / Alpha / Rotation / Index default
//     OFF. Visibility is SESSION-SCOPED — every boot is fresh.
//   - Default focus channel is "red" (the first visible).
//   - Renders the placeholder when no emitter is selected.
//   - When an emitter is selected, the multi-channel CurveEditor SVG
//     mounts and one <g data-channel-id=…> renders per VISIBLE channel.
//   - Toggling a channel checkbox flips the SVG layer's presence
//     (visibleChannels prop wired correctly).

import { describe, it, expect, vi, beforeEach } from "vitest";
import { act, fireEvent, render as rtlRender, screen, waitFor } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import type {
  Bridge,
  TrackDto,
} from "@particle-editor/bridge-schema";
import { TRACK_NAMES } from "@particle-editor/bridge-schema";
import { CurveEditorPanel, CHANNELS } from "../CurveEditorPanel";

// the toolbar buttons mount Tips (Radix Tooltip.Root), which
// require the app-level Tooltip.Provider — wrapper stands in for it
// (precedent: renderToolbar in Toolbar.test.tsx).
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });
import { makeDefaultEngineState } from "@/bridge/mock-state";
import {
  getCurveKeysClipboard,
  setCurveKeysClipboard,
} from "@/lib/curve-key-clipboard";
import { __resetAtlasContext } from "@/lib/atlas-context";
import { __resetRightDockForTests } from "@/lib/right-dock";

function fixtureTracks(): TrackDto[] {
  return TRACK_NAMES.map((name) => ({
    name,
    keys: [
      { time: 0,   value: 0 },
      { time: 100, value: name === "rotationSpeed" ? -1 : 1 },
    ],
    interpolation: "linear",
    lockedTo: null,
  }));
}

/** Same as fixtureTracks but with Green locked to Red — the minimal
 *  scenario for exercising panel-level handler gating. */
function lockedFixtureTracks(): TrackDto[] {
  return fixtureTracks().map((t) => ({
    ...t,
    lockedTo: t.name === "green" ? "red" : null,
  }));
}

type SelectionListener = (e: { payload: { id: number | null } }) => void;

function makeStubBridge(initialSelectedId: number | null, tracks?: TrackDto[]) {
  const listeners: SelectionListener[] = [];
  const bridge = {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") {
        return Promise.resolve({
          ...makeDefaultEngineState(),
          selectedEmitterId: initialSelectedId,
        });
      }
      if (req.kind === "emitters/get-tracks") {
        return Promise.resolve({ tracks: tracks ?? fixtureTracks() });
      }
      if (req.kind === "emitters/get-properties") {
        // textureSize=1 → ineligible for atlas auto-open; keeps the
        // controller inert in these tests (same as before the atlas picker).
        return Promise.resolve({ properties: { textureSize: 1 } });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockImplementation((kind: string, h: SelectionListener) => {
      if (kind === "emitters/selected") listeners.push(h);
      return () => {
        const idx = listeners.indexOf(h);
        if (idx >= 0) listeners.splice(idx, 1);
      };
    }),
  } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
    on: ReturnType<typeof vi.fn>;
  };
  return { bridge };
}

/** Enable + focus a channel by clicking its row. Used by specs that
 *  exercise channels not visible/focused by default (Scale, Alpha,
 *  Rotation, Index — the panel's defaults boot with only R/G/B
 *  visible and focus on Red). */
function selectChannel(id: string) {
  fireEvent.click(screen.getByTestId(`curve-channel-row-${id}`));
}

describe("CurveEditorPanel", () => {
  // CurveEditorPanel now publishes to the module-singleton atlas
  // context + mounts the auto-open controller (which writes the right-dock
  // store). Both stores survive across tests, so reset them per-test.
  beforeEach(() => {
    localStorage.clear();
    __resetAtlasContext();
    __resetRightDockForTests();
  });

  it("renders the .panel chrome + 7 channel rows regardless of selection", async () => {
    const { bridge } = makeStubBridge(null);
    render(<CurveEditorPanel bridge={bridge} />);
    expect(screen.getByTestId("curve-editor-panel")).toBeInTheDocument();
    // All 7 channels — checkboxes are present with the documented
    // default state.
    for (const c of CHANNELS) {
      const cb = screen.getByTestId(
        `curve-channel-checkbox-${c.id}`,
      ) as HTMLInputElement;
      expect(cb).toBeInTheDocument();
      expect(cb.checked).toBe(c.defaultOn);
    }
  });

  it("R / G / B default ON; Scale / Alpha / Rotation / Index default OFF", () => {
    const { bridge } = makeStubBridge(null);
    render(<CurveEditorPanel bridge={bridge} />);
    for (const id of ["red", "green", "blue"]) {
      const cb = screen.getByTestId(
        `curve-channel-checkbox-${id}`,
      ) as HTMLInputElement;
      expect(cb.checked).toBe(true);
    }
    for (const id of ["scale", "alpha", "rotation", "index"]) {
      const cb = screen.getByTestId(
        `curve-channel-checkbox-${id}`,
      ) as HTMLInputElement;
      expect(cb.checked).toBe(false);
    }
  });

  it("renders the placeholder when no emitter is selected", async () => {
    const { bridge } = makeStubBridge(null);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-editor-placeholder")).toBeInTheDocument();
    });
    // The multi-channel SVG isn't mounted in the placeholder branch.
    expect(screen.queryByTestId("curve-editor-svg")).toBeNull();
  });

  it("mounts the multi-channel SVG with one <g> per visible channel when an emitter is selected", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    // Snapshot resolves, then get-tracks; wait for layers (which only
    // appear after both promises have settled).
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    // 3 channels default ON → 3 layers (R / G / B). The rest are
    // hidden until the user clicks them on.
    for (const id of ["red", "green", "blue"]) {
      expect(
        screen.getByTestId(`curve-layer-${id}`),
      ).toBeInTheDocument();
    }
    for (const id of ["scale", "alpha", "rotation", "index"]) {
      expect(screen.queryByTestId(`curve-layer-${id}`)).toBeNull();
    }
  });

  it("toggling a channel checkbox adds/removes its SVG layer", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    // Wait for the get-tracks promise to settle so layers render.
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });

    // Initially Alpha is OFF — no layer. (Using Alpha rather than
    // Scale because Scale has the exclusive-on behavior — turning it
    // on would hide everything else and skew this spec's intent.)
    expect(screen.queryByTestId("curve-layer-alpha")).toBeNull();
    // Click the Alpha checkbox to enable it.
    const alphaCb = screen.getByTestId(
      "curve-channel-checkbox-alpha",
    ) as HTMLInputElement;
    fireEvent.click(alphaCb);
    expect(alphaCb.checked).toBe(true);
    // Layer appears.
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-alpha")).toBeInTheDocument();
    });

    // Click Red checkbox to disable it (default ON).
    const redCb = screen.getByTestId(
      "curve-channel-checkbox-red",
    ) as HTMLInputElement;
    fireEvent.click(redCb);
    expect(redCb.checked).toBe(false);
    await waitFor(() => {
      expect(screen.queryByTestId("curve-layer-red")).toBeNull();
    });
  });

  it("enabling Scale via its checkbox hides every other channel (scale-exclusive)", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    // Sanity: R / G / B start visible.
    for (const id of ["red", "green", "blue"]) {
      expect(
        (screen.getByTestId(`curve-channel-checkbox-${id}`) as HTMLInputElement).checked,
      ).toBe(true);
    }
    // Click Scale checkbox on.
    fireEvent.click(screen.getByTestId("curve-channel-checkbox-scale"));
    // Scale ON; everything else OFF.
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(true);
    for (const id of ["red", "green", "blue", "alpha", "rotation", "index"]) {
      expect(
        (screen.getByTestId(`curve-channel-checkbox-${id}`) as HTMLInputElement).checked,
      ).toBe(false);
    }
    // Layer set reflects: only Scale present.
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
    });
    for (const id of ["red", "green", "blue", "alpha", "rotation", "index"]) {
      expect(screen.queryByTestId(`curve-layer-${id}`)).toBeNull();
    }
  });

  it("enabling Scale via row click also hides every other channel", () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    fireEvent.click(screen.getByTestId("curve-channel-row-scale"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(true);
    for (const id of ["red", "green", "blue"]) {
      expect(
        (screen.getByTestId(`curve-channel-checkbox-${id}`) as HTMLInputElement).checked,
      ).toBe(false);
    }
    // Focus also moved to Scale.
    expect(
      (screen.getByTestId("curve-editor-panel")).dataset.focusChannel,
    ).toBe("scale");
  });

  it("clicking a non-Scale row while Scale is visible hides Scale (exits solo)", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    // Enter Scale solo.
    fireEvent.click(screen.getByTestId("curve-channel-row-scale"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(true);
    // Click Green row.
    fireEvent.click(screen.getByTestId("curve-channel-row-green"));
    // Scale hidden, Green visible + focused.
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(false);
    expect(
      (screen.getByTestId("curve-channel-checkbox-green") as HTMLInputElement).checked,
    ).toBe(true);
    expect(
      screen.getByTestId("curve-editor-panel").dataset.focusChannel,
    ).toBe("green");
  });

  it("enabling other channels via CHECKBOX while Scale is on does NOT auto-hide Scale", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    // Enable Scale (others now hidden).
    fireEvent.click(screen.getByTestId("curve-channel-checkbox-scale"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(true);
    // Re-enable Red. Scale must STAY on.
    fireEvent.click(screen.getByTestId("curve-channel-checkbox-red"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-red") as HTMLInputElement).checked,
    ).toBe(true);
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(true);
  });

  // Index is exclusive too — enabling it hides every other channel.
  it("enabling Index via its checkbox hides every other channel (index-exclusive)", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByTestId("curve-channel-checkbox-index"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-index") as HTMLInputElement).checked,
    ).toBe(true);
    for (const id of ["red", "green", "blue", "alpha", "scale", "rotation"]) {
      expect(
        (screen.getByTestId(`curve-channel-checkbox-${id}`) as HTMLInputElement).checked,
      ).toBe(false);
    }
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-index")).toBeInTheDocument();
    });
  });

  // The two exclusive channels replace each other — clicking Index
  // while Scale is solo turns Scale off and Index on.
  it("clicking Index while Scale is solo swaps solo to Index", () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    fireEvent.click(screen.getByTestId("curve-channel-row-scale"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(true);
    fireEvent.click(screen.getByTestId("curve-channel-row-index"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-index") as HTMLInputElement).checked,
    ).toBe(true);
    expect(
      (screen.getByTestId("curve-channel-checkbox-scale") as HTMLInputElement).checked,
    ).toBe(false);
    expect(
      screen.getByTestId("curve-editor-panel").dataset.focusChannel,
    ).toBe("index");
  });

  // Selecting a non-exclusive curve exits Index solo.
  it("clicking a non-Index row while Index is solo exits solo", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByTestId("curve-channel-row-index"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-index") as HTMLInputElement).checked,
    ).toBe(true);
    fireEvent.click(screen.getByTestId("curve-channel-row-green"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-index") as HTMLInputElement).checked,
    ).toBe(false);
    expect(
      (screen.getByTestId("curve-channel-checkbox-green") as HTMLInputElement).checked,
    ).toBe(true);
  });

  // Rotation is exclusive too (matches Index/Scale).
  it("enabling Rotation via its checkbox hides every other channel (rotation-exclusive)", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByTestId("curve-channel-checkbox-rotation"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-rotation") as HTMLInputElement).checked,
    ).toBe(true);
    for (const id of ["red", "green", "blue", "alpha", "scale", "index"]) {
      expect(
        (screen.getByTestId(`curve-channel-checkbox-${id}`) as HTMLInputElement).checked,
      ).toBe(false);
    }
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-rotation")).toBeInTheDocument();
    });
  });

  it("clicking Rotation while Index is solo swaps solo to Rotation", () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    fireEvent.click(screen.getByTestId("curve-channel-row-index"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-index") as HTMLInputElement).checked,
    ).toBe(true);
    fireEvent.click(screen.getByTestId("curve-channel-row-rotation"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-rotation") as HTMLInputElement).checked,
    ).toBe(true);
    expect(
      (screen.getByTestId("curve-channel-checkbox-index") as HTMLInputElement).checked,
    ).toBe(false);
    expect(
      screen.getByTestId("curve-editor-panel").dataset.focusChannel,
    ).toBe("rotation");
  });

  it("clicking a non-Rotation row while Rotation is solo exits solo", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByTestId("curve-channel-row-rotation"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-rotation") as HTMLInputElement).checked,
    ).toBe(true);
    fireEvent.click(screen.getByTestId("curve-channel-row-green"));
    expect(
      (screen.getByTestId("curve-channel-checkbox-rotation") as HTMLInputElement).checked,
    ).toBe(false);
    expect(
      (screen.getByTestId("curve-channel-checkbox-green") as HTMLInputElement).checked,
    ).toBe(true);
  });

  // ── Hybrid focus-channel restoration ─────────────────────────────

  describe("focus channel", () => {
    it("defaults the focus channel to 'red' (the first visible)", () => {
      const { bridge } = makeStubBridge(null);
      render(<CurveEditorPanel bridge={bridge} />);
      const panel = screen.getByTestId("curve-editor-panel");
      expect(panel.dataset.focusChannel).toBe("red");
      const redRow = screen.getByTestId("curve-channel-row-red");
      expect(redRow.dataset.focus).toBe("true");
    });

    it("clicking a different channel row moves focus + does not toggle visibility off", () => {
      const { bridge } = makeStubBridge(null);
      render(<CurveEditorPanel bridge={bridge} />);
      // Focus starts on red. Click Green row.
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      const panel = screen.getByTestId("curve-editor-panel");
      expect(panel.dataset.focusChannel).toBe("green");
      expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      expect(screen.getByTestId("curve-channel-row-red").dataset.focus).toBe("false");
      // Green was already visible; row click MUST NOT turn it off.
      const greenCb = screen.getByTestId(
        "curve-channel-checkbox-green",
      ) as HTMLInputElement;
      expect(greenCb.checked).toBe(true);
    });

    it("clicking a HIDDEN channel row turns it ON and sets focus", () => {
      const { bridge } = makeStubBridge(null);
      render(<CurveEditorPanel bridge={bridge} />);
      // Index defaults OFF.
      const indexCb = screen.getByTestId(
        "curve-channel-checkbox-index",
      ) as HTMLInputElement;
      expect(indexCb.checked).toBe(false);
      // Click the Index row body (not the checkbox).
      fireEvent.click(screen.getByTestId("curve-channel-row-index"));
      // Visibility flipped ON + focus moved.
      expect(indexCb.checked).toBe(true);
      const panel = screen.getByTestId("curve-editor-panel");
      expect(panel.dataset.focusChannel).toBe("index");
    });

    it("clicking the checkbox itself toggles visibility WITHOUT changing focus", () => {
      const { bridge } = makeStubBridge(null);
      render(<CurveEditorPanel bridge={bridge} />);
      const panel = screen.getByTestId("curve-editor-panel");
      expect(panel.dataset.focusChannel).toBe("red");
      // Click the Green checkbox — should toggle visibility off without
      // moving focus to Green.
      const greenCb = screen.getByTestId(
        "curve-channel-checkbox-green",
      ) as HTMLInputElement;
      fireEvent.click(greenCb);
      expect(greenCb.checked).toBe(false);
      // Focus still on red.
      expect(panel.dataset.focusChannel).toBe("red");
    });

    it("only the focus layer carries data-focus='true'; the others carry data-focus='false'", async () => {
      const { bridge } = makeStubBridge(0);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      const redLayer = screen.getByTestId("curve-layer-red");
      const greenLayer = screen.getByTestId("curve-layer-green");
      expect(redLayer.dataset.focus).toBe("true");
      expect(greenLayer.dataset.focus).toBe("false");
    });

    it("only the focus channel's keys render as interactive curve-key circles", async () => {
      const { bridge } = makeStubBridge(0);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      // Every curve-key on screen should carry data-channel-id === focus
      // (red by default).
      const circles = document.querySelectorAll("[data-testid='curve-key']");
      expect(circles.length).toBeGreaterThan(0);
      for (const c of circles) {
        expect(c.getAttribute("data-channel-id")).toBe("red");
      }
    });

    it("hiding the focus channel via its checkbox auto-moves focus to the next visible channel", () => {
      const { bridge } = makeStubBridge(null);
      render(<CurveEditorPanel bridge={bridge} />);
      const panel = screen.getByTestId("curve-editor-panel");
      // Focus starts on red. Hide red via its checkbox.
      const redCb = screen.getByTestId(
        "curve-channel-checkbox-red",
      ) as HTMLInputElement;
      fireEvent.click(redCb);
      expect(redCb.checked).toBe(false);
      // Focus should auto-move to the next visible channel (green).
      expect(panel.dataset.focusChannel).toBe("green");
    });

    it("focus change clears the selected-keys set", async () => {
      const { bridge } = makeStubBridge(0);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      // Select a key on Red (the default focus).
      const circles = document.querySelectorAll("[data-testid='curve-key']");
      fireEvent.click(circles[0]!);
      const panel = screen.getByTestId("curve-editor-panel");
      expect(Number(panel.dataset.selectedKeyCount)).toBe(1);
      // Move focus to Green.
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      expect(panel.dataset.focusChannel).toBe("green");
      expect(Number(panel.dataset.selectedKeyCount)).toBe(0);
    });
  });

  // ── Edit affordances toolbar ─────────────────────────────────────

  describe("edit toolbar", () => {
    it("renders the Select / Insert mode toggle defaulting to Select", () => {
      const { bridge } = makeStubBridge(null);
      render(<CurveEditorPanel bridge={bridge} />);
      const panel = screen.getByTestId("curve-editor-panel");
      expect(panel.dataset.mode).toBe("select");
      expect(
        screen.getByTestId("ce-tool-select").getAttribute("data-state"),
      ).toBe("on");
      expect(
        screen.getByTestId("ce-tool-insert").getAttribute("data-state"),
      ).toBe("off");
    });

    it("clicking Insert flips the mode to insert", () => {
      const { bridge } = makeStubBridge(null);
      render(<CurveEditorPanel bridge={bridge} />);
      fireEvent.click(screen.getByTestId("ce-tool-insert"));
      const panel = screen.getByTestId("curve-editor-panel");
      expect(panel.dataset.mode).toBe("insert");
      expect(
        screen.getByTestId("ce-tool-insert").getAttribute("data-state"),
      ).toBe("on");
    });

    it("clicking the Smooth interpolation toggle fires set-track-interpolation for the focus channel's track", async () => {
      const { bridge } = makeStubBridge(0);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      fireEvent.click(screen.getByTestId("ce-interp-smooth"));
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls;
      const match = calls.find(
        (call) => (call[0] as { kind: string }).kind === "emitters/set-track-interpolation",
      );
      expect(match).toBeDefined();
      expect(match![0]).toMatchObject({
        kind: "emitters/set-track-interpolation",
        params: { id: 0, track: "red", interpolation: "smooth" },
      });
    });

    it("interpolation toggle reflects the focus track's current interpolation via data-state='on'", async () => {
      // Build a bridge where the red track is "step".
      const listeners: SelectionListener[] = [];
      const bridge = {
        request: vi.fn().mockImplementation((req: { kind: string }) => {
          if (req.kind === "engine/state/snapshot") {
            return Promise.resolve({
              ...makeDefaultEngineState(),
              selectedEmitterId: 0,
            });
          }
          if (req.kind === "emitters/get-tracks") {
            const t = fixtureTracks();
            const red = t.find((x) => x.name === "red")!;
            red.interpolation = "step";
            return Promise.resolve({ tracks: t });
          }
          return Promise.resolve({});
        }),
        on: vi.fn().mockImplementation((kind: string, h: SelectionListener) => {
          if (kind === "emitters/selected") listeners.push(h);
          return () => {
            const idx = listeners.indexOf(h);
            if (idx >= 0) listeners.splice(idx, 1);
          };
        }),
      } as unknown as Bridge & {
        request: ReturnType<typeof vi.fn>;
        on: ReturnType<typeof vi.fn>;
      };
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      expect(
        screen.getByTestId("ce-interp-step").getAttribute("data-state"),
      ).toBe("on");
      expect(
        screen.getByTestId("ce-interp-linear").getAttribute("data-state"),
      ).toBe("off");
    });

    it("Lock-to combo trigger is disabled when the focus track only has 'None' (Red)", () => {
      // Red is the default focus and its Lock-to table is just
      // ["None"] (you can't lock the first channel to anything).
      const { bridge } = makeStubBridge(0);
      render(<CurveEditorPanel bridge={bridge} />);
      const trigger = screen.getByTestId("ce-lock-to-trigger") as HTMLButtonElement;
      expect(trigger).toBeDisabled();
    });

    it("Lock-to combo trigger enables after switching focus to Alpha (4 options)", async () => {
      const { bridge } = makeStubBridge(0);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      // Alpha is hidden by default; clicking the row enables AND
      // focuses it (handleRowClick covers both).
      fireEvent.click(screen.getByTestId("curve-channel-row-alpha"));
      const trigger = screen.getByTestId("ce-lock-to-trigger") as HTMLButtonElement;
      expect(trigger).not.toBeDisabled();
    });
  });

  // ── Time / Value spinners ────────────────────────────────────────

  describe("spinners", () => {
    it("Time + Value spinners are disabled with no selection; populate when one key is selected", async () => {
      // Use the 3-key fixture so we have an interior key.
      const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
      render(<CurveEditorPanel bridge={bridge} />);
      // Scale is hidden + unfocused by default — click its row to
      // enable + focus it before exercising the interior key.
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      selectChannel("scale");
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
      });
      const queryInputs = () => ({
        time: screen.getByTestId("ce-spinner-time-wrapper").querySelector("input") as HTMLInputElement,
        value: screen.getByTestId("ce-spinner-value-wrapper").querySelector("input") as HTMLInputElement,
      });
      const before = queryInputs();
      expect(before.time.disabled).toBe(true);
      expect(before.value.disabled).toBe(true);

      // Click the middle scale key (time=50, value=50).
      const circles = document.querySelectorAll("[data-testid='curve-key']");
      const middle = Array.from(circles).find(
        (c) => c.getAttribute("data-key-time") === "50",
      );
      expect(middle).toBeDefined();
      fireEvent.click(middle!);

      await waitFor(() => {
        const after = queryInputs();
        expect(after.time.disabled).toBe(false);
        expect(after.value.disabled).toBe(false);
        expect(Number(after.time.value)).toBe(50);
        expect(Number(after.value.value)).toBeCloseTo(50, 2);
      });
    });

    it("editing the Value spinner fires set-track-key with newValue", async () => {
      const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      selectChannel("scale");
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
      });
      // Select the middle scale key (time=50).
      const circles = document.querySelectorAll("[data-testid='curve-key']");
      const middle = Array.from(circles).find(
        (c) => c.getAttribute("data-key-time") === "50",
      )!;
      fireEvent.click(middle);
      const valueInput = screen.getByTestId(
        "ce-spinner-value-wrapper",
      ).querySelector("input") as HTMLInputElement;
      await waitFor(() => expect(valueInput.disabled).toBe(false));
      fireEvent.focus(valueInput);
      fireEvent.change(valueInput, { target: { value: "75" } });
      fireEvent.blur(valueInput);
      await waitFor(() => {
        const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls;
        const match = calls.find(
          (call) => (call[0] as { kind: string }).kind === "emitters/set-track-key",
        );
        expect(match).toBeDefined();
        expect(match![0]).toMatchObject({
          kind: "emitters/set-track-key",
          params: { id: 0, track: "scale", oldTime: 50, newTime: 50, newValue: 75 },
        });
      });
    });
  });

  // ── Delete keyboard handler ──────────────────────────────────────

  describe("Delete keyboard handler", () => {
    it("Delete key on the focused panel fires delete-track-keys (border filtered)", async () => {
      const { bridge } = makeStubBridgeWithFocusInteriorKey(7);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      selectChannel("scale");
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
      });
      // Select the middle scale key (time=50).
      const circles = document.querySelectorAll("[data-testid='curve-key']");
      const middle = Array.from(circles).find(
        (c) => c.getAttribute("data-key-time") === "50",
      )!;
      fireEvent.click(middle);
      // Fire a Delete keydown on the document body.
      fireEvent.keyDown(document.body, { key: "Delete" });
      await waitFor(() => {
        const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls;
        const match = calls.find(
          (call) => (call[0] as { kind: string }).kind === "emitters/delete-track-keys",
        );
        expect(match).toBeDefined();
        expect(match![0]).toMatchObject({
          kind: "emitters/delete-track-keys",
          params: { id: 7, track: "scale", times: [50] },
        });
      });
    });

    it("Delete inside an <input> does NOT fire delete-track-keys", async () => {
      const { bridge } = makeStubBridgeWithFocusInteriorKey(7);
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      selectChannel("scale");
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
      });
      // Select an interior key.
      const circles = document.querySelectorAll("[data-testid='curve-key']");
      const middle = Array.from(circles).find(
        (c) => c.getAttribute("data-key-time") === "50",
      )!;
      fireEvent.click(middle);
      // Now focus the Value spinner input and fire Delete on it.
      const valueInput = screen.getByTestId(
        "ce-spinner-value-wrapper",
      ).querySelector("input") as HTMLInputElement;
      valueInput.focus();
      fireEvent.keyDown(valueInput, { key: "Delete" });
      // No delete-track-keys call should appear.
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls;
      const match = calls.find(
        (call) => (call[0] as { kind: string }).kind === "emitters/delete-track-keys",
      );
      expect(match).toBeUndefined();
    });
  });

  // ── Insert mode → add-track-key ──────────────────────────────────

  it("Insert mode + canvas pointer-down fires add-track-key", async () => {
    const { bridge } = makeStubBridge(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByTestId("ce-tool-insert"));
    const backdrop = screen.getByTestId("curve-canvas-backdrop");
    // jsdom getBoundingClientRect returns zeros, so eventToViewBox
    // returns NaN → no event fires. We need to stub it.
    Object.defineProperty(backdrop, "ownerSVGElement", {
      value: backdrop.parentElement,
      configurable: true,
    });
    const svg = screen.getByTestId("curve-editor-svg");
    svg.getBoundingClientRect = () => ({
      x: 0, y: 0, top: 0, left: 0, right: 600, bottom: 300,
      width: 600, height: 300, toJSON: () => ({}),
    } as DOMRect);
    fireEvent.pointerDown(backdrop, {
      button: 0,
      clientX: 300,
      clientY: 150,
      pointerId: 1,
    });
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls;
      const match = calls.find(
        (call) => (call[0] as { kind: string }).kind === "emitters/add-track-key",
      );
      expect(match).toBeDefined();
      expect((match![0] as { params: { track: string } }).params.track).toBe("red");
    });
  });

  // ── Locked focus channel — panel gating ─────────────────────────
  describe("locked focus channel — panel gating", () => {
    it("commits no mutating track command from drag, insert-click, context-menu, or marquee on a locked focus", async () => {
      // Build a stub bridge returning lockedFixtureTracks (green→red).
      const { bridge } = makeStubBridge(1, lockedFixtureTracks());

      render(<CurveEditorPanel bridge={bridge} />);

      // Wait for curves to load.
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });

      // Focus the green channel (locked to red).
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      });

      // Snapshot the call count AFTER setup (get-tracks, snapshot, etc.)
      // so we only catch new calls.
      const callsBefore = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.length;

      // Attempt 1: pointer drag on a curve-key.
      const keys = document.querySelectorAll("[data-testid='curve-key']");
      // Hard assertion — if the testid renames, we catch it here rather than
      // silently skipping the gesture attempts below.
      expect(keys.length).toBeGreaterThan(0);
      fireEvent.pointerDown(keys[0]!, { button: 0, pointerId: 1, clientX: 10, clientY: 10 });
      fireEvent.pointerMove(keys[0]!, { pointerId: 1, clientX: 20, clientY: 20 });
      fireEvent.pointerUp(keys[0]!, { pointerId: 1 });

      // Attempt 2: context menu on a curve-key (key context menu should not appear).
      fireEvent.contextMenu(keys[0]!);
      expect(screen.queryByTestId("ce-key-context-menu-delete")).toBeNull();

      // Attempt 3: backdrop pointer-down + move + up (marquee-style).
      const backdrop = screen.queryByTestId("curve-canvas-backdrop");
      // Hard assertion — if the testid renames, we catch it here rather than
      // silently skipping the backdrop gesture below.
      expect(backdrop).not.toBeNull();
      fireEvent.pointerDown(backdrop!, { button: 0, pointerId: 2, clientX: 5, clientY: 5 });
      fireEvent.pointerMove(backdrop!, { pointerId: 2, clientX: 50, clientY: 50 });
      fireEvent.pointerUp(backdrop!, { pointerId: 2 });

      // Assert: NO mutating track commands were issued.
      const newCalls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls
        .slice(callsBefore)
        .map((call) => (call[0] as { kind: string }).kind);

      const mutatingKinds = [
        "emitters/set-track-key",
        "emitters/add-track-key",
        "emitters/delete-track-keys",
      ];
      for (const kind of mutatingKinds) {
        expect(newCalls).not.toContain(kind);
      }
    });

    it("refuses Delete and spinner commits when the lock lands under an existing selection (mid-gesture race)", async () => {
      // Build a bridge with a MUTABLE tracks variable.  Green starts
      // UNLOCKED with an interior key at t=50 so Delete can actually
      // attempt a delete-track-keys call before the guard closes it.
      const unlockedGreenWith3Keys: TrackDto[] = TRACK_NAMES.map((name) => ({
        name,
        keys: name === "green"
          ? [{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]
          : [{ time: 0, value: 0 }, { time: 100, value: name === "rotationSpeed" ? -1 : 1 }],
        interpolation: "linear" as const,
        lockedTo: null,
      }));
      const lockedGreenWith3Keys: TrackDto[] = unlockedGreenWith3Keys.map((t) => ({
        ...t,
        lockedTo: t.name === "green" ? "red" : null,
      }));

      let currentTracks: TrackDto[] = unlockedGreenWith3Keys;
      const selectionListeners: SelectionListener[] = [];
      const treeChangedListeners: Array<() => void> = [];
      const bridge = {
        request: vi.fn().mockImplementation((req: { kind: string }) => {
          if (req.kind === "engine/state/snapshot") {
            return Promise.resolve({
              ...makeDefaultEngineState(),
              selectedEmitterId: 1,
            });
          }
          if (req.kind === "emitters/get-tracks") {
            return Promise.resolve({ tracks: currentTracks });
          }
          if (req.kind === "emitters/add-track-key") {
            const p = (req as unknown as { params: { time: number; value: number } }).params;
            return Promise.resolve({ time: p.time, value: p.value });
          }
          return Promise.resolve({});
        }),
        on: vi.fn().mockImplementation((kind: string, h: unknown) => {
          if (kind === "emitters/selected") selectionListeners.push(h as SelectionListener);
          if (kind === "emitters/tree/changed") treeChangedListeners.push(h as () => void);
          return () => {
            if (kind === "emitters/selected") {
              const idx = selectionListeners.indexOf(h as SelectionListener);
              if (idx >= 0) selectionListeners.splice(idx, 1);
            }
            if (kind === "emitters/tree/changed") {
              const idx = treeChangedListeners.indexOf(h as () => void);
              if (idx >= 0) treeChangedListeners.splice(idx, 1);
            }
          };
        }),
      } as unknown as Bridge & {
        request: ReturnType<typeof vi.fn>;
        on: ReturnType<typeof vi.fn>;
      };

      render(<CurveEditorPanel bridge={bridge} />);

      // Wait for initial curves to load (red layer is always present).
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });

      // Focus the green channel (currently UNLOCKED).
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      });

      // Wait for the green layer to mount (confirming green is focused + tracks loaded).
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-green")).toBeInTheDocument();
      });

      // Click the interior green key at t=50 to create a live selection.
      const greenInterior = Array.from(
        document.querySelectorAll("[data-testid='curve-key']"),
      ).find((c) => c.getAttribute("data-key-time") === "50");
      expect(greenInterior).toBeDefined();
      fireEvent.click(greenInterior!);

      // Verify selection took.
      const panel = screen.getByTestId("curve-editor-panel");
      await waitFor(() => {
        expect(panel.getAttribute("data-selected-key-count")).toBe("1");
      });

      // ── Mid-gesture race: flip tracks to locked, trigger refetch ──
      currentTracks = lockedGreenWith3Keys;
      // Fire emitters/tree/changed to force a tracks refetch — the same path a
      // link-group sibling propagates a lock while a selection is live.
      for (const l of treeChangedListeners) l();

      // Wait for the lock to land — the lock-to trigger reflects it.
      await waitFor(() => {
        expect(
          screen.getByTestId("ce-lock-to-trigger").getAttribute("data-locked"),
        ).toBe("true");
      });

      // Selection must survive the refetch that delivered the lock — a future
      // change that clears selection on refetch would make subsequent guards
      // pass vacuously.
      expect(panel.getAttribute("data-selected-key-count")).toBe("1");
      const greenInteriorAfterLock = Array.from(
        document.querySelectorAll("[data-testid='curve-key']"),
      ).find((c) => c.getAttribute("data-key-time") === "50");
      expect(greenInteriorAfterLock?.getAttribute("data-selected")).toBe("true");

      // Clear call history so we only see calls made AFTER the lock landed.
      (bridge.request as ReturnType<typeof vi.fn>).mockClear();

      // ── Assert Delete is blocked ──
      fireEvent.keyDown(window, { key: "Delete" });

      // ── Assert spinner is disabled (focusLocked disables spinners after the guard) ──
      const valueInput = screen
        .getByTestId("ce-spinner-value-wrapper")
        .querySelector("input") as HTMLInputElement;
      expect(valueInput.disabled).toBe(true);

      // Allow any async microtasks to flush before checking.
      await new Promise((r) => setTimeout(r, 0));

      // Neither a delete nor a set-track-key call should have been issued.
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map(
        (c) => (c[0] as { kind: string }).kind,
      );
      expect(calls).not.toContain("emitters/delete-track-keys");
      expect(calls).not.toContain("emitters/set-track-key");
    });

    it("blocks a spinner blur-commit that was started pre-lock and fires after the lock lands", async () => {
      // Exercises the handleValueSpinner guard: a user begins editing the
      // value spinner input while the track is unlocked, then the lock arrives
      // (tree/changed refetch) before they blur.  The blur fires onChange which
      // calls handleValueSpinner — but the guard (focusLocked check at the top
      // of that callback) must prevent the emitters/set-track-key bridge call.
      //
      // Note: disabled arriving on the input mid-edit does NOT remove React's
      // pending onBlur — the handler still fires, making this scenario testable
      // in jsdom without any special tricks.
      const unlockedTracks: TrackDto[] = TRACK_NAMES.map((name) => ({
        name,
        keys: name === "green"
          ? [{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]
          : [{ time: 0, value: 0 }, { time: 100, value: name === "rotationSpeed" ? -1 : 1 }],
        interpolation: "linear" as const,
        lockedTo: null,
      }));
      const lockedTracks: TrackDto[] = unlockedTracks.map((t) => ({
        ...t,
        lockedTo: t.name === "green" ? "red" : null,
      }));

      let currentTracks: TrackDto[] = unlockedTracks;
      const treeChangedListeners: Array<() => void> = [];
      const bridge = {
        request: vi.fn().mockImplementation((req: { kind: string }) => {
          if (req.kind === "engine/state/snapshot") {
            return Promise.resolve({ ...makeDefaultEngineState(), selectedEmitterId: 1 });
          }
          if (req.kind === "emitters/get-tracks") {
            return Promise.resolve({ tracks: currentTracks });
          }
          if (req.kind === "emitters/add-track-key") {
            const p = (req as unknown as { params: { time: number; value: number } }).params;
            return Promise.resolve({ time: p.time, value: p.value });
          }
          return Promise.resolve({});
        }),
        on: vi.fn().mockImplementation((kind: string, h: unknown) => {
          if (kind === "emitters/tree/changed") treeChangedListeners.push(h as () => void);
          return () => {
            if (kind === "emitters/tree/changed") {
              const idx = treeChangedListeners.indexOf(h as () => void);
              if (idx >= 0) treeChangedListeners.splice(idx, 1);
            }
          };
        }),
      } as unknown as Bridge & {
        request: ReturnType<typeof vi.fn>;
        on: ReturnType<typeof vi.fn>;
      };

      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });

      // Focus the green channel (currently UNLOCKED).
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      });
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-green")).toBeInTheDocument();
      });

      // Select the interior green key at t=50.
      const greenInterior = Array.from(
        document.querySelectorAll("[data-testid='curve-key']"),
      ).find((c) => c.getAttribute("data-key-time") === "50");
      expect(greenInterior).toBeDefined();
      fireEvent.click(greenInterior!);

      const panel = screen.getByTestId("curve-editor-panel");
      await waitFor(() => {
        expect(panel.getAttribute("data-selected-key-count")).toBe("1");
      });

      // Grab the value spinner input and begin an edit while the track is unlocked.
      const valueInput = screen
        .getByTestId("ce-spinner-value-wrapper")
        .querySelector("input") as HTMLInputElement;
      await waitFor(() => expect(valueInput.disabled).toBe(false));
      fireEvent.focus(valueInput);
      fireEvent.change(valueInput, { target: { value: "0.99" } });
      // At this point the user has typed a new value but NOT yet blurred.

      // ── Race: flip the track to locked and trigger refetch ──
      currentTracks = lockedTracks;
      for (const l of treeChangedListeners) l();

      // Wait for the lock to land.
      await waitFor(() => {
        expect(
          screen.getByTestId("ce-lock-to-trigger").getAttribute("data-locked"),
        ).toBe("true");
      });
      // Spinner must now be disabled (focusLocked disables it on re-render).
      expect(valueInput.disabled).toBe(true);

      // Clear call history — only calls from the blur-commit onward matter.
      (bridge.request as ReturnType<typeof vi.fn>).mockClear();

      // Blur the input: this fires Spinner's onBlur → commit(text) → onChange
      // → handleValueSpinner.  The focusLocked guard must intercept it.
      fireEvent.blur(valueInput);
      await new Promise((r) => setTimeout(r, 0));

      const afterCalls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map(
        (c) => (c[0] as { kind: string }).kind,
      );
      expect(afterCalls).not.toContain("emitters/set-track-key");
    });

    it("does not start a marquee from the axis gutter when focus is locked", async () => {
      // The panel's gutter handler guards: if (mode === "select" && !focusLocked)
      // — with a locked focus channel the guard must block startMarquee so no
      // marquee rect mounts and no mutating calls are issued.
      const { bridge } = makeStubBridge(1, lockedFixtureTracks());

      const { container } = render(<CurveEditorPanel bridge={bridge} />);

      // Wait for curves to load.
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });

      // Focus the green channel (locked to red).
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      });

      const callsBefore = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.length;

      // Fire a primary pointerDown on the gutter wrapper (outside the plot SVG).
      // CanvasWithAxisLabels routes this to onGutterPointerDown, which the panel
      // gates behind !focusLocked — so startMarquee must not be called.
      const gutterWrapper = container.querySelector("[data-testid='curve-canvas-with-axes']");
      expect(gutterWrapper).not.toBeNull();
      fireEvent.pointerDown(gutterWrapper!, { button: 0, pointerId: 3, clientX: 5, clientY: 5 });

      // No marquee rect should have mounted.
      expect(container.querySelector("[data-testid='curve-marquee']")).toBeNull();

      // No mutating bridge calls either.
      const newCalls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls
        .slice(callsBefore)
        .map((call) => (call[0] as { kind: string }).kind);

      const mutatingKinds = [
        "emitters/set-track-key",
        "emitters/add-track-key",
        "emitters/delete-track-keys",
      ];
      for (const kind of mutatingKinds) {
        expect(newCalls).not.toContain(kind);
      }
    });

    it("forces Insert mode back to Select when the focus channel is locked, and disables the toggle", async () => {
      // Build a bridge with a MUTABLE tracks variable — green starts UNLOCKED.
      let currentTracks: TrackDto[] = fixtureTracks(); // all unlocked
      const treeChangedListeners: Array<() => void> = [];
      const bridge = {
        request: vi.fn().mockImplementation((req: { kind: string }) => {
          if (req.kind === "engine/state/snapshot") {
            return Promise.resolve({
              ...makeDefaultEngineState(),
              selectedEmitterId: 1,
            });
          }
          if (req.kind === "emitters/get-tracks") {
            return Promise.resolve({ tracks: currentTracks });
          }
          return Promise.resolve({});
        }),
        on: vi.fn().mockImplementation((kind: string, h: unknown) => {
          if (kind === "emitters/tree/changed") treeChangedListeners.push(h as () => void);
          return () => {
            if (kind === "emitters/tree/changed") {
              const idx = treeChangedListeners.indexOf(h as () => void);
              if (idx >= 0) treeChangedListeners.splice(idx, 1);
            }
          };
        }),
      } as unknown as Bridge & {
        request: ReturnType<typeof vi.fn>;
        on: ReturnType<typeof vi.fn>;
      };

      render(<CurveEditorPanel bridge={bridge} />);

      // Wait for curves to load.
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });

      // Focus the green channel (currently UNLOCKED).
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      });

      // Switch to Insert mode — both buttons should be interactive and mode should flip.
      fireEvent.click(screen.getByTestId("ce-tool-insert"));
      await waitFor(() => {
        expect(screen.getByTestId("ce-tool-insert").getAttribute("data-state")).toBe("on");
      });

      // ── Flip fixture to locked + trigger refetch ──
      currentTracks = lockedFixtureTracks(); // green → red
      for (const l of treeChangedListeners) l();

      // Wait for the lock to land.
      await waitFor(() => {
        expect(
          screen.getByTestId("ce-lock-to-trigger").getAttribute("data-locked"),
        ).toBe("true");
      });

      // Insert button must revert to off; both buttons must be disabled.
      expect(screen.getByTestId("ce-tool-insert").getAttribute("data-state")).toBe("off");
      expect(screen.getByTestId("ce-tool-insert")).toBeDisabled();
      expect(screen.getByTestId("ce-tool-select").getAttribute("data-state")).toBe("on");
      expect(screen.getByTestId("ce-tool-select")).toBeDisabled();

      // span-shim: both buttons must still be in the DOM (the inline-block
      // wrapper span must not swallow the testid or prevent discovery).
      expect(screen.getByTestId("ce-tool-select").closest("span.inline-block")).not.toBeNull();
      expect(screen.getByTestId("ce-tool-insert").closest("span.inline-block")).not.toBeNull();

      // Clear call history so we only catch calls after the lock landed.
      (bridge.request as ReturnType<typeof vi.fn>).mockClear();

      // Pointer-down on the canvas backdrop must not issue add-track-key.
      const backdrop = screen.queryByTestId("curve-canvas-backdrop");
      expect(backdrop).not.toBeNull();
      fireEvent.pointerDown(backdrop!, { button: 0, pointerId: 1, clientX: 5, clientY: 5 });

      await new Promise((r) => setTimeout(r, 0));

      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map(
        (c) => (c[0] as { kind: string }).kind,
      );
      expect(calls).not.toContain("emitters/add-track-key");
    });

    it("Ctrl+X refuses but Ctrl+C still copies under a lock landed mid-selection", async () => {
      // Race-test scaffold: select t=50 unlocked → flip fixture → fire tree/changed → waitFor data-locked.
      const unlockedGreenWith3Keys: TrackDto[] = TRACK_NAMES.map((name) => ({
        name,
        keys: name === "green"
          ? [{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]
          : [{ time: 0, value: 0 }, { time: 100, value: name === "rotationSpeed" ? -1 : 1 }],
        interpolation: "linear" as const,
        lockedTo: null,
      }));
      const lockedGreenWith3Keys: TrackDto[] = unlockedGreenWith3Keys.map((t) => ({
        ...t,
        lockedTo: t.name === "green" ? "red" : null,
      }));

      let currentTracks: TrackDto[] = unlockedGreenWith3Keys;
      const treeChangedListeners: Array<() => void> = [];
      const bridge = {
        request: vi.fn().mockImplementation((req: { kind: string }) => {
          if (req.kind === "engine/state/snapshot") {
            return Promise.resolve({ ...makeDefaultEngineState(), selectedEmitterId: 1 });
          }
          if (req.kind === "emitters/get-tracks") {
            return Promise.resolve({ tracks: currentTracks });
          }
          if (req.kind === "emitters/add-track-key") {
            const p = (req as unknown as { params: { time: number; value: number } }).params;
            return Promise.resolve({ time: p.time, value: p.value });
          }
          return Promise.resolve({});
        }),
        on: vi.fn().mockImplementation((kind: string, h: unknown) => {
          if (kind === "emitters/tree/changed") treeChangedListeners.push(h as () => void);
          return () => {
            const idx = treeChangedListeners.indexOf(h as () => void);
            if (idx >= 0) treeChangedListeners.splice(idx, 1);
          };
        }),
      } as unknown as Bridge & {
        request: ReturnType<typeof vi.fn>;
        on: ReturnType<typeof vi.fn>;
      };

      render(<CurveEditorPanel bridge={bridge} />);

      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });

      // Focus the green channel (currently UNLOCKED).
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      });
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-green")).toBeInTheDocument();
      });

      // Select the interior green key at t=50.
      const greenInterior = Array.from(
        document.querySelectorAll("[data-testid='curve-key']"),
      ).find((c) => c.getAttribute("data-key-time") === "50");
      expect(greenInterior).toBeDefined();
      fireEvent.click(greenInterior!);

      const panel = screen.getByTestId("curve-editor-panel");
      await waitFor(() => {
        expect(panel.getAttribute("data-selected-key-count")).toBe("1");
      });

      // ── Mid-gesture race: flip tracks to locked, trigger refetch ──
      currentTracks = lockedGreenWith3Keys;
      for (const l of treeChangedListeners) l();

      // Wait for the lock to land.
      await waitFor(() => {
        expect(
          screen.getByTestId("ce-lock-to-trigger").getAttribute("data-locked"),
        ).toBe("true");
      });

      // Selection must survive the refetch (guards would pass vacuously if cleared).
      expect(panel.getAttribute("data-selected-key-count")).toBe("1");

      // Clear call history so we only see calls issued after the lock landed.
      (bridge.request as ReturnType<typeof vi.fn>).mockClear();

      // ── Ctrl+X must be refused (delete-track-keys must not fire) ──
      fireEvent.keyDown(document.body, { key: "x", ctrlKey: true });
      await new Promise((r) => setTimeout(r, 0));
      const afterCut = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map(
        (c) => (c[0] as { kind: string }).kind,
      );
      expect(afterCut).not.toContain("emitters/delete-track-keys");

      // ── Ctrl+C must still copy the selection into the clipboard ──
      // Reset the clipboard so we get a clean read.
      setCurveKeysClipboard([]);
      fireEvent.keyDown(document.body, { key: "c", ctrlKey: true });
      await waitFor(() => expect(getCurveKeysClipboard()).toHaveLength(1));
      expect(getCurveKeysClipboard()[0]).toMatchObject({ time: 50, value: 0.5 });

      // Belt-and-braces note: handleKeyDragEnd, handleCanvasAdd, and
      // handleGroupDragEnd each carry a focusLocked early-return
      // (defense-in-depth). Those handlers are not directly invocable via
      // jsdom (the renderer withholds drag props and the canvas is not
      // exercisable at pixel level in a unit test), so direct invocation
      // is not meaningful here. The race guard above (zero mutating calls
      // after lock lands) covers the mid-gesture lock scenario that motivates
      // those guards; the guards themselves are belt-and-braces for paths
      // the renderer's prop-withholding might not reach in all future refactors.
    });

    it("shows the lock glyph with master-naming aria-label only while locked", async () => {
      // ── Locked half ──
      const { bridge: lockedBridge } = makeStubBridge(1, lockedFixtureTracks());
      const { unmount } = render(<CurveEditorPanel bridge={lockedBridge} />);

      // Wait for the lock trigger to appear (curves loaded + green focused).
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(
          screen.getByTestId("ce-lock-to-trigger").getAttribute("data-locked"),
        ).toBe("true");
      });

      expect(screen.getByTestId("ce-lock-glyph").getAttribute("aria-label")).toBe(
        "Green is locked to Red — read-only",
      );

      unmount();

      // ── Unlocked half ──
      const { bridge: unlockedBridge } = makeStubBridge(1, fixtureTracks());
      render(<CurveEditorPanel bridge={unlockedBridge} />);

      // Wait for curves to load then focus green.
      await waitFor(() => {
        expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
      });
      fireEvent.click(screen.getByTestId("curve-channel-row-green"));
      await waitFor(() => {
        expect(screen.getByTestId("curve-channel-row-green").dataset.focus).toBe("true");
      });

      expect(screen.queryByTestId("ce-lock-glyph")).toBeNull();
    });
  });
});

/** Helper bridge for spinner / delete specs — adds a middle key on
 *  the scale track so we have an interior (non-border) key to act on. */
function makeStubBridgeWithFocusInteriorKey(initialSelectedId: number) {
  const listeners: SelectionListener[] = [];
  const tracksWithMiddleKey: TrackDto[] = TRACK_NAMES.map((name) => ({
    name,
    keys: name === "scale"
      ? [
          { time: 0,   value: 0 },
          { time: 50,  value: 50 },
          { time: 100, value: 100 },
        ]
      : [
          { time: 0,   value: 0 },
          { time: 100, value: name === "rotationSpeed" ? -1 : 1 },
        ],
    interpolation: "linear",
    lockedTo: null,
  }));
  const bridge = {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") {
        return Promise.resolve({
          ...makeDefaultEngineState(),
          selectedEmitterId: initialSelectedId,
        });
      }
      if (req.kind === "emitters/get-tracks") {
        return Promise.resolve({ tracks: tracksWithMiddleKey });
      }
      if (req.kind === "emitters/add-track-key") {
        const p = (req as unknown as { params: { time: number; value: number } }).params;
        return Promise.resolve({ time: p.time, value: p.value });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockImplementation((kind: string, h: SelectionListener) => {
      if (kind === "emitters/selected") listeners.push(h);
      return () => {
        const idx = listeners.indexOf(h);
        if (idx >= 0) listeners.splice(idx, 1);
      };
    }),
  } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
    on: ReturnType<typeof vi.fn>;
  };
  return { bridge };
}

/** Bridge whose `scale` track has THREE interior keys (25/50/75) plus
 *  the two borders (0/100), so multi-key average specs have a real
 *  group to act on. */
function makeStubBridgeMultiInterior(initialSelectedId: number) {
  const listeners: SelectionListener[] = [];
  const tracks: TrackDto[] = TRACK_NAMES.map((name) => ({
    name,
    keys: name === "scale"
      ? [
          { time: 0,   value: 0 },
          { time: 25,  value: 20 },
          { time: 50,  value: 40 },
          { time: 75,  value: 60 },
          { time: 100, value: 80 },
        ]
      : [
          { time: 0,   value: 0 },
          { time: 100, value: name === "rotationSpeed" ? -1 : 1 },
        ],
    interpolation: "linear",
    lockedTo: null,
  }));
  const bridge = {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") {
        return Promise.resolve({
          ...makeDefaultEngineState(),
          selectedEmitterId: initialSelectedId,
        });
      }
      if (req.kind === "emitters/get-tracks") {
        return Promise.resolve({ tracks });
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockImplementation((kind: string, h: SelectionListener) => {
      if (kind === "emitters/selected") listeners.push(h);
      return () => {
        const idx = listeners.indexOf(h);
        if (idx >= 0) listeners.splice(idx, 1);
      };
    }),
  } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
    on: ReturnType<typeof vi.fn>;
  };
  return { bridge };
}

// Multi-key average edit (shift-by-delta / preserve spread).
describe("CurveEditorPanel — multi-key average edit", () => {
  /** Click a focus-channel key by its time (ctrl for additive). */
  function clickKey(time: number, additive: boolean) {
    const el = screen
      .getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === String(time));
    if (el === undefined) throw new Error(`no curve-key at time ${time}`);
    fireEvent.click(el, additive ? { ctrlKey: true } : {});
  }

  function setTrackKeyCalls(bridge: { request: ReturnType<typeof vi.fn> }) {
    return bridge.request.mock.calls
      .map((c) => c[0] as { kind: string; params: { oldTime: number; newTime: number; newValue: number } })
      .filter((c) => c.kind === "emitters/set-track-key")
      .map((c) => c.params);
  }

  async function selectScaleInterior(bridge: unknown) {
    render(<CurveEditorPanel bridge={bridge as Bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    // Solo + focus the scale channel, then select its 3 interior keys.
    fireEvent.click(screen.getByTestId("curve-channel-row-scale"));
    clickKey(25, false);
    clickKey(50, true);
    clickKey(75, true);
  }

  it("shows the AVERAGE time/value of the selected keys", async () => {
    const { bridge } = makeStubBridgeMultiInterior(0);
    await selectScaleInterior(bridge);
    // avg time = (25+50+75)/3 = 50; avg value = (20+40+60)/3 = 40.
    // Time spinner (step 0.1) renders at the app-wide 2dp default.
    expect(
      (screen.getByLabelText("Selected key time") as HTMLInputElement).value,
    ).toBe("50.00");
    // Scale track (step 0.1) now renders at the app-wide 2dp default.
    expect(
      (screen.getByLabelText("Selected key value") as HTMLInputElement).value,
    ).toBe("40.00");
  });

  it("editing the Value average shifts every selected key by the delta", async () => {
    const { bridge } = makeStubBridgeMultiInterior(0);
    await selectScaleInterior(bridge);
    const valueInput = screen.getByLabelText("Selected key value") as HTMLInputElement;
    fireEvent.change(valueInput, { target: { value: "50" } }); // avg 40 → +10
    fireEvent.blur(valueInput);
    const calls = setTrackKeyCalls(bridge);
    const byOld = new Map(calls.map((c) => [c.oldTime, c]));
    expect(byOld.get(25)).toMatchObject({ oldTime: 25, newTime: 25, newValue: 30 });
    expect(byOld.get(50)).toMatchObject({ oldTime: 50, newTime: 50, newValue: 50 });
    expect(byOld.get(75)).toMatchObject({ oldTime: 75, newTime: 75, newValue: 70 });
  });

  it("editing the Time average shifts the group's times by the delta", async () => {
    const { bridge } = makeStubBridgeMultiInterior(0);
    await selectScaleInterior(bridge);
    const timeInput = screen.getByLabelText("Selected key time") as HTMLInputElement;
    fireEvent.change(timeInput, { target: { value: "60" } }); // avg 50 → +10
    fireEvent.blur(timeInput);
    const calls = setTrackKeyCalls(bridge);
    const byOld = new Map(calls.map((c) => [c.oldTime, c]));
    expect(byOld.get(25)!.newTime).toBeCloseTo(35, 3);
    expect(byOld.get(50)!.newTime).toBeCloseTo(60, 3);
    expect(byOld.get(75)!.newTime).toBeCloseTo(85, 3);
    // Values untouched by a pure time shift.
    expect(byOld.get(50)!.newValue).toBe(40);
  });

  it("disables the Time field when the selection is all border keys", async () => {
    const { bridge } = makeStubBridgeMultiInterior(0);
    render(<CurveEditorPanel bridge={bridge as Bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    fireEvent.click(screen.getByTestId("curve-channel-row-scale"));
    clickKey(0, false);
    clickKey(100, true);
    expect(
      (screen.getByLabelText("Selected key time") as HTMLInputElement).disabled,
    ).toBe(true);
    // Value stays editable (border values can move).
    expect(
      (screen.getByLabelText("Selected key value") as HTMLInputElement).disabled,
    ).toBe(false);
  });
});

// The curve Time spinner uses a 0.1 step (legacy granularity) and
// displays at the app-wide 2dp default (decoupled from step).
describe("CurveEditorPanel — decimal-grained time", () => {
  it("Time spinner displays the selected key time at 2 decimal places", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    selectChannel("scale");
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
    });
    const middle = Array.from(
      document.querySelectorAll("[data-testid='curve-key']"),
    ).find((c) => c.getAttribute("data-key-time") === "50")!;
    fireEvent.click(middle);
    await waitFor(() => {
      const timeInput = screen.getByLabelText("Selected key time") as HTMLInputElement;
      expect(timeInput.disabled).toBe(false);
      expect(timeInput.value).toBe("50.00");
    });
  });

  it("ArrowUp on the Time spinner nudges by 0.1 (not 1)", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    selectChannel("scale");
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
    });
    const middle = Array.from(
      document.querySelectorAll("[data-testid='curve-key']"),
    ).find((c) => c.getAttribute("data-key-time") === "50")!;
    fireEvent.click(middle);
    const timeInput = screen.getByLabelText("Selected key time") as HTMLInputElement;
    await waitFor(() => expect(timeInput.disabled).toBe(false));
    fireEvent.keyDown(timeInput, { key: "ArrowUp" });
    await waitFor(() => {
      const move = (bridge.request as ReturnType<typeof vi.fn>).mock.calls
        .map((c) => c[0] as { kind: string; params: { newTime: number } })
        .find((c) => c.kind === "emitters/set-track-key");
      expect(move).toBeDefined();
      expect(move!.params.newTime).toBeCloseTo(50.1, 3);
    });
  });
});

// Right-click on the empty curve canvas. In Select mode it clears
// the selection (legacy WM_RBUTTONDOWN → CM_SELECT); in Insert mode it
// drops back to Select mode without deselecting (legacy CM_INSERT branch).
describe("CurveEditorPanel — right-click deselect", () => {
  async function focusScaleWithSelection() {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });
    selectChannel("scale");
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument();
    });
    const middle = Array.from(
      document.querySelectorAll("[data-testid='curve-key']"),
    ).find((c) => c.getAttribute("data-key-time") === "50")!;
    fireEvent.click(middle);
    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("1"));
    return { bridge, panel };
  }

  it("Select mode: right-click on empty canvas clears the selection", async () => {
    const { panel } = await focusScaleWithSelection();
    expect(panel.getAttribute("data-mode")).toBe("select");
    fireEvent.contextMenu(screen.getByTestId("curve-canvas-backdrop"));
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("0"));
    // Mode stays Select.
    expect(panel.getAttribute("data-mode")).toBe("select");
  });

  it("Insert mode: right-click drops back to Select mode WITHOUT deselecting", async () => {
    const { panel } = await focusScaleWithSelection();
    fireEvent.click(screen.getByTestId("ce-tool-insert"));
    expect(panel.getAttribute("data-mode")).toBe("insert");
    fireEvent.contextMenu(screen.getByTestId("curve-canvas-backdrop"));
    await waitFor(() => expect(panel.getAttribute("data-mode")).toBe("select"));
    // Selection is preserved (legacy CM_INSERT branch doesn't deselect).
    expect(panel.getAttribute("data-selected-key-count")).toBe("1");
  });
});

// Copy / Cut / Paste of selected curve keys via Ctrl+C / X / V,
// matching legacy CurveEditor.cpp CopyKeys / PasteKeys. Window-scoped (SVG
// clicks don't move DOM focus into the panel), with a TYPING_TAGS guard and
// an emitter-tree-origin guard so the two clipboards never both fire.
describe("CurveEditorPanel — key copy/cut/paste", () => {
  beforeEach(() => setCurveKeysClipboard([]));

  function clickKeyByTime(time: number, additive = false) {
    const el = Array.from(
      document.querySelectorAll("[data-testid='curve-key']"),
    ).find((c) => c.getAttribute("data-key-time") === String(time));
    if (el === undefined) throw new Error(`no curve-key at time ${time}`);
    fireEvent.click(el, additive ? { ctrlKey: true } : {});
  }

  function addTrackKeyCalls(bridge: { request: ReturnType<typeof vi.fn> }) {
    return bridge.request.mock.calls
      .map((c) => c[0] as { kind: string; params: { track: string; time: number; value: number } })
      .filter((c) => c.kind === "emitters/add-track-key");
  }

  async function focusScale(bridge: Bridge) {
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument());
    selectChannel("scale");
    await waitFor(() => expect(screen.getByTestId("curve-layer-scale")).toBeInTheDocument());
  }

  it("Ctrl+C copies the selected keys' {time,value} to the clipboard", async () => {
    const { bridge } = makeStubBridgeMultiInterior(0);
    await focusScale(bridge);
    clickKeyByTime(25);
    clickKeyByTime(50, true);
    fireEvent.keyDown(document.body, { key: "c", ctrlKey: true });
    await waitFor(() => expect(getCurveKeysClipboard()).toHaveLength(2));
    const byTime = new Map(getCurveKeysClipboard().map((k) => [k.time, k.value]));
    expect(byTime.get(25)).toBe(20);
    expect(byTime.get(50)).toBe(40);
  });

  it("Ctrl+V adds one key per clipboard entry on the focus track + selects the results", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    await focusScale(bridge);
    clickKeyByTime(50);
    fireEvent.keyDown(document.body, { key: "c", ctrlKey: true });
    await waitFor(() => expect(getCurveKeysClipboard()).toHaveLength(1));
    fireEvent.keyDown(document.body, { key: "v", ctrlKey: true });
    await waitFor(() => {
      const adds = addTrackKeyCalls(bridge);
      expect(adds).toHaveLength(1);
      expect(adds[0]!.params).toMatchObject({ track: "scale", time: 50, value: 50 });
    });
    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("1"));
  });

  it("Ctrl+X copies the selection then deletes the non-border keys", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    await focusScale(bridge);
    clickKeyByTime(50);
    fireEvent.keyDown(document.body, { key: "x", ctrlKey: true });
    await waitFor(() => {
      const del = (bridge.request as ReturnType<typeof vi.fn>).mock.calls
        .map((c) => c[0] as { kind: string; params: { times: number[] } })
        .find((c) => c.kind === "emitters/delete-track-keys");
      expect(del).toBeDefined();
      expect(del!.params.times).toEqual([50]);
    });
    expect(getCurveKeysClipboard()).toHaveLength(1);
    expect(getCurveKeysClipboard()[0]).toMatchObject({ time: 50, value: 50 });
  });

  it("Ctrl+C with no selection does not write the clipboard", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    await focusScale(bridge);
    fireEvent.keyDown(document.body, { key: "c", ctrlKey: true });
    expect(getCurveKeysClipboard()).toHaveLength(0);
  });

  it("Ctrl+V with an empty clipboard fires no add-track-key", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    await focusScale(bridge);
    fireEvent.keyDown(document.body, { key: "v", ctrlKey: true });
    expect(addTrackKeyCalls(bridge)).toHaveLength(0);
  });

  it("Ctrl+C fired inside a text input does not write the clipboard (TYPING_TAGS guard)", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    await focusScale(bridge);
    clickKeyByTime(50);
    const valueInput = screen
      .getByTestId("ce-spinner-value-wrapper")
      .querySelector("input") as HTMLInputElement;
    valueInput.focus();
    fireEvent.keyDown(valueInput, { key: "c", ctrlKey: true });
    expect(getCurveKeysClipboard()).toHaveLength(0);
  });

  it("Ctrl+C originating inside the emitter tree does not write the curve clipboard", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    await focusScale(bridge);
    clickKeyByTime(50);
    const tree = document.createElement("div");
    tree.setAttribute("data-testid", "emitter-tree");
    const child = document.createElement("div");
    tree.appendChild(child);
    document.body.appendChild(tree);
    try {
      fireEvent.keyDown(child, { key: "c", ctrlKey: true });
      expect(getCurveKeysClipboard()).toHaveLength(0);
    } finally {
      document.body.removeChild(tree);
    }
  });

  it("cross-track paste: copy on Scale, switch focus to Red, paste lands on Red", async () => {
    const { bridge } = makeStubBridgeWithFocusInteriorKey(0);
    await focusScale(bridge);
    clickKeyByTime(50);
    fireEvent.keyDown(document.body, { key: "c", ctrlKey: true });
    await waitFor(() => expect(getCurveKeysClipboard()).toHaveLength(1));
    selectChannel("red");
    await waitFor(() => expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument());
    fireEvent.keyDown(document.body, { key: "v", ctrlKey: true });
    await waitFor(() => {
      const adds = addTrackKeyCalls(bridge);
      expect(adds).toHaveLength(1);
      expect(adds[0]!.params.track).toBe("red");
    });
  });
});

// Group-drag live-updates the Time/Value spinners.
//
// When ≥2 keys are selected and the user drags the group, the Value
// spinner must reflect the live shifted average — not the stale committed
// average. This exercises the full panel path: onGroupDragMove → liveGroup
// state → multiSelected recompute → spinnerValueValue.
describe("CurveEditorPanel — group-drag live-updates spinners", () => {
  /** Bridge whose `red` track has TWO interior keys (25/75) plus borders (0/100),
   *  making it easy to select both interior keys for a group drag on the default
   *  focus channel without needing a channel switch. */
  function makeStubBridgeRedInterior() {
    const tracks: TrackDto[] = TRACK_NAMES.map((name) => ({
      name,
      keys: name === "red"
        ? [
            { time: 0,   value: 0 },
            { time: 25,  value: 0.25 },
            { time: 75,  value: 0.75 },
            { time: 100, value: 1 },
          ]
        : [
            { time: 0,   value: 0 },
            { time: 100, value: name === "rotationSpeed" ? -1 : 1 },
          ],
      interpolation: "linear" as const,
      lockedTo: null,
    }));
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) => {
        if (req.kind === "engine/state/snapshot") {
          return Promise.resolve({ ...makeDefaultEngineState(), selectedEmitterId: 1 });
        }
        if (req.kind === "emitters/get-tracks") {
          return Promise.resolve({ tracks });
        }
        return Promise.resolve({});
      }),
      on: vi.fn().mockImplementation(() => () => {}),
    } as unknown as Bridge & {
      request: ReturnType<typeof vi.fn>;
      on: ReturnType<typeof vi.fn>;
    };
    return { bridge };
  }

  it("Value spinner live-updates during a group drag (multiSelected uses shifted positions)", async () => {
    const { bridge } = makeStubBridgeRedInterior();
    render(<CurveEditorPanel bridge={bridge} />);

    // Wait for the red track to render (default focus channel).
    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });

    // Select both interior red keys (t=25 and t=75) with ctrl+click.
    const keyAt25 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red");
    const keyAt75 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "75" && k.getAttribute("data-channel-id") === "red");
    expect(keyAt25).toBeDefined();
    expect(keyAt75).toBeDefined();
    fireEvent.click(keyAt25!);
    fireEvent.click(keyAt75!, { ctrlKey: true });

    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("2"));

    // Verify the committed average value is (0.25 + 0.75) / 2 = 0.5 before the drag.
    // NOTE: the Spinner is keyed by its value, so it REMOUNTS on every live
    // change — re-query `getByLabelText` for each read rather than caching it.
    const readValue = () =>
      Number((screen.getByLabelText("Selected key value") as HTMLInputElement).value);
    await waitFor(() =>
      expect((screen.getByLabelText("Selected key value") as HTMLInputElement).disabled).toBe(false),
    );
    expect(readValue()).toBeCloseTo(0.5, 2);

    // Set up the SVG for pointer event coordinates.
    // Red channel default focus: the SVG is 600×300 (jsdom falls back to props).
    const svg = document.querySelector("[data-testid='curve-editor-svg']") as SVGSVGElement;
    expect(svg).not.toBeNull();
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);

    // Begin group drag on the t=25 key.
    // t=25 → x = 25/100 * 600 = 150; v=0.25 → y = 300 - (0.25 * 300) = 225.
    fireEvent.pointerDown(keyAt25!, { button: 0, pointerId: 77, clientX: 150, clientY: 225 });

    // Move past DRAG_SLOP (1.5 px): shift value UP by 30 px (−30 in SVG y → +30/300 ≈ +0.1 value).
    // clientY 225 → 195: ΔclientY = −30 → Δvalue ≈ +0.1 on a 300px / 1.0 range canvas.
    fireEvent.pointerMove(svg, { pointerId: 77, clientX: 150, clientY: 195 });

    // During the drag, the live average value should reflect the shifted positions.
    // Pre-drag avg = 0.5; with dValue ≈ +0.1, live avg ≈ 0.5 + 0.1 = 0.6.
    // We just verify the spinner has moved away from the committed 0.5.
    await waitFor(() => {
      const liveValue = readValue();
      expect(liveValue).not.toBeCloseTo(0.5, 1); // spinner must have updated
      expect(liveValue).toBeGreaterThan(0.5);     // drag went up → value increased
    });
  });

  it("Value spinner returns to committed average after group drag cancel", async () => {
    const { bridge } = makeStubBridgeRedInterior();
    render(<CurveEditorPanel bridge={bridge} />);

    await waitFor(() => {
      expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument();
    });

    const keyAt25 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red")!;
    const keyAt75 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "75" && k.getAttribute("data-channel-id") === "red")!;
    fireEvent.click(keyAt25);
    fireEvent.click(keyAt75, { ctrlKey: true });

    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("2"));

    const svg = document.querySelector("[data-testid='curve-editor-svg']") as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);

    // The Spinner remounts on each live change — re-query per read.
    const readValue = () =>
      Number((screen.getByLabelText("Selected key value") as HTMLInputElement).value);
    await waitFor(() =>
      expect((screen.getByLabelText("Selected key value") as HTMLInputElement).disabled).toBe(false),
    );

    // Drag past slop, then cancel.
    fireEvent.pointerDown(keyAt25, { button: 0, pointerId: 78, clientX: 150, clientY: 225 });
    fireEvent.pointerMove(svg, { pointerId: 78, clientX: 150, clientY: 195 });

    // Verify spinner moved.
    await waitFor(() => expect(readValue()).not.toBeCloseTo(0.5, 1));

    // Cancel the drag — spinner must revert to the committed average.
    fireEvent.pointerCancel(svg, { pointerId: 78 });

    await waitFor(() => {
      expect(readValue()).toBeCloseTo(0.5, 2);
    });
  });

  it("Time spinner live-updates during a horizontal group drag", async () => {
    const { bridge } = makeStubBridgeRedInterior();
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument());

    const keyAt25 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red")!;
    const keyAt75 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "75" && k.getAttribute("data-channel-id") === "red")!;
    fireEvent.click(keyAt25);
    fireEvent.click(keyAt75, { ctrlKey: true });
    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("2"));

    const svg = document.querySelector("[data-testid='curve-editor-svg']") as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);

    // Both interior keys selected → avgTime = (25+75)/2 = 50 before the drag.
    const readTime = () =>
      Number((screen.getByLabelText("Selected key time") as HTMLInputElement).value);
    await waitFor(() =>
      expect((screen.getByLabelText("Selected key time") as HTMLInputElement).disabled).toBe(false),
    );
    expect(readTime()).toBeCloseTo(50, 1);

    // Grab t=25 (x=150) and drag RIGHT by 60px → +60/600*100 = +10 time units.
    fireEvent.pointerDown(keyAt25, { button: 0, pointerId: 81, clientX: 150, clientY: 225 });
    fireEvent.pointerMove(svg, { pointerId: 81, clientX: 210, clientY: 225 });

    await waitFor(() => {
      const t = readTime();
      expect(t).not.toBeCloseTo(50, 1); // Time spinner must live-update too
      expect(t).toBeGreaterThan(50);    // dragged right → average time increased
    });
  });

  it("all-border group drag live-updates value only; time spinner stays disabled", async () => {
    const { bridge } = makeStubBridgeRedInterior();
    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument());

    // Select BOTH border keys (t=0, t=100) — no interior in the selection.
    const keyAt0 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "0" && k.getAttribute("data-channel-id") === "red")!;
    const keyAt100 = screen.getAllByTestId("curve-key")
      .find((k) => k.getAttribute("data-key-time") === "100" && k.getAttribute("data-channel-id") === "red")!;
    fireEvent.click(keyAt0);
    fireEvent.click(keyAt100, { ctrlKey: true });
    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("2"));

    const svg = document.querySelector("[data-testid='curve-editor-svg']") as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);

    // Time spinner disabled (all-border selection); value spinner enabled at avg (0+1)/2 = 0.5.
    const timeInput = () => screen.getByLabelText("Selected key time") as HTMLInputElement;
    const readValue = () =>
      Number((screen.getByLabelText("Selected key value") as HTMLInputElement).value);
    await waitFor(() => expect(timeInput().disabled).toBe(true));
    expect(readValue()).toBeCloseTo(0.5, 2);

    // Grab t=0 (x=0, v=0 → y=300) and drag UP 30px → value +0.1.
    fireEvent.pointerDown(keyAt0, { button: 0, pointerId: 82, clientX: 0, clientY: 300 });
    fireEvent.pointerMove(svg, { pointerId: 82, clientX: 0, clientY: 270 });

    await waitFor(() => {
      expect(readValue()).toBeGreaterThan(0.5); // borders shift in value
    });
    expect(timeInput().disabled).toBe(true);     // time still pinned for all-border
  });

  // #610 regression: the live-spinner state write is COALESCED to an animation
  // frame, not committed synchronously on every pointer-move. This is the fix
  // for the curve trailing the cursor during a drag — unthrottled per-move
  // re-renders of this large panel starved the renderer's rAF curve reshape.
  // Pins deferral: with rAF held, a move must NOT update the spinner; the
  // update lands only once a frame flushes. A revert to synchronous setState
  // would fail the pre-flush assertion.
  it("coalesces the live-spinner write to an animation frame (does not update synchronously per move) (#610)", async () => {
    const rafCbs: FrameRequestCallback[] = [];
    const realRaf = window.requestAnimationFrame;
    const realCaf = window.cancelAnimationFrame;
    // Capture rAF callbacks instead of auto-firing so we control the flush.
    window.requestAnimationFrame = ((cb: FrameRequestCallback) => {
      rafCbs.push(cb);
      return rafCbs.length; // 1-based handle
    }) as typeof window.requestAnimationFrame;
    window.cancelAnimationFrame = ((id: number) => {
      if (id >= 1 && id <= rafCbs.length) rafCbs[id - 1] = (() => {}) as FrameRequestCallback;
    }) as typeof window.cancelAnimationFrame;
    const flushFrames = () =>
      act(() => {
        for (const cb of rafCbs.splice(0)) cb(0);
      });

    try {
      const { bridge } = makeStubBridgeRedInterior();
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument());

      const keyAt25 = screen.getAllByTestId("curve-key")
        .find((k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red")!;
      const keyAt75 = screen.getAllByTestId("curve-key")
        .find((k) => k.getAttribute("data-key-time") === "75" && k.getAttribute("data-channel-id") === "red")!;
      fireEvent.click(keyAt25);
      fireEvent.click(keyAt75, { ctrlKey: true });
      const panel = screen.getByTestId("curve-editor-panel");
      await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("2"));

      const svg = document.querySelector("[data-testid='curve-editor-svg']") as SVGSVGElement;
      svg.getBoundingClientRect = () =>
        ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);
      const readValue = () =>
        Number((screen.getByLabelText("Selected key value") as HTMLInputElement).value);
      await waitFor(() =>
        expect((screen.getByLabelText("Selected key value") as HTMLInputElement).disabled).toBe(false),
      );
      // Drain any frames queued by mounting/selection so the drag starts clean.
      flushFrames();
      expect(readValue()).toBeCloseTo(0.5, 2);

      // Begin the group drag, then move THREE times within the held frame. Each
      // callback fires synchronously but the state write is deferred, and the
      // moves must COALESCE — not schedule a fresh flush apiece.
      fireEvent.pointerDown(keyAt25, { button: 0, pointerId: 91, clientX: 150, clientY: 225 });
      fireEvent.pointerMove(svg, { pointerId: 91, clientX: 150, clientY: 215 }); // +10px up
      fireEvent.pointerMove(svg, { pointerId: 91, clientX: 150, clientY: 205 }); // +20px up
      fireEvent.pointerMove(svg, { pointerId: 91, clientX: 150, clientY: 195 }); // +30px up → +0.1 value

      // Deferral: with the frame still held, the spinner must NOT have moved.
      expect(readValue()).toBeCloseTo(0.5, 2);
      // Coalescing: three moves scheduled FEWER than three frames (the panel's
      // live-write frame is reused, not re-queued per move; the renderer's own
      // drag-tick frame coalesces too). A per-move setState would queue ≥3.
      expect(rafCbs.length).toBeLessThan(3);

      // Flush once: only the LATEST move's value lands (0.5 + 0.1 = 0.6),
      // proving the intermediate writes collapsed into the last.
      flushFrames();
      expect(readValue()).toBeCloseTo(0.6, 1);
    } finally {
      window.requestAnimationFrame = realRaf;
      window.cancelAnimationFrame = realCaf;
    }
  });

  // #613 regression: a SINGLE-key Value spinner edit must update the curve
  // IMMEDIATELY (optimistic track write, no bridge round-trip) and SNAP (no
  // morph glide). matchMedia is stubbed so morphs are enabled — a revert of the
  // suppress would mount the morph overlay, and a revert of the optimistic
  // setTracks would leave the key un-moved (no refetch fires in this stub).
  it("single-key Value spinner edit moves the curve at once and does not morph (#613)", async () => {
    // matchMedia is read-only in jsdom; define it (morphs enabled: reduce=false).
    const realMatchMedia = (window as Window & typeof globalThis).matchMedia;
    Object.defineProperty(window, "matchMedia", {
      configurable: true,
      value: (q: string) => ({
        matches: false, media: q, onchange: null,
        addListener() {}, removeListener() {},
        addEventListener() {}, removeEventListener() {},
        dispatchEvent() { return false; },
      }),
    });
    try {
      const { bridge } = makeStubBridgeRedInterior();
      render(<CurveEditorPanel bridge={bridge} />);
      await waitFor(() => expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument());

      const keyAt25 = () =>
        screen.getAllByTestId("curve-key").find(
          (k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red",
        )!;
      fireEvent.click(keyAt25());
      const panel = screen.getByTestId("curve-editor-panel");
      await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("1"));

      const cyBefore = Number(keyAt25().getAttribute("cy"));

      // Edit the Value spinner up (0.25 -> 0.9) and commit.
      const valueInput = screen
        .getByTestId("ce-spinner-value-wrapper")
        .querySelector("input") as HTMLInputElement;
      await waitFor(() => expect(valueInput.disabled).toBe(false));
      fireEvent.focus(valueInput);
      fireEvent.change(valueInput, { target: { value: "0.9" } });
      fireEvent.blur(valueInput);

      // Optimistic: the key jumps up immediately (higher value -> smaller cy),
      // with NO refetch in this stub. A revert of the optimistic setTracks
      // would leave cy unchanged.
      await waitFor(() => {
        expect(Number(keyAt25().getAttribute("cy"))).toBeLessThan(cyBefore - 1);
      });

      // Suppressed: the change never mounts a morph overlay. A revert of the
      // suppress would glide (overlay present) since morphs are enabled here.
      await new Promise((r) => setTimeout(r, 40));
      expect(document.querySelector('[data-testid="curve-morph-overlay"]')).toBeNull();
    } finally {
      Object.defineProperty(window, "matchMedia", { configurable: true, value: realMatchMedia });
    }
  });

  // #613 regression (scrub race): every set-track-key echoes tree/changed →
  // get-tracks. During a rapid spinner scrub those refetch responses land LATE,
  // carrying pre-edit snapshots; without the edit-epoch guard they overwrite the
  // optimistic tracks and yank the curve backwards ("curve lags the key"). This
  // pins the guard: a get-tracks response ISSUED BEFORE a newer local edit must
  // be discarded, leaving the optimistic (newest) curve untouched.
  it("a stale get-tracks response from before a newer local edit does not yank the curve back (#613)", async () => {
    const seedTracks: TrackDto[] = TRACK_NAMES.map((name) => ({
      name,
      keys: name === "red"
        ? [
            { time: 0,   value: 0 },
            { time: 25,  value: 0.25 },
            { time: 100, value: 1 },
          ]
        : [
            { time: 0,   value: 0 },
            { time: 100, value: name === "rotationSpeed" ? -1 : 1 },
          ],
      interpolation: "linear" as const,
      lockedTo: null,
    }));
    // The stale snapshot: red@25 = 0.5 (the FIRST edit), not the newest (0.9).
    const staleTracks = seedTracks.map((t) =>
      t.name !== "red"
        ? t
        : { ...t, keys: t.keys.map((k) => (k.time === 25 ? { time: 25, value: 0.5 } : k)) },
    );

    // Bridge stub with manual control: the initial get-tracks resolves with the
    // seed; every LATER get-tracks queues as a deferred the test resolves; the
    // host's tree/changed emission is fired by hand.
    const pendingGetTracks: Array<(v: { tracks: TrackDto[] }) => void> = [];
    const treeChanged: Array<() => void> = [];
    let firstFetch = true;
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string }) => {
        if (req.kind === "engine/state/snapshot") {
          return Promise.resolve({ ...makeDefaultEngineState(), selectedEmitterId: 1 });
        }
        if (req.kind === "emitters/get-tracks") {
          if (firstFetch) {
            firstFetch = false;
            return Promise.resolve({ tracks: seedTracks });
          }
          return new Promise((res) => { pendingGetTracks.push(res as (v: { tracks: TrackDto[] }) => void); });
        }
        return Promise.resolve({});
      }),
      on: vi.fn().mockImplementation((kind: string, cb: () => void) => {
        if (kind === "emitters/tree/changed") treeChanged.push(cb);
        return () => {};
      }),
    } as unknown as Bridge;

    render(<CurveEditorPanel bridge={bridge} />);
    await waitFor(() => expect(screen.getByTestId("curve-layer-red")).toBeInTheDocument());

    const keyAt25 = () =>
      screen.getAllByTestId("curve-key").find(
        (k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red",
      )!;
    fireEvent.click(keyAt25());
    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("1"));

    const valueInput = () =>
      screen.getByTestId("ce-spinner-value-wrapper").querySelector("input") as HTMLInputElement;
    await waitFor(() => expect(valueInput().disabled).toBe(false));

    // Edit #1 (scrub tick): 0.25 → 0.5. The host echo then triggers a refetch
    // — issued NOW, so it captures the pre-edit-#2 epoch.
    fireEvent.focus(valueInput());
    fireEvent.change(valueInput(), { target: { value: "0.5" } });
    fireEvent.blur(valueInput());
    act(() => { for (const cb of treeChanged) cb(); });
    await waitFor(() => expect(pendingGetTracks.length).toBe(1));

    // Edit #2 (next scrub tick): 0.5 → 0.9. Optimistic curve moves to 0.9.
    fireEvent.focus(valueInput());
    fireEvent.change(valueInput(), { target: { value: "0.9" } });
    fireEvent.blur(valueInput());
    let cyAt09 = 0;
    await waitFor(() => {
      cyAt09 = Number(keyAt25().getAttribute("cy"));
      expect(cyAt09).toBeGreaterThan(0); // rendered
      expect(Number(valueInput().value)).toBeCloseTo(0.9, 3);
    });

    // NOW the stale refetch (issued after edit #1, carrying value 0.5) lands.
    // The epoch guard must DISCARD it: the curve stays at the optimistic 0.9.
    await act(async () => {
      pendingGetTracks[0]!({ tracks: staleTracks });
      await Promise.resolve();
    });
    await new Promise((r) => setTimeout(r, 20));
    expect(Number(keyAt25().getAttribute("cy"))).toBeCloseTo(cyAt09, 1);
  });

  // #613 Codex-review regression: the epoch guard must NOT invalidate the
  // authoritative SELECTION-change fetch. Editing the still-visible OLD emitter
  // mid-switch bumps the epoch; if the new emitter's fetch were epoch-guarded it
  // would be discarded and — with no tree/changed to re-fetch — the panel would
  // be stranded on the old emitter's tracks. The selection fetch is unguarded.
  it("a selection-change fetch still applies even if a local edit bumped the epoch after it was issued (#613)", async () => {
    const tracksA: TrackDto[] = TRACK_NAMES.map((name) => ({
      name,
      keys: name === "red"
        ? [{ time: 0, value: 0 }, { time: 25, value: 0.25 }, { time: 100, value: 1 }]
        : [{ time: 0, value: 0 }, { time: 100, value: name === "rotationSpeed" ? -1 : 1 }],
      interpolation: "linear" as const,
      lockedTo: null,
    }));
    // Emitter B's red track has a DISTINCTIVE key at t=50 that A lacks.
    const tracksB: TrackDto[] = TRACK_NAMES.map((name) => ({
      name,
      keys: name === "red"
        ? [{ time: 0, value: 0 }, { time: 50, value: 0.5 }, { time: 100, value: 1 }]
        : [{ time: 0, value: 0 }, { time: 100, value: name === "rotationSpeed" ? -1 : 1 }],
      interpolation: "linear" as const,
      lockedTo: null,
    }));

    const selectionListeners: Array<(e: { payload: { id: number } }) => void> = [];
    const pendingBFetch: Array<(v: { tracks: TrackDto[] }) => void> = [];
    const bridge = {
      request: vi.fn().mockImplementation((req: { kind: string; params?: { id?: number } }) => {
        if (req.kind === "engine/state/snapshot") {
          return Promise.resolve({ ...makeDefaultEngineState(), selectedEmitterId: 1 });
        }
        if (req.kind === "emitters/get-tracks") {
          if (req.params?.id === 1) return Promise.resolve({ tracks: tracksA });
          // Emitter B's fetch is deferred so the test controls when it lands.
          return new Promise((res) => { pendingBFetch.push(res as (v: { tracks: TrackDto[] }) => void); });
        }
        return Promise.resolve({});
      }),
      on: vi.fn().mockImplementation((kind: string, cb: (e: { payload: { id: number } }) => void) => {
        if (kind === "emitters/selected") selectionListeners.push(cb);
        return () => {};
      }),
    } as unknown as Bridge;

    render(<CurveEditorPanel bridge={bridge} />);
    // Emitter A loaded (its t=25 key is present, B's t=50 is not).
    await waitFor(() =>
      expect(
        screen.getAllByTestId("curve-key").some(
          (k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red",
        ),
      ).toBe(true),
    );

    // Select A's interior key so a spinner edit is possible.
    fireEvent.click(
      screen.getAllByTestId("curve-key").find(
        (k) => k.getAttribute("data-key-time") === "25" && k.getAttribute("data-channel-id") === "red",
      )!,
    );
    const panel = screen.getByTestId("curve-editor-panel");
    await waitFor(() => expect(panel.getAttribute("data-selected-key-count")).toBe("1"));

    // Switch selection to emitter B → B's get-tracks is issued (and deferred).
    act(() => { for (const cb of selectionListeners) cb({ payload: { id: 2 } }); });
    await waitFor(() => expect(pendingBFetch.length).toBe(1));

    // Edit the still-visible A key via the Value spinner → bumps the edit epoch
    // (bridge id is now 2; the host would no-op with no tree/changed).
    const valueInput = screen
      .getByTestId("ce-spinner-value-wrapper")
      .querySelector("input") as HTMLInputElement;
    await waitFor(() => expect(valueInput.disabled).toBe(false));
    fireEvent.focus(valueInput);
    fireEvent.change(valueInput, { target: { value: "0.7" } });
    fireEvent.blur(valueInput);

    // NOW emitter B's authoritative fetch lands. Despite the bumped epoch it
    // must apply (unguarded), so B's tracks show — its distinctive t=50 key
    // appears and A's t=25 is gone.
    await act(async () => {
      pendingBFetch[0]!({ tracks: tracksB });
      await Promise.resolve();
    });
    await waitFor(() => {
      const keys = screen.getAllByTestId("curve-key")
        .filter((k) => k.getAttribute("data-channel-id") === "red")
        .map((k) => k.getAttribute("data-key-time"));
      expect(keys).toContain("50");   // B applied
      expect(keys).not.toContain("25"); // not stranded on A
    });
  });
});

// ─── Snap-to-grid toggle persistence (#618) ──────────────────────────────────
describe("CurveEditorPanel — snap-to-grid toggle (#618)", () => {
  beforeEach(() => {
    localStorage.clear();
    __resetAtlasContext();
    __resetRightDockForTests();
  });

  it("defaults OFF and renders in the toolbar", () => {
    const { bridge } = makeStubBridge(null);
    render(<CurveEditorPanel bridge={bridge} />);
    const toggle = screen.getByTestId("curve-snap-toggle");
    expect(toggle).toBeInTheDocument();
    expect(toggle.getAttribute("aria-pressed")).toBe("false");
    // Untouched → no stored value.
    expect(localStorage.getItem("curveEditor.snapToGrid")).toBeNull();
  });

  it("flips on/off and persists each state to localStorage", () => {
    const { bridge } = makeStubBridge(null);
    render(<CurveEditorPanel bridge={bridge} />);
    const toggle = screen.getByTestId("curve-snap-toggle");

    fireEvent.click(toggle);
    expect(toggle.getAttribute("aria-pressed")).toBe("true");
    expect(localStorage.getItem("curveEditor.snapToGrid")).toBe("1");

    fireEvent.click(toggle);
    expect(toggle.getAttribute("aria-pressed")).toBe("false");
    expect(localStorage.getItem("curveEditor.snapToGrid")).toBe("0");
  });

  it("reads the persisted preference back on mount", () => {
    localStorage.setItem("curveEditor.snapToGrid", "1");
    const { bridge } = makeStubBridge(null);
    render(<CurveEditorPanel bridge={bridge} />);
    expect(screen.getByTestId("curve-snap-toggle").getAttribute("aria-pressed")).toBe("true");
  });

  it("threads snapEnabled to the canvas — a key drag with snap ON commits a SNAPPED value to the bridge", async () => {
    // Give red an interior key at t=50 so we can drag it freely.
    const tracks = fixtureTracks().map((t) =>
      t.name === "red"
        ? { ...t, keys: [{ time: 0, value: 0.5 }, { time: 50, value: 0.5 }, { time: 100, value: 0.5 }] }
        : t,
    );
    const { bridge } = makeStubBridge(0, tracks);
    render(<CurveEditorPanel bridge={bridge} />);
    const svg = (await screen.findByTestId("curve-editor-svg")) as unknown as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);

    // Turn snap ON, then drag the interior key off-grid: t=50 (x=300, v=0.5 →
    // y=150) → x=307 (time 51.17 → snaps 52), y=143 (value 0.5233 → snaps 0.52).
    fireEvent.click(screen.getByTestId("curve-snap-toggle"));
    const key = document.querySelector(
      '[data-testid="curve-key"][data-key-time="50"][data-channel-id="red"]',
    )!;
    fireEvent.pointerDown(key, { button: 0, pointerId: 80, clientX: 300, clientY: 150 });
    fireEvent.pointerMove(svg, { pointerId: 80, clientX: 307, clientY: 143 });
    fireEvent.pointerUp(svg, { pointerId: 80, clientX: 307, clientY: 143 });

    const call = bridge.request.mock.calls.find(
      (c: unknown[]) => (c[0] as { kind: string }).kind === "emitters/set-track-key",
    );
    expect(call).toBeDefined();
    const params = (call![0] as { params: { newTime: number; newValue: number } }).params;
    expect(params.newTime).toBeCloseTo(52, 4);   // snapped, not raw 51.17
    expect(params.newValue).toBeCloseTo(0.52, 4); // snapped, not raw 0.5233
  });
});
