import { describe, it, expect } from "vitest";
import { initialAutoOpen, autoOpenReducer, type AutoOpenState } from "../atlas-autoopen";

const arm = (s: AutoOpenState) => autoOpenReducer(s, { type: "focusIndex" }).state;

describe("auto-open state machine (spec §3.2)", () => {
  it("arms on index focus", () => {
    expect(initialAutoOpen.armed).toBe(false);
    expect(arm(initialAutoOpen)).toEqual({ armed: true, active: false, remembered: null });
  });
  it("opens on first key selection when armed + eligible, remembering current dock", () => {
    const s = arm(initialAutoOpen);
    const r = autoOpenReducer(s, { type: "keySelected", eligible: true, currentDock: "lighting" });
    expect(r.command).toEqual({ type: "open", remembered: "lighting" });
    expect(r.state).toEqual({ armed: false, active: true, remembered: "lighting" });
  });
  it("does not open when not eligible; keeps armed for a later eligible selection", () => {
    const s = arm(initialAutoOpen);
    const r1 = autoOpenReducer(s, { type: "keySelected", eligible: false, currentDock: null });
    expect(r1.command).toEqual({ type: "none" });
    expect(r1.state.armed).toBe(true);
    const r2 = autoOpenReducer(r1.state, { type: "keySelected", eligible: true, currentDock: null });
    expect(r2.command).toEqual({ type: "open", remembered: null });
  });
  it("does not re-open when dock is already atlas", () => {
    const s = arm(initialAutoOpen);
    const r = autoOpenReducer(s, { type: "keySelected", eligible: true, currentDock: "atlas" });
    expect(r.command).toEqual({ type: "none" });
  });
  it("restores remembered panel on focus leaving index", () => {
    let s = arm(initialAutoOpen);
    s = autoOpenReducer(s, { type: "keySelected", eligible: true, currentDock: "lighting" }).state;
    const r = autoOpenReducer(s, { type: "focusOther" });
    expect(r.command).toEqual({ type: "restore", to: "lighting" });
    expect(r.state).toEqual({ armed: false, active: false, remembered: null });
  });
  it("restores on emitter cleared and on eligibility lost", () => {
    let s = arm(initialAutoOpen);
    s = autoOpenReducer(s, { type: "keySelected", eligible: true, currentDock: "spawner" }).state;
    expect(autoOpenReducer(s, { type: "emitterCleared" }).command).toEqual({ type: "restore", to: "spawner" });
    expect(autoOpenReducer(s, { type: "eligibilityLost" }).command).toEqual({ type: "restore", to: "spawner" });
  });
  it("CANCEL RULE: a user dock mutation while active disarms restore", () => {
    let s = arm(initialAutoOpen);
    s = autoOpenReducer(s, { type: "keySelected", eligible: true, currentDock: "lighting" }).state;
    const cancelled = autoOpenReducer(s, { type: "userDockMutation" });
    expect(cancelled.command).toEqual({ type: "none" });
    expect(cancelled.state).toEqual({ armed: false, active: false, remembered: null });
    expect(autoOpenReducer(cancelled.state, { type: "focusOther" }).command).toEqual({ type: "none" });
  });
  it("focus round-trip cannot capture the picker itself as remembered", () => {
    let s = arm(initialAutoOpen);
    s = autoOpenReducer(s, { type: "keySelected", eligible: true, currentDock: "lighting" }).state;
    s = autoOpenReducer(s, { type: "focusOther" }).state;
    s = autoOpenReducer(s, { type: "focusIndex" }).state;
    const r = autoOpenReducer(s, { type: "keySelected", eligible: true, currentDock: "lighting" });
    expect(r.command).toEqual({ type: "open", remembered: "lighting" });
  });
  it("inactive transitions are no-ops", () => {
    expect(autoOpenReducer(initialAutoOpen, { type: "focusOther" }).command).toEqual({ type: "none" });
    expect(autoOpenReducer(initialAutoOpen, { type: "emitterCleared" }).command).toEqual({ type: "none" });
    expect(autoOpenReducer(initialAutoOpen, { type: "userDockMutation" }).command).toEqual({ type: "none" });
  });
});
