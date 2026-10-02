// Vitest specs for the tri-state Generation radio mutex.
//
// Replaces the legacy `Use Bursts` checkbox with a three-radio mutex
// (Bursts / Continuous stream / Weather particle) deriving from
// (useBursts, isWeatherParticle). Each radio click commits one atomic
// two-key patch so the engine never sees a transient inconsistent
// state pair.
//
// The Weather sub-fields (Particles / Distance from camera / Cube size)
// live under the Weather radio branch — moved away from the Physics tab
// where they sat in the previous UI.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render as rtlRender, screen, fireEvent } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import { BasicTab } from "../EmitterPropertyTabs";
import { makeFixtureProperties } from "@/bridge/mock-state";

// BasicTab mounts Tips (Radix Tooltip.Root) on the form-row labels,
// which require the app-level Tooltip.Provider — wrapper stands in for it
// (precedent: renderWithTooltips in EmitterTree.test.tsx).
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

describe("BasicTab — tri-state Generation mutex", () => {
  let onCommit: ReturnType<typeof vi.fn>;
  beforeEach(() => {
    onCommit = vi.fn();
  });

  const renderWithMode = (useBursts: boolean, isWeather: boolean) => {
    const properties = {
      ...makeFixtureProperties(0),
      useBursts,
      isWeatherParticle: isWeather,
    };
    render(<BasicTab properties={properties} onCommit={onCommit} />);
  };

  it("renders three radios for bursts / continuous / weather", () => {
    renderWithMode(false, false);
    expect(screen.getByRole("radio", { name: /Bursts/i })).toBeTruthy();
    expect(screen.getByRole("radio", { name: /Continuous/i })).toBeTruthy();
    expect(screen.getByRole("radio", { name: /Weather/i })).toBeTruthy();
  });

  it("active radio reflects (useBursts=true, isWeather=false) → bursts", () => {
    renderWithMode(true, false);
    expect(screen.getByRole("radio", { name: /Bursts/i }).getAttribute("aria-checked")).toBe("true");
    expect(screen.getByRole("radio", { name: /Continuous/i }).getAttribute("aria-checked")).toBe("false");
    expect(screen.getByRole("radio", { name: /Weather/i }).getAttribute("aria-checked")).toBe("false");
  });

  it("active radio reflects (useBursts=true, isWeather=true) → weather", () => {
    renderWithMode(true, true);
    expect(screen.getByRole("radio", { name: /Weather/i }).getAttribute("aria-checked")).toBe("true");
    expect(screen.getByRole("radio", { name: /Bursts/i }).getAttribute("aria-checked")).toBe("false");
    expect(screen.getByRole("radio", { name: /Continuous/i }).getAttribute("aria-checked")).toBe("false");
  });

  it("active radio reflects (useBursts=false, isWeather=true) → weather", () => {
    renderWithMode(false, true);
    expect(screen.getByRole("radio", { name: /Weather/i }).getAttribute("aria-checked")).toBe("true");
    expect(screen.getByRole("radio", { name: /Bursts/i }).getAttribute("aria-checked")).toBe("false");
    expect(screen.getByRole("radio", { name: /Continuous/i }).getAttribute("aria-checked")).toBe("false");
  });

  it("clicking Bursts commits both keys atomically", () => {
    renderWithMode(false, false);
    fireEvent.click(screen.getByRole("radio", { name: /Bursts/i }));
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith({ useBursts: true, isWeatherParticle: false });
  });

  it("clicking Continuous commits both keys atomically", () => {
    renderWithMode(true, false);
    fireEvent.click(screen.getByRole("radio", { name: /Continuous/i }));
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith({ useBursts: false, isWeatherParticle: false });
  });

  it("clicking Weather sets only isWeatherParticle (preserves useBursts)", () => {
    renderWithMode(true, false);
    fireEvent.click(screen.getByRole("radio", { name: /Weather/i }));
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith({ isWeatherParticle: true });
  });

  it("burst sub-fields disabled when mode != bursts", () => {
    renderWithMode(false, false);
    expect((screen.getByLabelText(/Bursts:/i) as HTMLInputElement).disabled).toBe(true);
    expect((screen.getByLabelText(/Particles\/burst:/i) as HTMLInputElement).disabled).toBe(true);
  });

  it("weather sub-fields disabled when mode != weather", () => {
    renderWithMode(false, false);
    expect((screen.getByLabelText(/Cube size:/i) as HTMLInputElement).disabled).toBe(true);
    expect((screen.getByLabelText(/Distance from camera:/i) as HTMLInputElement).disabled).toBe(true);
  });

  it("ArrowDown on Bursts cycles forward to Continuous", () => {
    // Roving-tabindex + arrow-nav per WAI-ARIA radio group pattern.
    // ArrowDown from bursts → continuous; from continuous → weather;
    // from weather → bursts (cyclical). We assert one hop here and
    // trust the modulo math to handle the rest.
    renderWithMode(true, false);
    fireEvent.keyDown(screen.getByRole("radio", { name: /Bursts/i }), { key: "ArrowDown" });
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith({ useBursts: false, isWeatherParticle: false });
  });

  it("describes timing, lifetime and linking without guessing mesh emission meanings", () => {
    renderWithMode(false, false);
    const descriptions = [
      ["Skip time:", "Pre-spawns non-weather particles from this many seconds earlier."],
      ["Freeze time:", "Freezes emitter time after this many seconds, counting Skip time; 0 or a value below Skip time disables freezing."],
      ["Minimum lifetime:", "Percent of maximum lifetime; each particle gets a random lifespan between that fraction and the maximum."],
      ["Link particles to instance", "Makes already-spawned particles follow the effect instance's position, without rotating them."],
      ["Emit mode:", "Saved in the file. This preview does not use it."],
      ["Emit offset:", "Available when Emit mode is not Disable."],
    ] as const;
    const ids: string[] = [];
    for (const [label, help] of descriptions) {
      const id = screen.getByLabelText(label).getAttribute("aria-describedby");
      expect(id).toBeTruthy();
      ids.push(id!);
      expect(document.getElementById(id!)).toHaveAttribute("hidden");
      expect(document.getElementById(id!)).toHaveTextContent(help);
    }
    expect(new Set(ids).size).toBe(descriptions.length);
    expect(screen.getByLabelText("Emit offset:")).toBeDisabled();
  });

  it("disabled Emit offset explains the Emit mode dependency without committing on keyboard activation", async () => {
    const user = userEvent.setup();
    renderWithMode(false, false);
    const control = screen.getByLabelText("Emit offset:");
    const id = control.getAttribute("aria-describedby");
    expect(control).toBeDisabled();
    expect(id).toBeTruthy();
    expect(document.getElementById(id!)).toHaveAttribute("hidden");
    expect(document.getElementById(id!)).toHaveTextContent("Available when Emit mode is not Disable.");
    const label = screen.getByText("Emit offset:");
    expect(label).toHaveAttribute("tabindex", "0");
    expect(label).toHaveAttribute("aria-describedby", id);
    screen.getByRole("combobox", { name: "Emit mode:" }).focus();
    await user.tab();
    expect(label).toHaveFocus();
    await user.keyboard("{Enter} ");
    expect(onCommit).not.toHaveBeenCalled();
  });

  it.each([1, 2, 3])("enabled Emit offset describes preview behaviour and preserves its commit payload in Emit mode %s", (emitFromMesh) => {
    const properties = { ...makeFixtureProperties(0), emitFromMesh };
    render(<BasicTab properties={properties} onCommit={onCommit} />);
    const ids = ["Emit mode:", "Emit offset:"].map((label) => {
      const control = screen.getByLabelText(label);
      const id = control.getAttribute("aria-describedby");
      expect(control).toBeEnabled();
      expect(id).toBeTruthy();
      expect(document.getElementById(id!)).toHaveAttribute("hidden");
      expect(document.getElementById(id!)).toHaveTextContent("Saved in the file. This preview does not use it.");
      expect(document.getElementById(id!)).not.toHaveTextContent("Available when Emit mode is not Disable.");
      expect(screen.getByText(label)).not.toHaveAttribute("tabindex");
      return id;
    });
    expect(new Set(ids).size).toBe(2);
    const control = screen.getByLabelText("Emit offset:");
    fireEvent.focus(control);
    fireEvent.change(control, { target: { value: "2.5" } });
    fireEvent.blur(control);
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith({ emitFromMeshOffset: 2.5 });
  });

  it("help preserves timing, minimum-lifetime and linking commit payloads", () => {
    renderWithMode(false, false);
    for (const [label, value, patch] of [
      ["Skip time:", "2.5", { skipTime: 2.5 }],
      ["Freeze time:", "4.5", { freezeTime: 4.5 }],
      ["Minimum lifetime:", "25", { randomLifetimePerc: 0.75 }],
    ] as const) {
      onCommit.mockClear();
      const input = screen.getByLabelText(label);
      fireEvent.focus(input);
      fireEvent.change(input, { target: { value } });
      fireEvent.blur(input);
      expect(onCommit).toHaveBeenCalledTimes(1);
      expect(onCommit).toHaveBeenCalledWith(patch);
    }
    onCommit.mockClear();
    const checkbox = screen.getByRole("checkbox", { name: "Link particles to instance" }) as HTMLInputElement;
    const next = !checkbox.checked;
    fireEvent.click(checkbox);
    expect(onCommit).toHaveBeenCalledTimes(1);
    expect(onCommit).toHaveBeenCalledWith({ linkToSystem: next });
  });
});
