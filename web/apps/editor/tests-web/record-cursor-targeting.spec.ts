import { expect, test, type Page } from "@playwright/test";

async function seedTargets(page: Page): Promise<void> {
  await page.goto("/");
  await page.evaluate(() => {
    // Drop the live app before seeding. resolveTargetCenter resolves through
    // document.querySelector, so the editor's OWN [data-testid="curve-key"],
    // channel-row and atlas-canvas nodes shadow the fixture below — they come
    // first in document order. That silently made these tests assert against
    // whatever the editor happened to render instead of the fixture, and it
    // broke outright once the curve editor started rendering a channel row
    // scrolled out of view inside a scroll container: the resolver correctly
    // reports a clipped-out element as unresolved, so the fixture's own
    // perfectly visible row never got a look in. These are unit tests of the
    // resolver, so the fixture must be the only DOM it can see.
    document.getElementById("root")?.remove();
    document.body.insertAdjacentHTML(
      "beforeend",
      "\n        <div id=\"record-cursor-targets\" style=\"position:fixed;left:20px;top:30px;z-index:1\">\n          <button data-testid=\"curve-key\" data-channel-id=\"red\" data-key-time=\"0\" style=\"position:absolute;left:10px;top:20px;width:18px;height:18px\">key</button>\n          <canvas data-testid=\"atlas-canvas\" data-atlas-cols=\"4\" data-atlas-cell=\"24\" data-atlas-gap=\"4\" data-atlas-total=\"16\" style=\"position:absolute;left:50px;top:20px;width:108px;height:24px\"></canvas>\n          <div data-testid=\"curve-channel-row-alpha\" style=\"position:absolute;left:10px;top:70px;width:120px;height:28px\">alpha</div>\n        </div>\n      ",
    );
  });
}

test.describe("record cursor semantic targeting", () => {
  test("resolves element target centers with non-zero browser rects", async ({ page }) => {
    await seedTargets(page);
    const result = await page.evaluate(async () => {
      // Vite serves the modules at these URLs; tsc types them from the source.
      const modUrl = "/src/lib/record/record-cursor-eval.ts";
      const mod: typeof import("../src/lib/record/record-cursor-eval") = await import(modUrl);
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
      // Vite serves the modules at these URLs; tsc types them from the source.
      const modUrl = "/src/lib/record/record-cursor-eval.ts";
      const mod: typeof import("../src/lib/record/record-cursor-eval") = await import(modUrl);
      const dockUrl = "/src/lib/dock-anim.ts";
      const dock: typeof import("../src/lib/dock-anim") = await import(dockUrl);
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
      // Vite serves the modules at these URLs; tsc types them from the source.
      const modUrl = "/src/lib/record/record-cursor-eval.ts";
      const mod: typeof import("../src/lib/record/record-cursor-eval") = await import(modUrl);
      const dockUrl = "/src/lib/dock-anim.ts";
      const dock: typeof import("../src/lib/dock-anim") = await import(dockUrl);
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
