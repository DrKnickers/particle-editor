// timeline-panel.spec.ts — the timeline panel in a real browser against the
// MOCK app (the mock keeps the host's session state machine; play does not
// advance there). Open from the toolbar, drive every control, close, and
// check a reload starts closed. The native half is tests/timeline.spec.ts.
import { test, expect, type Page } from "@playwright/test";

async function boot(page: Page) {
  await page.goto("/");
  await page.waitForSelector('[data-testid="emitter-tree"]');
}

test("the panel is closed by default and opens under the viewport from the toolbar", async ({ page }) => {
  await boot(page);
  const panel = page.getByRole("region", { name: "Timeline" });
  await expect(panel).toHaveCount(0);
  const viewport = page.getByTestId("quadrant-viewport");
  const before = await viewport.boundingBox();

  await page.getByRole("button", { name: "Toggle timeline" }).click();
  await expect(panel).toBeVisible();
  await expect(page.getByRole("button", { name: "Toggle timeline" })).toHaveAttribute("aria-pressed", "true");
  // The row sits under the viewport rect, which shrinks to make room.
  const after = await viewport.boundingBox();
  const row = await panel.boundingBox();
  expect(after!.height).toBeLessThan(before!.height);
  expect(row!.y).toBeGreaterThanOrEqual(after!.y + after!.height - 1);
});

test("every control drives the session", async ({ page }) => {
  await boot(page);
  await page.getByRole("button", { name: "Toggle timeline" }).click();
  const panel = page.getByRole("region", { name: "Timeline" });
  const readout = panel.getByTestId("timeline-readout");
  await expect(readout).toContainText("Frame 0 / 600");

  await panel.getByRole("button", { name: "Play" }).click();
  await expect(panel.getByRole("button", { name: "Pause" })).toHaveAttribute("aria-pressed", "true");
  await panel.getByRole("button", { name: "Pause" }).click();

  await panel.getByRole("button", { name: "Next frame" }).click();
  await expect(readout).toContainText("Frame 1 / 600");
  await page.getByRole("button", { name: "Step 10" }).click(); // the toolbar reroutes too
  await expect(readout).toContainText("Frame 11 / 600");
  await panel.getByRole("button", { name: "Previous frame" }).click();
  await expect(readout).toContainText("Frame 10 / 600");

  const playhead = panel.getByRole("slider", { name: "Playhead" });
  await playhead.fill("300");
  await expect(readout).toContainText("Time since spawn 5.000 s · Frame 300 / 600");
  await playhead.focus();
  await page.keyboard.press("End");
  await expect(readout).toContainText("Frame 600 / 600");
  await page.keyboard.press("Home");
  await expect(readout).toContainText("Frame 0 / 600");

  await panel.getByRole("button", { name: "Jump to end" }).click();
  await expect(readout).toContainText("Frame 600");
  await panel.getByRole("button", { name: "Reset" }).click();
  await expect(readout).toContainText("Frame 0 / 600");

  const loop = panel.getByRole("button", { name: "Loop" });
  await loop.click();
  await expect(loop).toHaveAttribute("aria-pressed", "true");

  await panel.getByText("Seed").click();
  const seed = panel.getByTestId("timeline-seed");
  const first = await seed.textContent();
  await panel.getByRole("button", { name: "New variation" }).click();
  await expect(seed).not.toHaveText(first ?? "");

  await panel.getByRole("button", { name: "Close timeline" }).click();
  await expect(panel).toHaveCount(0);
  await expect(page.getByRole("button", { name: "Toggle timeline" })).toHaveAttribute("aria-pressed", "false");
});

test("View → Timeline toggles the panel, and a reload starts closed", async ({ page }) => {
  await boot(page);
  await page.click('[role="menubar"] [role="menuitem"]:has-text("View")');
  await page.getByTestId("menu-view-timeline").click();
  await expect(page.getByRole("region", { name: "Timeline" })).toBeVisible();
  await page.reload();
  await page.waitForSelector('[data-testid="emitter-tree"]');
  await expect(page.getByRole("region", { name: "Timeline" })).toHaveCount(0);
});
