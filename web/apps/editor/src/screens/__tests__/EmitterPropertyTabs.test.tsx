// Vitest tests for EmitterPropertyTabs (Phase 4.1 Fix dispatch 1).
//
// Covered:
//   - Renders the placeholder when no emitter is selected.
//   - Renders all three tabs (Basic / Appearance / Physics) when
//     selected; Basic is the default-open tab (verified via the
//     `data-state="active"` attribute Radix sets).
//   - Basic tab renders form fields populated from the get-properties
//     response (Lifetime spinner, Name input, useBursts checkbox).
//   - Appearance + Physics tabs render the "Coming in Fix dispatch N"
//     placeholders.
//   - Editing the Lifetime spinner fires emitters/set-properties
//     with `patch: { lifetime: <new value> }`.

import { describe, it, expect, vi } from "vitest";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import type {
  Bridge,
  EmitterPropertiesDto,
} from "@particle-editor/bridge-schema";
import { EmitterPropertyTabs, AppearanceTab, PhysicsTab } from "../EmitterPropertyTabs";
import { makeDefaultEngineState, makeFixtureProperties } from "@/bridge/mock-state";

type SelectionListener = (e: { payload: { id: number | null } }) => void;

function makeStubBridge(
  initialSelectedId: number | null,
  propsOverride?: Partial<EmitterPropertiesDto>,
) {
  const listeners: SelectionListener[] = [];
  let properties: EmitterPropertiesDto = {
    ...makeFixtureProperties(initialSelectedId ?? 0),
    ...propsOverride,
  };
  const bridge = {
    request: vi.fn().mockImplementation((req: { kind: string; params?: unknown }) => {
      if (req.kind === "engine/state/snapshot") {
        return Promise.resolve({
          ...makeDefaultEngineState(),
          selectedEmitterId: initialSelectedId,
        });
      }
      if (req.kind === "emitters/get-properties") {
        return Promise.resolve({ properties });
      }
      if (req.kind === "emitters/set-properties") {
        const p = (req.params as { patch: Partial<EmitterPropertiesDto> }).patch;
        properties = { ...properties, ...p };
        return Promise.resolve({});
      }
      return Promise.resolve({});
    }),
    on: vi.fn().mockImplementation((kind: string, h: SelectionListener) => {
      if (kind === "emitters/selected") {
        listeners.push(h);
      }
      return () => {
        const idx = listeners.indexOf(h);
        if (idx >= 0) listeners.splice(idx, 1);
      };
    }),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
  return {
    bridge,
    pushSelection: (id: number | null) => listeners.forEach((l) => l({ payload: { id } })),
  };
}

describe("EmitterPropertyTabs", () => {
  it("renders the always-mounted tab strip with body-level placeholder when no emitter is selected", async () => {
    // B1.3.1: the tab strip is mounted unconditionally so the user can
    // see the Basic/Appearance/Physics structure (and pre-click a tab)
    // before any emitter is selected. The placeholder sits inside the
    // active tab's body, not in place of the whole component.
    const { bridge } = makeStubBridge(null);
    render(<EmitterPropertyTabs bridge={bridge} />);
    await waitFor(() => {
      expect(
        screen.getByTestId("emitter-property-tabs-placeholder"),
      ).toBeInTheDocument();
    });
    expect(
      screen.getByText(/Select an emitter to edit its properties/),
    ).toBeInTheDocument();
    // Strip is mounted (was previously absent on the no-selection branch).
    expect(screen.getByTestId("emitter-property-tabs")).toBeInTheDocument();
    // All three triggers are present so the user can pre-pick a tab.
    expect(screen.getByTestId("tab-trigger-basic")).toBeInTheDocument();
    expect(screen.getByTestId("tab-trigger-appearance")).toBeInTheDocument();
    expect(screen.getByTestId("tab-trigger-physics")).toBeInTheDocument();
  });

  it("renders three tab triggers with Basic active by default when an emitter is selected", async () => {
    const { bridge } = makeStubBridge(0);
    render(<EmitterPropertyTabs bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("emitter-property-tabs")).toBeInTheDocument();
    });
    const basic = screen.getByTestId("tab-trigger-basic");
    const appearance = screen.getByTestId("tab-trigger-appearance");
    const physics = screen.getByTestId("tab-trigger-physics");
    expect(basic).toBeInTheDocument();
    expect(appearance).toBeInTheDocument();
    expect(physics).toBeInTheDocument();
    // Radix sets data-state="active" on the open trigger + content.
    expect(basic.getAttribute("data-state")).toBe("active");
    expect(appearance.getAttribute("data-state")).toBe("inactive");
    expect(physics.getAttribute("data-state")).toBe("inactive");
  });

  it("Basic tab renders Maximum lifetime + Name + Bursts radio populated from get-properties", async () => {
    // Post-B1.3-P3: "Lifetime" is now "Maximum lifetime:" inside the
    // Generation section, and "Use Bursts" is the Bursts radio of a
    // tri-state mutex (Bursts / Continuous / Weather).
    const { bridge } = makeStubBridge(0, {
      lifetime: 2.5,
      name: "TestEmitter",
      useBursts: false,
      isWeatherParticle: false,
    });
    render(<EmitterPropertyTabs bridge={bridge} />);
    // B1.3.1: the tab strip mounts immediately; wait specifically for the
    // BasicTab form to hydrate (placeholder → loading → form transition).
    await waitFor(() => {
      expect(screen.getByLabelText("Maximum lifetime:")).toBeInTheDocument();
    });
    // Name input populated from properties.
    const nameInput = screen.getByLabelText("Name") as HTMLInputElement;
    expect(nameInput.value).toBe("TestEmitter");
    // Maximum lifetime spinner populated.
    const lifetimeInput = screen.getByLabelText("Maximum lifetime:") as HTMLInputElement;
    expect(Number(lifetimeInput.value)).toBeCloseTo(2.5, 5);
    // Bursts radio is present and NOT active (useBursts=false → continuous).
    const burstsRadio = screen.getByRole("radio", { name: /Bursts/i });
    expect(burstsRadio.getAttribute("aria-checked")).toBe("false");
    const continuousRadio = screen.getByRole("radio", { name: /Continuous/i });
    expect(continuousRadio.getAttribute("aria-checked")).toBe("true");
  });

  it("Appearance and Physics tab triggers render with their data-state attribute", async () => {
    const { bridge } = makeStubBridge(0);
    render(<EmitterPropertyTabs bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("emitter-property-tabs")).toBeInTheDocument();
    });
    // The three triggers exist and announce their inactive state.
    // Radix Tabs in jsdom doesn't reliably switch on fireEvent.click
    // (the known pointer-event flake from the lessons), so we assert
    // structurally: the triggers are present and the Basic content is
    // active (single source of truth via data-state).
    expect(screen.getByTestId("tab-trigger-appearance")).toBeInTheDocument();
    expect(screen.getByTestId("tab-trigger-physics")).toBeInTheDocument();
    expect(screen.getByTestId("tab-trigger-appearance").getAttribute("data-state")).toBe("inactive");
    expect(screen.getByTestId("tab-trigger-physics").getAttribute("data-state")).toBe("inactive");
    // The matching content panels exist as DOM nodes (Radix mounts the
    // <Tabs.Content> wrapper even when inactive — the children inside
    // it are what's conditionally mounted), so their `data-testid` is
    // queryable.
    expect(screen.getByTestId("tab-appearance-content")).toBeInTheDocument();
    expect(screen.getByTestId("tab-physics-content")).toBeInTheDocument();
  });

  // ─── Appearance tab specs (Fix dispatch 2) ─────────────────────
  // AppearanceTab is exported and mounted directly — Radix Tabs in
  // jsdom doesn't reliably switch tabs via fireEvent (the known
  // pointer-event flake from Fix dispatch 1), so we test the panel
  // content in isolation.

  it("AppearanceTab renders the expected post-P5 field labels", () => {
    const props = makeFixtureProperties(0);
    render(<AppearanceTab properties={props} onCommit={() => {}} />);
    // Post-B1.3-P5: five sections (Textures / Random color addition /
    // Tail / Rotation / Rendering) with renamed labels per spec §5.5.
    // `Triangles` and `Affected by Wind` are dropped (former
    // permanently from the inspector, latter relocated to Physics in
    // P6).
    const expectedLabels = [
      // Textures
      "Color:",
      "Bump:",
      "Texture elements:",
      "Minimum scale:",
      // Random color addition
      "RGBA:",
      "Grayscale",
      // Tail
      "Has tail",
      "Tail length:",
      // Rotation (moved in from Basic)
      "Random rotation direction",
      "Fixed random rotation:",
      "Rotation average:",
      "Rotation variance:",
      // Rendering
      "Always face camera",
      "Heat particle",
      "No depth test",
      "Blend mode:",
    ];
    for (const label of expectedLabels) {
      expect(screen.getByText(label)).toBeInTheDocument();
    }
  });

  it("AppearanceTab: editing Tail length fires onCommit with patch.tailSize", async () => {
    const onCommit = vi.fn();
    const props = { ...makeFixtureProperties(0), hasTail: true, tailSize: 0.5 };
    render(<AppearanceTab properties={props} onCommit={onCommit} />);
    const tailSizeInput = screen.getByLabelText("Tail length:") as HTMLInputElement;
    fireEvent.focus(tailSizeInput);
    fireEvent.change(tailSizeInput, { target: { value: "1.25" } });
    fireEvent.blur(tailSizeInput);
    await waitFor(() => {
      expect(onCommit).toHaveBeenCalledWith({ tailSize: 1.25 });
    });
  });

  it("AppearanceTab: hasTail === false disables Tail length spinner", () => {
    const props = { ...makeFixtureProperties(0), hasTail: false, tailSize: 2 };
    render(<AppearanceTab properties={props} onCommit={() => {}} />);
    const tailSizeInput = screen.getByLabelText("Tail length:") as HTMLInputElement;
    expect(tailSizeInput.disabled).toBe(true);
  });

  // `Triangles` was removed from Appearance in B1.3-P5 (dropped from
  // the inspector per Q2 decision; schema field retained on the wire).
  // Executable absence-assertion: fails if anyone re-adds Triangles to
  // the Appearance tab.
  it("AppearanceTab does not render the Triangles field (dropped per B1.3 Q2 decision)", () => {
    const props = makeFixtureProperties(0);
    render(<AppearanceTab properties={props} onCommit={() => {}} />);
    expect(screen.queryByLabelText("Triangles")).toBeNull();
    expect(screen.queryByLabelText("Triangles:")).toBeNull();
  });

  // ─── Physics tab specs (B1.3-P6 restructure) ───────────────────
  // PhysicsTab is exported and mounted directly for the same reason
  // AppearanceTab is: Radix Tabs in jsdom doesn't reliably switch on
  // fireEvent.click.
  //
  // Post-P6: four Sections (Initial position / Initial speed /
  // Acceleration / Ground interaction). `Emit From Mesh*` moved to
  // Basic > Connection (P4); Weather Particle / Cube Size / Cube
  // Distance moved to Basic > Generation Weather radio (P3);
  // Weather Fadeout Distance dropped (Q3); groups[1] not rendered
  // (Q4); Parent speed inherit (Basic→Physics) and Affected by wind
  // (Appearance→Physics) added under Initial speed.

  it("PhysicsTab renders the expected post-P6 field labels", () => {
    const props = makeFixtureProperties(0);
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    // Acceleration row is a 3-spinner cluster with a combined
    // "X / Y / Z:" label.
    expect(screen.getByText("X / Y / Z:")).toBeInTheDocument();
    expect(screen.getByLabelText("Acceleration X")).toBeInTheDocument();
    expect(screen.getByLabelText("Acceleration Y")).toBeInTheDocument();
    expect(screen.getByLabelText("Acceleration Z")).toBeInTheDocument();
    const expectedLabels = [
      // Initial speed
      "Inward speed:",
      "Parent speed inherit:",
      "Affected by wind",
      // Acceleration
      "Gravity acceleration:",
      "Inward acceleration:",
      "Object space acceleration",
      // Ground interaction
      "Behavior:",
      "Bounciness:",
    ];
    for (const label of expectedLabels) {
      expect(screen.getByText(label)).toBeInTheDocument();
    }
  });

  it("PhysicsTab renders four section headers in order: Initial position, Initial speed, Acceleration, Ground interaction", () => {
    const props = makeFixtureProperties(0);
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    const pos      = screen.getByTestId("section-initial-position");
    const speed    = screen.getByTestId("section-initial-speed");
    const accel    = screen.getByTestId("section-acceleration");
    const ground   = screen.getByTestId("section-ground-interaction");
    expect(pos).toBeInTheDocument();
    expect(speed).toBeInTheDocument();
    expect(accel).toBeInTheDocument();
    expect(ground).toBeInTheDocument();
    // DOM order matches legacy IDD_EMITTER_PROPS3.
    expect(pos.compareDocumentPosition(speed)   & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(speed.compareDocumentPosition(accel) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(accel.compareDocumentPosition(ground) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });

  it("PhysicsTab: removed fields are not rendered (Emit From Mesh*, Weather*, weather fadeout)", () => {
    const props = makeFixtureProperties(0);
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    // Moved to Basic > Connection in P4:
    expect(screen.queryByLabelText("Emit From Mesh")).toBeNull();
    expect(screen.queryByLabelText("Emit From Mesh Offset")).toBeNull();
    // Moved to Basic > Generation Weather radio in P3:
    expect(screen.queryByLabelText("Weather Particle")).toBeNull();
    expect(screen.queryByLabelText("Weather Cube Size")).toBeNull();
    expect(screen.queryByLabelText("Weather Cube Distance")).toBeNull();
    // Dropped per Q3 (schema field retained):
    expect(screen.queryByLabelText("Weather Fadeout Distance")).toBeNull();
    // Old PascalCase labels also gone:
    expect(screen.queryByLabelText("Gravity")).toBeNull();
    expect(screen.queryByLabelText("Inward Speed")).toBeNull();
    expect(screen.queryByLabelText("Bounciness")).toBeNull();
  });

  it("PhysicsTab: Acceleration renders 3 spinners side-by-side", () => {
    const props = {
      ...makeFixtureProperties(0),
      acceleration: [1, 2, 3] as unknown as [number, number, number],
    };
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    const x = screen.getByLabelText("Acceleration X") as HTMLInputElement;
    const y = screen.getByLabelText("Acceleration Y") as HTMLInputElement;
    const z = screen.getByLabelText("Acceleration Z") as HTMLInputElement;
    expect(Number(x.value)).toBeCloseTo(1, 5);
    expect(Number(y.value)).toBeCloseTo(2, 5);
    expect(Number(z.value)).toBeCloseTo(3, 5);
  });

  it("PhysicsTab: Ground Behavior dropdown lists Bounce and Stick options", () => {
    const props = makeFixtureProperties(0);
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    // The trigger renders the currently-selected label.
    const trigger = screen.getByTestId("physics-ground-behavior-trigger");
    expect(trigger).toBeInTheDocument();
    // The aria-label on the Select.Trigger surfaces the field name.
    expect(trigger.getAttribute("aria-label")).toBe("Behavior:");
    // The default value is groundBehavior=0 → "None".
    expect(trigger.textContent ?? "").toContain("None");
    // Opening the listbox in jsdom isn't reliable, but the option set
    // is statically defined — assert via the source-of-truth constant
    // list by inspecting the underlying select primitive once opened.
    // Fallback: render with each value and assert the trigger label.
    for (const [value, label] of [[2, "Bounce"], [3, "Stick"]] as const) {
      const altProps = { ...props, groundBehavior: value };
      const { unmount } = render(<PhysicsTab properties={altProps} onCommit={() => {}} />);
      const altTriggers = screen.getAllByTestId("physics-ground-behavior-trigger");
      // Multiple PhysicsTab instances are mounted; the new one is the
      // last in the list.
      const altTrigger = altTriggers[altTriggers.length - 1]!;
      expect(altTrigger.textContent ?? "").toContain(label);
      unmount();
    }
  });

  it("PhysicsTab: Affected by wind commits affectedByWind on toggle", () => {
    const onCommit = vi.fn();
    const props = { ...makeFixtureProperties(0), isWeatherParticle: false, affectedByWind: false };
    render(<PhysicsTab properties={props} onCommit={onCommit} />);
    const checkbox = screen.getByLabelText("Affected by wind");
    fireEvent.click(checkbox);
    expect(onCommit).toHaveBeenCalledWith({ affectedByWind: true });
  });

  it("PhysicsTab: Affected by wind STAYS ENABLED when weather mode active (legacy parity, Emitter.cpp:175-190)", () => {
    const props = { ...makeFixtureProperties(0), isWeatherParticle: true, affectedByWind: false };
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    // Radix Checkbox renders a <button> for the input; query by role.
    const checkbox = screen.getByRole("checkbox", { name: "Affected by wind" });
    expect(checkbox).not.toHaveAttribute("data-disabled");
  });

  it("PhysicsTab: Parent speed inherit IS disabled when weather mode active (legacy parity, Emitter.cpp:175-190)", () => {
    const props = { ...makeFixtureProperties(0), isWeatherParticle: true };
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    const input = screen.getByLabelText("Parent speed inherit:") as HTMLInputElement;
    expect(input.disabled).toBe(true);
  });

  it("PhysicsTab: Inward speed STAYS ENABLED when weather mode active (legacy parity, Emitter.cpp:175-190)", () => {
    const props = { ...makeFixtureProperties(0), isWeatherParticle: true };
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    const input = screen.getByLabelText("Inward speed:") as HTMLInputElement;
    expect(input.disabled).toBe(false);
  });

  it("PhysicsTab: Parent speed inherit displays * 100 and commits / 100", async () => {
    const onCommit = vi.fn();
    const props = { ...makeFixtureProperties(0), parentLinkStrength: 0.5 };
    render(<PhysicsTab properties={props} onCommit={onCommit} />);
    const input = screen.getByLabelText("Parent speed inherit:") as HTMLInputElement;
    expect(Number(input.value)).toBe(50);
    fireEvent.focus(input);
    fireEvent.change(input, { target: { value: "75" } });
    fireEvent.blur(input);
    await waitFor(() => {
      expect(onCommit).toHaveBeenCalledWith({ parentLinkStrength: 0.75 });
    });
  });

  it("PhysicsTab: only groups[0] and groups[2] render — groups[1] is preserved on wire but absent from the DOM", () => {
    const props = makeFixtureProperties(0);
    render(<PhysicsTab properties={props} onCommit={() => {}} />);
    expect(screen.queryByTestId("physics-group-0")).toBeTruthy();
    expect(screen.queryByTestId("physics-group-1")).toBeNull();
    expect(screen.queryByTestId("physics-group-2")).toBeTruthy();
    // Their Type selectors are also present.
    expect(screen.queryByTestId("physics-group-0-type-trigger")).toBeTruthy();
    expect(screen.queryByTestId("physics-group-2-type-trigger")).toBeTruthy();
  });

  it("PhysicsTab: GroupBody has no fieldset/legend chrome (parent Section carries the title)", () => {
    const props = makeFixtureProperties(0);
    const { container } = render(<PhysicsTab properties={props} onCommit={() => {}} />);
    // The old GroupSection wrapped each group in a <fieldset>; the new
    // GroupBody is a plain <div>.
    expect(container.querySelector("fieldset")).toBeNull();
    expect(container.querySelector("legend")).toBeNull();
    // But the Section header for the parent title is present.
    expect(screen.getByText("Initial position")).toBeInTheDocument();
    expect(screen.getByText("Initial speed")).toBeInTheDocument();
  });

  it("PhysicsTab: GT_SPHERE renders Radius + Constrain-to-surface (no edge spinner, no cylinder fields)", () => {
    const base = makeFixtureProperties(0);
    const groups = base.groups.map((g, i) =>
      // sphereEdge nonzero → the Constrain-to-surface checkbox is checked
      // (the engine treats sphereEdge as a surface-constraint boolean).
      i === 0
        ? { ...g, type: 3, sphereRadius: 1.5, sphereEdge: 8 }
        : g,
    );
    render(<PhysicsTab properties={{ ...base, groups }} onCommit={() => {}} />);
    expect(screen.getByLabelText("Radius:")).toBeInTheDocument();
    const constrain = screen.getByLabelText("Constrain to surface");
    expect(constrain).toBeInTheDocument();
    expect(constrain).toBeChecked();
    // The old numeric "edge" spinner is gone, and no cylinder fields render.
    expect(screen.queryByLabelText("Sphere edge:")).toBeNull();
    expect(screen.queryByLabelText("Cylinder radius")).toBeNull();
    expect(screen.queryByLabelText("Cylinder height")).toBeNull();
  });

  it("PhysicsTab: GT_CYLINDER renders Radius + Height (one row) + Constrain-to-surface; cylinderEdge=0 → unchecked", () => {
    const base = makeFixtureProperties(0);
    const groups = base.groups.map((g, i) =>
      i === 0
        ? { ...g, type: 4, cylinderRadius: 2, cylinderHeight: 3, cylinderEdge: 0 }
        : g,
    );
    render(<PhysicsTab properties={{ ...base, groups }} onCommit={() => {}} />);
    expect(screen.getByLabelText("Cylinder radius")).toBeInTheDocument();
    expect(screen.getByLabelText("Cylinder height")).toBeInTheDocument();
    const constrain = screen.getByLabelText("Constrain to surface");
    expect(constrain).toBeInTheDocument();
    expect(constrain).not.toBeChecked();
    // No numeric "edge" spinner anymore.
    expect(screen.queryByLabelText("Cylinder edge:")).toBeNull();
  });

  it("Tabs.Content outer elements carry overflow-y-auto for panel scroll", async () => {
    const { bridge } = makeStubBridge(0);
    render(<EmitterPropertyTabs bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("emitter-property-tabs")).toBeInTheDocument();
    });
    for (const id of ["tab-basic-content", "tab-appearance-content", "tab-physics-content"]) {
      const el = screen.getByTestId(id);
      expect(el.className).toContain("overflow-y-auto");
    }
  });

  it("BasicTab renders three section headers in order: Emitter Timing, Generation, Connection", async () => {
    const { bridge } = makeStubBridge(0);
    render(<EmitterPropertyTabs bridge={bridge} />);
    // Wait for Basic tab content to populate.
    await waitFor(() => {
      expect(screen.getByTestId("section-emitter-timing")).toBeInTheDocument();
    });
    expect(screen.getByTestId("section-generation")).toBeInTheDocument();
    expect(screen.getByTestId("section-connection")).toBeInTheDocument();
    // DOM order: Emitter Timing first, then Generation, then Connection.
    const timing     = screen.getByTestId("section-emitter-timing");
    const generation = screen.getByTestId("section-generation");
    const connection = screen.getByTestId("section-connection");
    expect(timing.compareDocumentPosition(generation) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(generation.compareDocumentPosition(connection) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });

  it("clicking a BasicTab section header collapses its children", async () => {
    const { bridge } = makeStubBridge(0);
    render(<EmitterPropertyTabs bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByTestId("section-emitter-timing")).toBeInTheDocument();
    });
    // Post-B1.3-P3: Lifetime moved into Generation, so test the
    // collapse via an Emitter Timing field that still lives there
    // (Initial spawn delay).
    const header = screen.getByTestId("section-emitter-timing");
    expect(screen.getByLabelText("Initial spawn delay:")).toBeInTheDocument();
    expect(header).toHaveAttribute("aria-expanded", "true");
    // Collapse Emitter Timing. Post-animation the body stays mounted and
    // collapses via the .collapse-anim wrapper (CSS height + visibility,
    // which jsdom can't observe) — so assert the collapsed STATE, not the
    // field's absence.
    fireEvent.click(header);
    expect(header).toHaveAttribute("aria-expanded", "false");
    const section = header.closest(".panel-section");
    expect(section?.querySelector(".collapse-anim")).toHaveAttribute(
      "data-open",
      "false",
    );
  });

  it("Name row uses the .name-row modifier class for its custom grid", async () => {
    const { bridge } = makeStubBridge(0);
    render(<EmitterPropertyTabs bridge={bridge} />);
    await waitFor(() => {
      expect(screen.getByLabelText("Name")).toBeInTheDocument();
    });
    // The Name field's <input> is inside a div with the .name-row modifier class.
    const nameInput = screen.getByLabelText("Name");
    const row = nameInput.closest('div.form-row') as HTMLElement | null;
    expect(row).not.toBeNull();
    expect(row!.classList.contains("name-row")).toBe(true);
  });

  it("editing Maximum lifetime fires emitters/set-properties with patch.lifetime", async () => {
    // Post-B1.3-P3: "Lifetime" relabelled to "Maximum lifetime:" and
    // moved into Generation. The underlying `lifetime` key is unchanged.
    const { bridge } = makeStubBridge(0, { lifetime: 1.0 });
    render(<EmitterPropertyTabs bridge={bridge} />);
    // B1.3.1: tab strip mounts immediately; wait for the BasicTab form.
    await waitFor(() => {
      expect(screen.getByLabelText("Maximum lifetime:")).toBeInTheDocument();
    });
    const lifetimeInput = screen.getByLabelText("Maximum lifetime:") as HTMLInputElement;
    // Spinner commits on blur after a text edit. Type a new value and blur.
    fireEvent.focus(lifetimeInput);
    fireEvent.change(lifetimeInput, { target: { value: "3.5" } });
    fireEvent.blur(lifetimeInput);
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls;
      const match = calls.find(
        (call) => (call[0] as { kind: string }).kind === "emitters/set-properties",
      );
      expect(match).toBeDefined();
      expect(match![0]).toMatchObject({
        kind: "emitters/set-properties",
        params: {
          id: 0,
          patch: { lifetime: 3.5 },
        },
      });
    });
  });
});
