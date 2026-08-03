import { beforeEach, describe, expect, it } from "vitest";
import { useDockAnim } from "@/lib/dock-anim";

// Module-level store — reset between cases.
describe("dock-anim signal store", () => {
  beforeEach(() => useDockAnim.setState({ animating: false }));

  it("defaults to not animating", () => {
    expect(useDockAnim.getState().animating).toBe(false);
  });

  it("setAnimating raises and lowers the signal", () => {
    useDockAnim.getState().setAnimating(true);
    expect(useDockAnim.getState().animating).toBe(true);
    useDockAnim.getState().setAnimating(false);
    expect(useDockAnim.getState().animating).toBe(false);
  });

  it("notifies subscribers on each change (the ViewportSlot ref-sync path)", () => {
    const seen: boolean[] = [];
    const unsub = useDockAnim.subscribe((s) => seen.push(s.animating));
    useDockAnim.getState().setAnimating(true);
    useDockAnim.getState().setAnimating(false);
    unsub();
    useDockAnim.getState().setAnimating(true); // after unsub — not seen
    expect(seen).toEqual([true, false]);
  });
});
