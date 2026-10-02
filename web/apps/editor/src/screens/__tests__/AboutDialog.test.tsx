// Vitest unit test for the About dialog. Verifies the version string
// rendered from the Vite-injected `VITE_APP_VERSION` define.

import { describe, it, expect, beforeEach, afterEach, vi } from "vitest";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { AboutDialog } from "../AboutDialog";
import { makeBridgeStub } from "@/test/bridge-stub";
import { EXTERNAL_LINKS } from "@/lib/external-links";
import { __resetStatusFeedbackForTests, useStatusFeedback } from "@/lib/status-feedback";

// Static content makes no link request; the backdrop snapshot runs only with a
// quadrant viewport mounted — a plain stub satisfies the prop.
const bridge = makeBridgeStub();

describe("AboutDialog", () => {
  beforeEach(__resetStatusFeedbackForTests);
  afterEach(() => vi.restoreAllMocks());

  it.each(["opened", "false", "rejected"] as const)("sends one repository request and keeps its address copyable when %s", async (mode) => {
    const user = userEvent.setup();
    const open = vi.spyOn(window, "open").mockReturnValue(null);
    vi.spyOn(console, "warn").mockImplementation(() => {});
    const actionBridge = makeBridgeStub({ responses: {
      "app/open-external": () => {
        if (mode === "rejected") throw new Error("offline");
        return { opened: mode === "opened" };
      },
    } });
    render(<AboutDialog bridge={actionBridge} open onOpenChange={() => {}} />);
    await user.click(screen.getByRole("button", { name: "Open project page" }));
    await waitFor(() => {
      expect(actionBridge.request.mock.calls.filter(([req]) => req.kind === "app/open-external"))
        .toEqual([[{ kind: "app/open-external", params: { target: "repository" } }]]);
    });
    expect(open).not.toHaveBeenCalled();
    if (mode !== "opened") {
      expect(useStatusFeedback.getState().message).toContain("Copy the project address");
    }
    const input = screen.getByRole("textbox", { name: "Project address" });
    expect(input).toHaveValue(EXTERNAL_LINKS.repository);
    expect(input).toHaveAttribute("readonly");
    expect(input).toHaveClass("select-text");
    await user.keyboard("{Tab}");
    expect(input).toHaveFocus();
    await user.keyboard("{Control>}a{/Control}");
    await user.copy();
    expect(await navigator.clipboard.readText()).toBe(EXTERNAL_LINKS.repository);
  });

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
