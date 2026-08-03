import { readFileSync } from "node:fs";

// Single parser for the app version. The canonical source is the C header
// src/version.h (PE_VERSION_STR), shared with the binary's VS_VERSION_INFO
// and the legacy Win32 About. Both vite.config.ts (build-time injection into
// the React About) and vitest.config.ts (the test-time mirror of that inject)
// call this so there is exactly ONE place that knows the header format.
//
// Returns "unknown" if the header is missing or unparseable, so a detached
// build (release tarball without the C sources) still renders the dialog.
export function readAppVersion(versionHeaderPath: string): string {
  try {
    const header = readFileSync(versionHeaderPath, "utf8");
    return header.match(/PE_VERSION_STR\b\s+"([^"]+)"/)?.[1] ?? "unknown";
  } catch {
    return "unknown";
  }
}
