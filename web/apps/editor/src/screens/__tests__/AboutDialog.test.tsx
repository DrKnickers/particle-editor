// Vitest unit test for the About dialog. Verifies the version string
// rendered from the Vite-injected `VITE_APP_VERSION` define.

import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import { AboutDialog } from "../AboutDialog";

describe("AboutDialog", () => {
  it("renders the version string from VITE_APP_VERSION", () => {
    render(<AboutDialog open onOpenChange={() => {}} />);
    // VITE_APP_VERSION is defined as "1.5" in vite.config.ts. We assert
    // on the pattern "Version <digits>(.<digits>)?" so the test stays
    // green when the constant bumps.
    expect(screen.getByText(/Version \d+(\.\d+)?/)).toBeInTheDocument();
  });

  it("shows the upstream fork attribution", () => {
    render(<AboutDialog open onOpenChange={() => {}} />);
    expect(
      screen.getByText(/Forked from Mike\.NL's GlyphX Particle Editor v1\.5/),
    ).toBeInTheDocument();
  });
});
