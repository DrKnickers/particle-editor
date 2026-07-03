// App smoke test — mounts the full editor shell (<App/>) against the
// MockBridge, the same way main.tsx bootstraps it (StrictMode +
// ErrorBoundary; makeBridge() returns MockBridge under jsdom because
// window.chrome.webview is absent).
//
// This is deliberately shallow: it proves the whole composition mounts
// without throwing under jsdom, the stable chrome roots exist (menubar,
// toolbar row, status bar, emitter-tree quadrant), and at least one
// startup seed consumed the mock snapshot (document title via
// useSeedFileState → formatWindowTitle). Deep behavior lives in the
// per-component suites.

// KNOWN MOCK ARTIFACT (verified against the native source): AppShell's
// startup pushes (engine/set/msaa-level, engine/set/model-shadows,
// engine/set/soft-shadows) are view-only preferences that the native
// host explicitly never marks dirty (BridgeDispatcher.cpp ~1949), but
// MockBridge's isMutating() blanket `engine/set/*` rule doesn't exclude
// them — so the app boots DIRTY under the mock. The title assertions
// below therefore tolerate the `● ` dirty prefix instead of asserting a
// clean boot. If the mock parity gap is fixed, tighten them.

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, waitFor } from "@testing-library/react";
import { StrictMode } from "react";
import { App } from "@/App";
import { ErrorBoundary } from "@/components/ErrorBoundary";
import { useFileStateStore } from "@/lib/file-state";
import { useMockEngineState, useMockRecentFiles } from "@/bridge/mock-state";

// Mount exactly like main.tsx does (minus createRoot — RTL owns the root).
function renderApp() {
  return render(
    <StrictMode>
      <ErrorBoundary>
        <App />
      </ErrorBoundary>
    </StrictMode>,
  );
}

beforeEach(() => {
  // The MockBridge's backing stores are module singletons — reset the
  // ones App's startup seeds read/write so renders stay independent.
  useMockEngineState.getState().reset();
  useMockRecentFiles.getState().reset();
  useFileStateStore.setState({
    currentFilePath: null,
    dirty: false,
    recentFiles: [],
    pendingAction: null,
  });
});

describe("App — smoke render against the MockBridge", () => {
  it("mounts the full shell without throwing and renders the stable chrome roots", async () => {
    renderApp();

    // The shell itself (not the ErrorBoundary fallback).
    expect(screen.getByTestId("app-shell")).toBeInTheDocument();
    expect(screen.queryByText(/something went wrong/i)).not.toBeInTheDocument();

    // Menubars — the app MenuBar plus the EmitterTree toolbar (both are
    // Radix Menubar roots, so query all and pin the app one via its
    // top-level File trigger).
    const menubars = await screen.findAllByRole("menubar");
    expect(menubars.length).toBeGreaterThanOrEqual(1);
    expect(screen.getByRole("menuitem", { name: "File" })).toBeInTheDocument();

    // StatusBar — the always-on rightmost spawn hint.
    expect(screen.getByText(/Shift: spawn instance/)).toBeInTheDocument();

    // Emitter-tree region: the PanelLayout quadrant plus the tree
    // component inside it (waits for the async tree seed).
    expect(await screen.findByTestId("quadrant-emitter-tree")).toBeInTheDocument();
    expect(await screen.findByTestId("emitter-tree")).toBeInTheDocument();

    // Viewport + curve-editor quadrants round out the layout contract
    // Playwright and Modal.tsx's portal lookup rely on.
    expect(screen.getByTestId("quadrant-viewport")).toBeInTheDocument();
    expect(screen.getByTestId("quadrant-curve-editor")).toBeInTheDocument();
  });

  it("seeds file state from the mock snapshot: path + recents land in the atom and the title", async () => {
    // Pre-populate the MockBridge's backing stores BEFORE mounting so
    // "seed consumed the snapshot" is distinguishable from the atom's own
    // defaults (which would produce the same Untitled title).
    useMockEngineState.getState().applyPatch({ currentFilePath: "C:/mods/fire.alo" });
    useMockRecentFiles.getState().setPaths(["C:/mods/fire.alo", "C:/mods/smoke.alo"]);

    renderApp();

    // useSeedFileState consumed engine/state/snapshot → atom → title effect.
    await waitFor(() => {
      expect(useFileStateStore.getState().currentFilePath).toBe("C:/mods/fire.alo");
    });
    await waitFor(() => {
      // Tolerates the mock's spurious boot-dirty `● ` prefix (see header note).
      expect(document.title).toMatch(/^(● )?fire\.alo — Particle Editor$/);
    });
    // file/recent/list seed landed too.
    await waitFor(() => {
      expect(useFileStateStore.getState().recentFiles).toEqual([
        "C:/mods/fire.alo",
        "C:/mods/smoke.alo",
      ]);
    });
  });

  it("renders the mock emitter tree's root emitters (tree seed consumed)", async () => {
    renderApp();

    // The MockBridge fixture tree has roots Smoke / Sparks / Flash.
    expect(await screen.findByText("Smoke")).toBeInTheDocument();
    expect(screen.getByText("Sparks")).toBeInTheDocument();
    expect(screen.getByText("Flash")).toBeInTheDocument();
  });
});
