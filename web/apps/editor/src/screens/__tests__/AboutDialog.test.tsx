// Vitest unit test for the About dialog. Verifies the version string
// rendered from the Vite-injected `VITE_APP_VERSION` define.

import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import { AboutDialog } from "../AboutDialog";
import { makeBridgeStub } from "@/test/bridge-stub";

// Only the dialog backdrop snapshot reaches the bridge, and only with a
// quadrant viewport mounted — a plain stub satisfies the prop.
const bridge = makeBridgeStub();

describe("AboutDialog", () => {
  it("renders the version string from VITE_APP_VERSION", () => {
    render(<AboutDialog bridge={bridge} open onOpenChange={() => {}} />);
    // VITE_APP_VERSION is injected from src/version.h (PE_VERSION_STR) by
    // vite.config.ts. We assert on the SemVer pattern "Version X.Y.Z" so the
    // test stays green when the version bumps.
    expect(screen.getByText(/Version \d+\.\d+\.\d+/)).toBeInTheDocument();
  });

  it("shows the upstream fork attribution", () => {
    render(<AboutDialog bridge={bridge} open onOpenChange={() => {}} />);
    expect(
      screen.getByText(/Forked from Mike\.NL's GlyphX Particle Editor v1\.5/),
    ).toBeInTheDocument();
  });
});
