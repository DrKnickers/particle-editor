import { describe, it, expect, afterEach } from "vitest";
import { render, screen, cleanup, act } from "@testing-library/react";
import { RecordTitleStrip } from "../RecordTitleStrip";
import { markHeadless, __resetRecordModeForTests } from "@/lib/record-mode";

afterEach(() => {
  cleanup();
  __resetRecordModeForTests();
});

describe("RecordTitleStrip", () => {
  it("renders nothing outside headless record mode", () => {
    render(<RecordTitleStrip currentFilePath="C:/x/P_HP_IMPERIAL_DAMAGE.ALO" dirty={false} />);
    expect(screen.queryByTestId("record-title-strip")).toBeNull();
  });

  it("shows the app name + basename filename in headless mode", () => {
    markHeadless();
    render(<RecordTitleStrip currentFilePath="D:/mods/Data/Art/Models/P_HP_IMPERIAL_DAMAGE.ALO" dirty={false} />);
    const strip = screen.getByTestId("record-title-strip");
    expect(strip.textContent).toContain("Particle Editor");
    expect(strip.textContent).toContain("P_HP_IMPERIAL_DAMAGE.ALO");
    // basename only — no directory
    expect(strip.textContent).not.toContain("D:/mods");
    // clean doc → no dirty dot
    expect(strip.textContent).not.toContain("●");
  });

  it("shows the ● dirty marker when the doc is dirty", () => {
    markHeadless();
    render(<RecordTitleStrip currentFilePath={null} dirty={true} />);
    const strip = screen.getByTestId("record-title-strip");
    expect(strip.textContent).toContain("●");
    expect(strip.textContent).toContain("Untitled.alo"); // null path → Untitled
  });

  it("mounts REACTIVELY when headless latches AFTER the initial render (production order)", () => {
    // Production mounts the strip during normal app render (not headless yet),
    // then flips headless later from the ui/record-headless message. A one-time
    // snapshot instead of a live subscription would leave the strip absent for
    // the whole clip — this pins the reactive path.
    render(<RecordTitleStrip currentFilePath="C:/x/foo.alo" dirty={false} />);
    expect(screen.queryByTestId("record-title-strip")).toBeNull();
    act(() => {
      markHeadless(); // the host's push arrives after mount
    });
    expect(screen.getByTestId("record-title-strip")).not.toBeNull();
  });
});
