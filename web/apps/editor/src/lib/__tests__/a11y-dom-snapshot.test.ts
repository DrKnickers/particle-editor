import { describe, expect, it, vi } from "vitest";
import type { Locator, Page } from "@playwright/test";
import { captureDomA11y } from "../../../tests/helpers/a11y-dom-snapshot";

describe("captureDomA11y", () => {
  it("keeps body as the default shell baseline", async () => {
    const ariaSnapshot = vi.fn().mockResolvedValue("shell\n");
    const locator = vi.fn().mockReturnValue({ ariaSnapshot });
    const waitForFunction = vi.fn().mockResolvedValue(undefined);
    const page = { locator, waitForFunction } as unknown as Page;

    expect(await captureDomA11y(page)).toBe("shell\n");
    expect(locator).toHaveBeenCalledWith("body");
    expect(waitForFunction).toHaveBeenCalledTimes(2);
    expect(ariaSnapshot).toHaveBeenCalledTimes(1);
  });

  it("settles the page before snapshotting only the supplied surface", async () => {
    const calls: string[] = [];
    const waitForFunction = vi.fn(async () => {
      calls.push("settle");
    });
    const locator = vi.fn();
    const ariaSnapshot = vi.fn(async () => {
      calls.push("surface");
      return "dialog\n";
    });
    const page = { locator, waitForFunction } as unknown as Page;
    const scope = { ariaSnapshot } as unknown as Locator;

    expect(await captureDomA11y(page, scope)).toBe("dialog\n");
    expect(calls).toEqual(["settle", "settle", "surface"]);
    expect(locator).not.toHaveBeenCalled();
  });

  it("does not snapshot a scoped surface when settling fails", async () => {
    const timeout = Object.assign(new Error("Timeout exceeded"), { name: "TimeoutError" });
    const page = { waitForFunction: vi.fn().mockRejectedValue(timeout) } as unknown as Page;
    const ariaSnapshot = vi.fn();

    await expect(captureDomA11y(page, { ariaSnapshot } as unknown as Locator)).rejects.toThrow(/stuck animation/);
    expect(ariaSnapshot).not.toHaveBeenCalled();
  });
});
