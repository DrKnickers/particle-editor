// Vitest specs for the tri-state Generation radio mutex (P3, B1.3).
//
// Replaces the legacy `Use Bursts` checkbox with a three-radio mutex
// (Bursts / Continuous stream / Weather particle) deriving from
// (useBursts, isWeatherParticle). Each radio click commits one atomic
// two-key patch so the engine never sees a transient inconsistent
// state pair.
//
// The Weather sub-fields (Particles / Distance from camera / Cube size)
// live under the Weather radio branch — moved away from the Physics tab
// where they sat in the pre-B1.3 UI.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { BasicTab } from "../EmitterPropertyTabs";
import { makeFixtureProperties } from "@/bridge/mock-state";

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
});
