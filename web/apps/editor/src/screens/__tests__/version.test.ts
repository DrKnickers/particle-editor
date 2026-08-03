import { describe, expect, it } from "vitest";
import { readFileSync } from "node:fs";
import path from "node:path";

// src/version.h is the single source of truth for the app version, shared by
// the C++ binary (VS_VERSION_INFO + the legacy Win32 About) and this React
// UI's About (injected as VITE_APP_VERSION by vite.config.ts, parsed from the
// same header). These guards fail loudly if that wiring breaks — either the
// Vite injection silently falling back to "unknown", or the header's
// PE_VERSION_STR drifting from its PE_VERSION_MAJOR/MINOR/PATCH integers.

// Vitest runs with cwd = web/apps/editor (the package dir); the canonical
// header lives at the repo root, three levels up.
const VERSION_H = path.resolve(process.cwd(), "../../../src/version.h");

function readHeader() {
  const text = readFileSync(VERSION_H, "utf8");
  const str = text.match(/PE_VERSION_STR\b\s+"([^"]+)"/)?.[1];
  const major = text.match(/PE_VERSION_MAJOR\b\s+(\d+)/)?.[1];
  const minor = text.match(/PE_VERSION_MINOR\b\s+(\d+)/)?.[1];
  const patch = text.match(/PE_VERSION_PATCH\b\s+(\d+)/)?.[1];
  return { str, major, minor, patch };
}

describe("app version", () => {
  it("injects a real semver string into the About dialog (never 'unknown')", () => {
    const injected = import.meta.env.VITE_APP_VERSION;
    expect(injected).toMatch(/^\d+\.\d+\.\d+$/);
  });

  it("keeps PE_VERSION_STR in sync with the integer components", () => {
    const { str, major, minor, patch } = readHeader();
    expect(str).toBe(`${major}.${minor}.${patch}`);
  });

  it("injects the same version the header declares", () => {
    const { str } = readHeader();
    expect(import.meta.env.VITE_APP_VERSION).toBe(str);
  });
});
