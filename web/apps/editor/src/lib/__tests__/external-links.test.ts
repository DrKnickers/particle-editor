import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import path from "node:path";
import { EXTERNAL_LINKS } from "../external-links";

// Vitest runs with cwd = web/apps/editor; the native policy is at repo root.
const HEADER = path.resolve(process.cwd(), "../../../src/host/ExternalLinks.h");

describe("external links", () => {
  it("keeps both mock/fallback URLs in sync with the native constants", () => {
    const text = readFileSync(HEADER, "utf8");
    expect(text.match(/GUIDE_URL\[\]\s*=\s*L"([^"]+)"/)?.[1]).toBe(EXTERNAL_LINKS.guide);
    expect(text.match(/REPOSITORY_URL\[\]\s*=\s*L"([^"]+)"/)?.[1]).toBe(EXTERNAL_LINKS.repository);
  });
});
