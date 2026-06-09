import { afterEach, describe, expect, it, vi } from "vitest";
import { isLegacyMode } from "@/lib/hosting-mode";

// Mirrors the env-stub pattern the ViewportSlot tests use: vi.stubEnv writes
// process.env, which isLegacyMode reads alongside import.meta.env. NOTE: under
// vitest only the process.env clause is reachable — vi.stubEnv does NOT touch
// import.meta.env (and VITE_HOSTING_MODE isn't in vitest.config's `define`), so
// the `fromImportMeta === "legacy"` clause (the path Vite bakes at production
// build) is dead here. These cases are non-vacuous (the process.env side flips
// the result), but full coverage of the import.meta clause is a production-build
// concern, not a unit-test one.
describe("isLegacyMode", () => {
  afterEach(() => {
    vi.unstubAllEnvs();
  });

  it("defaults to false (architecture C) when VITE_HOSTING_MODE is unset", () => {
    expect(isLegacyMode()).toBe(false);
  });

  it("is true when VITE_HOSTING_MODE=legacy", () => {
    vi.stubEnv("VITE_HOSTING_MODE", "legacy");
    expect(isLegacyMode()).toBe(true);
  });

  it("is false for any non-legacy value", () => {
    vi.stubEnv("VITE_HOSTING_MODE", "composition");
    expect(isLegacyMode()).toBe(false);
  });
});
