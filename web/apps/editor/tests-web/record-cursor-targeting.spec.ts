import { expect, test, type Page } from "@playwright/test";

async function seedTargets(page: Page): Promise<void> {
  await page.goto("/");
  await page.evaluate(() => {
    document.body.insertAdjacentHTML(
      "beforeend",
      "\n        <div id=\"record-cursor-targets\" style=\"position:fixed;left:20px;top:30px;z-index:1\">\n          <button data-testid=\"curve-key\" data-channel-id=\"red\" data-key-time=\"0\" style=\"position:absolute;left:10px;top:20px;width:18px;height:18px\">key</button>\n          <button data-testid=\"atlas-cell\" data-frame=\"3\" style=\"position:absolute;left:50px;top:20px;width:24px;height:24px\">3</button>\n          <div data-testid=\"curve-channel-row-alpha\" style=\"position:absolute;left:10px;top:70px;width:120px;height:28px\">alpha</div>\n        </div>\n      ",
    );
  });
}

test.describe("record cursor semantic targeting", () => {
  test("resolves element target centers with non-zero browser rects", async ({ page }) => {
    await seedTargets(page);
    const result = await page.evaluate(async () => {
      const mod = await import("/src/lib/record-cursor-eval.ts");
      return {
        curve: mod.resolveTargetCenter({ kind: "element", ref: "curve-key:red:0" }),
        row: mod.resolveTargetCenter({ kind: "element", ref: "channel-row:alpha" }),
      };
    });

    for (const resolved of Object.values(result)) {
      expect(resolved.ok).toBe(true);
      expect(Number.isFinite(resolved.x)).toBe(true);
      expect(Number.isFinite(resolved.y)).toBe(true);
      expect(resolved.x).toBeGreaterThan(0);
      expect(resolved.y).toBeGreaterThan(0);
    }
  });

  test("reports absent refs and unsettled atlas tiles as unresolved", async ({ page }) => {
    await seedTargets(page);
    const result = await page.evaluate(async () => {
      const mod = await import("/src/lib/record-cursor-eval.ts");
      const dock = await import("/src/lib/dock-anim.ts");
      const absent = mod.resolveTargetCenter({ kind: "element", ref: "curve-key:red:404" });
      dock.useDockAnim.getState().setAtlasGridMounted(true);
      dock.useDockAnim.getState().setAnimating(true);
      const unsettledAtlas = mod.resolveTargetCenter({ kind: "element", ref: "atlas-tile:3" });
      dock.useDockAnim.getState().setAnimating(false);
      dock.useDockAnim.getState().setAtlasGridMounted(false);
      return { absent, unsettledAtlas };
    });

    expect(result.absent.ok).toBe(false);
    expect(result.unsettledAtlas.ok).toBe(false);
  });

  test("resolves atlas tiles only after the atlas grid is settled", async ({ page }) => {
    await seedTargets(page);
    const resolved = await page.evaluate(async () => {
      const mod = await import("/src/lib/record-cursor-eval.ts");
      const dock = await import("/src/lib/dock-anim.ts");
      dock.useDockAnim.getState().setAtlasGridMounted(true);
      dock.useDockAnim.getState().setAnimating(false);
      const atlas = mod.resolveTargetCenter({ kind: "element", ref: "atlas-tile:3" });
      dock.useDockAnim.getState().setAtlasGridMounted(false);
      return atlas;
    });

    expect(resolved.ok).toBe(true);
    expect(Number.isFinite(resolved.x)).toBe(true);
    expect(Number.isFinite(resolved.y)).toBe(true);
    expect(resolved.x).toBeGreaterThan(0);
    expect(resolved.y).toBeGreaterThan(0);
  });
});
