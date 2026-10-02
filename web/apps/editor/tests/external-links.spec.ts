// Fixed external-link targets never launch a browser under --test-host.
import { test, expect, type Page } from "./helpers/cdp";
import { bridgeRequest } from "./helpers/bridge-request";

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

for (const target of ["guide", "repository"] as const) {
  test(`app/open-external suppresses ${target} under --test-host`, async () => {
    const before = await bridgeRequest(page, { kind: "engine/state/snapshot", params: {} });
    expect(await bridgeRequest(page, { kind: "app/open-external", params: { target } }))
      .toEqual({ opened: false });
    const after = await bridgeRequest(page, { kind: "engine/state/snapshot", params: {} });
    expect(after.dirty).toBe(before.dirty);
  });
}

for (const target of ["", "Guide", "Repository", "https://example.com", "file:///C:/test"]) {
  test(`app/open-external rejects unknown target ${target}`, async () => {
    await expect(bridgeRequest(page, {
      kind: "app/open-external",
      params: { target: target as "guide" },
    })).rejects.toThrow("app/open-external: unknown target");
  });
}
