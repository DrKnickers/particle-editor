// Vitest for the AutosaveRecoveryDialog (crash recovery).
//
// Covers the pure view's 3-state variants + deterministic age text, and the
// container's check-recovery → open and recover dispatch wiring.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import type { AutosaveOrphan, Bridge } from "@particle-editor/bridge-schema";
import {
  AutosaveRecoveryView,
  AutosaveRecoveryDialog,
  formatAutosaveAge,
} from "../AutosaveRecoveryDialog";

const NOW = 1_700_000_000_000;
function orphan(p: Partial<AutosaveOrphan> = {}): AutosaveOrphan {
  return {
    originalFilename: "fire.alo",
    recentMtimeMs: NOW - 45_000,        // 45 seconds ago
    stableMtimeMs: NOW - 8 * 60_000,    // 8 minutes ago
    ...p,
  };
}

describe("formatAutosaveAge", () => {
  it("renders coarse units, mirroring legacy FormatAge", () => {
    expect(formatAutosaveAge(NOW, NOW)).toBe("just now");
    expect(formatAutosaveAge(NOW - 45_000, NOW)).toBe("45 seconds ago");
    expect(formatAutosaveAge(NOW - 60_000, NOW)).toBe("1 minute ago");
    expect(formatAutosaveAge(NOW - 8 * 60_000, NOW)).toBe("8 minutes ago");
    expect(formatAutosaveAge(NOW - 2 * 3600_000, NOW)).toBe("2 hours ago");
  });
});

describe("AutosaveRecoveryView", () => {
  it("both tiers → Discard + Restore stable + Restore recent, with ages", () => {
    render(
      <AutosaveRecoveryView orphan={orphan()} nowMs={NOW} onChoose={vi.fn()} onDismiss={vi.fn()} />,
    );
    expect(screen.getByTestId("autosave-discard")).toBeInTheDocument();
    expect(screen.getByTestId("autosave-restore-stable")).toBeInTheDocument();
    expect(screen.getByTestId("autosave-restore-recent")).toBeInTheDocument();
    expect(screen.getByTestId("autosave-recent-age")).toHaveTextContent("45 seconds ago");
    expect(screen.getByTestId("autosave-stable-age")).toHaveTextContent("8 minutes ago");
    expect(screen.getByText("fire.alo")).toBeInTheDocument();
  });

  it("recent only → no Restore stable button", () => {
    render(
      <AutosaveRecoveryView
        orphan={orphan({ stableMtimeMs: null })}
        nowMs={NOW}
        onChoose={vi.fn()}
        onDismiss={vi.fn()}
      />,
    );
    expect(screen.getByTestId("autosave-restore-recent")).toBeInTheDocument();
    expect(screen.queryByTestId("autosave-restore-stable")).toBeNull();
  });

  it("stable only → no Restore recent button", () => {
    render(
      <AutosaveRecoveryView
        orphan={orphan({ recentMtimeMs: null })}
        nowMs={NOW}
        onChoose={vi.fn()}
        onDismiss={vi.fn()}
      />,
    );
    expect(screen.getByTestId("autosave-restore-stable")).toBeInTheDocument();
    expect(screen.queryByTestId("autosave-restore-recent")).toBeNull();
  });

  it("empty originalFilename → 'Unsaved new file'", () => {
    render(
      <AutosaveRecoveryView
        orphan={orphan({ originalFilename: "" })}
        nowMs={NOW}
        onChoose={vi.fn()}
        onDismiss={vi.fn()}
      />,
    );
    expect(screen.getByText("Unsaved new file")).toBeInTheDocument();
  });

  it("null orphan → dialog closed (no buttons)", () => {
    render(<AutosaveRecoveryView orphan={null} onChoose={vi.fn()} onDismiss={vi.fn()} />);
    expect(screen.queryByTestId("autosave-restore-recent")).toBeNull();
  });

  it("each button fires onChoose with its choice", () => {
    const onChoose = vi.fn();
    render(
      <AutosaveRecoveryView orphan={orphan()} nowMs={NOW} onChoose={onChoose} onDismiss={vi.fn()} />,
    );
    fireEvent.click(screen.getByTestId("autosave-restore-recent"));
    fireEvent.click(screen.getByTestId("autosave-restore-stable"));
    fireEvent.click(screen.getByTestId("autosave-discard"));
    expect(onChoose.mock.calls.map((c) => c[0])).toEqual(["recent", "stable", "discard"]);
  });
});

function makeBridge(checkResult: { orphan: AutosaveOrphan | null }) {
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "autosave/check-recovery") return Promise.resolve(checkResult);
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

describe("AutosaveRecoveryDialog (container)", () => {
  it("opens when check-recovery returns an orphan", async () => {
    const bridge = makeBridge({ orphan: orphan() });
    render(<AutosaveRecoveryDialog bridge={bridge} />);
    expect(await screen.findByTestId("autosave-restore-recent")).toBeInTheDocument();
  });

  it("stays closed when check-recovery returns no orphan", async () => {
    const bridge = makeBridge({ orphan: null });
    render(<AutosaveRecoveryDialog bridge={bridge} />);
    // Give the check-recovery promise a tick to resolve.
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
      expect(calls.find((c) => c.kind === "autosave/check-recovery")).toBeDefined();
    });
    expect(screen.queryByTestId("autosave-restore-recent")).toBeNull();
  });

  it("clicking Restore recent dispatches autosave/recover{recent}", async () => {
    const bridge = makeBridge({ orphan: orphan() });
    render(<AutosaveRecoveryDialog bridge={bridge} />);
    fireEvent.click(await screen.findByTestId("autosave-restore-recent"));
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const recover = calls.find((c) => c.kind === "autosave/recover");
    expect(recover).toBeDefined();
    expect(recover.params).toEqual({ choice: "recent" });
  });
});
