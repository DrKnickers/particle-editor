// Vitest tests for CurveEditorPanel (Task 2.6 of the redesign).
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
import { fireEvent, render as rtlRender, screen, waitFor } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import type {
  Bridge,
  TrackDto,
} from "@particle-editor/bridge-schema";
import { TRACK_NAMES } from "@particle-editor/bridge-schema";
import { CurveEditorPanel, CHANNELS } from "../CurveEditorPanel";

//: the toolbar buttons mount Tips (Radix Tooltip.Root), which
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

type SelectionListener = (e: { payload: { id: number | null } }) => void;

function makeStubBridge(initialSelectedId: number | null) {
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
        return Promise.resolve({ tracks: fixtureTracks() });
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

  // F9: Index is exclusive too — enabling it hides every other channel.
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

  // F9: the two exclusive channels replace each other — clicking Index
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

  // F9: selecting a non-exclusive curve exits Index solo.
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
 *  the two borders (0/100), so F8 multi-key average specs have a real
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

// F8: multi-key average edit (shift-by-delta / preserve spread).
describe("CurveEditorPanel — F8 multi-key average edit", () => {
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
    // Time spinner (step 0.1, an-audit-finding) renders at the app-wide 2dp default.
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

// an-audit-finding: the curve Time spinner uses a 0.1 step (legacy granularity) and
// displays at the app-wide 2dp default (decoupled from step, per).
describe("CurveEditorPanel — an-audit-finding decimal-grained time", () => {
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

// an-audit-finding: right-click on the empty curve canvas. In Select mode it clears
// the selection (legacy WM_RBUTTONDOWN → CM_SELECT); in Insert mode it
// drops back to Select mode without deselecting (legacy CM_INSERT branch).
describe("CurveEditorPanel — an-audit-finding right-click deselect", () => {
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

// an-audit-finding: Copy / Cut / Paste of selected curve keys via Ctrl+C / X / V,
// matching legacy CurveEditor.cpp CopyKeys / PasteKeys. Window-scoped (SVG
// clicks don't move DOM focus into the panel), with a TYPING_TAGS guard and
// an emitter-tree-origin guard so the two clipboards never both fire.
describe("CurveEditorPanel — an-audit-finding key copy/cut/paste", () => {
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
