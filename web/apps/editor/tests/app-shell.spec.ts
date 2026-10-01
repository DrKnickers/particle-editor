// StatusBar contract tests: DOM presence and live
// stats/tick delivery from the C++ host at 4 Hz. Sibling of
// bridge-native.spec.ts and background-picker.spec.ts — same CDP-attach
// harness, same window.bridge host-object channel.
import { test, expect, type Page } from "./helpers/cdp";

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

test("StatusBar renders the 4 stat columns", async () => {
  const text = await page.evaluate(() => document.querySelector("footer")?.textContent ?? "");
  expect(text).toContain("FPS");
  expect(text).toContain("Emitters");
  expect(text).toContain("Particles");
  expect(text).toContain("Instances");
});

test("stats/tick event reaches the React StatusBar within 1.5s", async () => {
  // The host emits every 250ms. Wait up to 1.5s for the placeholder dashes
  // to be replaced by real numbers.
  const final = await page.evaluate(async () => {
    return new Promise<{ text: string; receivedTick: boolean }>((resolve) => {
      let receivedTick = false;
      const off = window.bridge!.on("stats/tick", () => {
        receivedTick = true;
      });
      setTimeout(() => {
        off();
        resolve({
          text: document.querySelector("footer")?.textContent ?? "",
          receivedTick,
        });
      }, 1500);
    });
  });
  expect(final.receivedTick).toBe(true);
  // The footer text should no longer have the placeholder dashes.
  // Check at least one digit appeared in the FPS column.
  expect(final.text).toMatch(/FPS\s*\d/);
});
