import { useCallback, useEffect, useMemo, useState } from "react";
import * as Tooltip from "@radix-ui/react-tooltip";
import { makeBridge } from "@/bridge";
import { exposeBridgeForTests } from "@/bridge/expose";
import { PanelLayout, resetPanelLayoutStorage } from "@/components/PanelLayout";
import { StatusBar } from "@/components/StatusBar";
import { Toolbar } from "@/components/Toolbar";
import { MenuBar } from "@/components/MenuBar";
import { ImportEmittersDialog } from "@/screens/ImportEmittersDialog";
import { ModNicknameDialog } from "@/screens/ModNicknameDialog";
import { PrimitivesGallery } from "@/screens/PrimitivesGallery";
import { AboutDialog } from "@/screens/AboutDialog";
import { RescaleDialog } from "@/screens/RescaleDialog";
import { IncrementIndexDialog } from "@/screens/IncrementIndexDialog";
import { RescaleEmitterDialog } from "@/screens/RescaleEmitterDialog";
import { LinkGroupSettingsDialog } from "@/screens/LinkGroupSettingsDialog";
import { SetLinkGroupDialog } from "@/screens/SetLinkGroupDialog";
import { AutosaveRecoveryDialog, AutosaveRecoveryView } from "@/screens/AutosaveRecoveryDialog";
import { FileOpErrorModal } from "@/components/FileOpErrorModal";
import { DeleteConfirmModal } from "@/components/DeleteConfirmModal";
import { SaveChangesPrompt } from "@/screens/SaveChangesPrompt";
import { useFileState, useSeedFileState } from "@/lib/file-state";
import { useSeedModStack } from "@/lib/mod-stack";
import { formatWindowTitle } from "@/lib/window-title";
import { promptModNickname } from "@/lib/mod-nickname";
import { BridgeContext } from "@/lib/bridge-context";
import { useBackingColorSync } from "@/lib/backing-color-sync";
import { useAppAccelerators } from "@/lib/use-app-accelerators";
import { applyMode, readStoredMode } from "@/lib/theme";
import { applyOverloadGuard, readOverloadGuard } from "@/lib/overload-guard";
import { applyMsaaLevel, readMsaaLevel } from "@/lib/msaa-quality";
import { applySkydomeSeamFix, readSkydomeSeamFix } from "@/lib/skydome-seam-fix";
import { applyModelShadows, readModelShadows } from "@/lib/model-shadows";
import { applySoftShadows, readSoftShadows } from "@/lib/soft-shadows";

// ?demo=primitives → render the primitives gallery instead of the app shell.
// Evaluated once at module load; a page navigation to ?demo=primitives
// triggers a full reload so the const is re-evaluated correctly.
const DEMO_PARAM = new URLSearchParams(window.location.search).get("demo");

// AppShell — the full editor shell (MenuBar + Toolbar + Viewport + StatusBar).
// Split from App so the hooks are always called unconditionally inside a single
// component (no early-return-before-hook violations).
function AppShell() {
  const bridge = useMemo(() => {
    const b = makeBridge();
    // Task 2.2: attach to window.bridge so Playwright (via CDP) and
    // anyone poking at DevTools can drive the bridge. Diagnostic-only —
    // no production code path reads window.bridge.
    exposeBridgeForTests(b);
    return b;
  }, []);

  // (session 3): keep the host's DComp composition backing painted
  // the current theme `--bg` so transparent panel gaps / splitter seams /
  // rounded-corner wedges blend into the app shell instead of showing the
  // black host backing. Pushes on mount + on every theme change.
  useBackingColorSync(bridge);

  // B1.4: tool-panel + right-dock visibility now live inside
  // `PanelLayout`, which mounts the relevant child components directly.
  // The MenuBar drives the right-dock (Spawner / Lighting) via
  // `toggleDock` imported there, so AppShell no longer threads any
  // panel-open callbacks.
  const [aboutOpen, setAboutOpen] = useState(false);
  const [rescaleOpen, setRescaleOpen] = useState(false);
  const [importEmittersOpen, setImportEmittersOpen] = useState(false);

  // B1.4 T6: View → Reset panel layout. Clearing the localStorage keys
  // is necessary but not sufficient — the live PanelLayout still has
  // each Group's `defaultLayout` baked into its first-mount memo. The
  // epoch bump forces React to fully remount PanelLayout, and the new
  // mount's `loadLayout` calls then read the cleared keys and return
  // in-code defaults.
  const [panelLayoutEpoch, setPanelLayoutEpoch] = useState(0);
  const resetPanelLayout = useCallback(() => {
    resetPanelLayoutStorage();
    setPanelLayoutEpoch((n) => n + 1);
  }, []);

  // Task 2.6 (Phase 2): the bottom row is now an always-on
  // CurveEditorPanel that owns its own selection subscription, so the
  // app-shell no longer needs to track `selectedEmitterId` to gate the
  // lower-right pane. The previously-mounted EmitterPropertyPanel +
  // its snapshot/event wiring have been removed from this shell.

  // Particle Editor 2026 redesign: apply persisted theme (or OS preference)
  // before any panel renders so the first paint is correctly themed.
  // 3-way mode (dark/light/system): system follows prefers-color-scheme live.
  useEffect(() => {
    applyMode(readStoredMode());
    const mq = window.matchMedia("(prefers-color-scheme: dark)");
    const onChange = () => { if (readStoredMode() === "system") applyMode("system"); };
    mq.addEventListener("change", onChange);
    return () => mq.removeEventListener("change", onChange);
  }, []);

  // [guard-config] Push the persisted overload-guard config to the engine
  // once at startup — mirrors the theme apply-on-mount above. Without
  // this the engine would sit on its built-in defaults until the user
  // first opens Preferences.
  useEffect(() => {
    applyOverloadGuard(bridge, readOverloadGuard());
  }, [bridge]);

  // Push the persisted MSAA level to the engine once at startup so the
  // saved quality preference applies immediately without requiring the
  // user to open Preferences first.
  useEffect(() => {
    applyMsaaLevel(bridge, readMsaaLevel());
  }, [bridge]);

  // Push the persisted "Smooth skydome seams" preference to the engine at
  // startup so a re-mapped (or asset-accurate) dome loads without opening
  // Preferences first.
  useEffect(() => {
    applySkydomeSeamFix(bridge, readSkydomeSeamFix());
  }, [bridge]);

  // Push the persisted "Model shadows" preference to the engine at startup
  // so the saved shadow setting applies immediately without opening Preferences.
  useEffect(() => {
    applyModelShadows(bridge, readModelShadows());
  }, [bridge]);

  // Push the persisted "Soft shadows" preference to the engine at startup
  // so the saved shadow-blur setting applies immediately without opening Preferences.
  useEffect(() => {
    applySoftShadows(bridge, readSoftShadows());
  }, [bridge]);

  // Screen 8 Batch 3: subscribe to file-state events (dirty/changed,
  // recent/changed, engine/state/changed) and seed from snapshot +
  // file/recent/list on mount. Stays mounted for the app's lifetime.
  useSeedFileState(bridge);

  // Task 12: seed the mod-stack store from mods/list and subscribe to
  // engine/state/changed so the preview cache is invalidated on mod switches.
  useSeedModStack(bridge);

  // Window title — single source of truth for the titlebar. The host
  // mirrors document.title into the Win32 titlebar (DocumentTitleChanged
  // → SetWindowTextW in HostWindow.cpp), so this effect drives the OS
  // title, taskbar, and Alt-Tab text. Format cases live (tested) in
  // lib/window-title.ts.
  const { currentFilePath, dirty } = useFileState();
  useEffect(() => {
    document.title = formatWindowTitle(currentFilePath, dirty);
  }, [currentFilePath, dirty]);

  // / /: wire the legacy global keyboard accelerators to
  // the new UI's existing actions. The host (AcceleratorBridge) translates
  // the registered combos and emits `accelerator/pressed`; the hook routes
  // each to the same bridge call the matching menu item uses.
  useAppAccelerators(bridge);

  // Task 2.1 verification hook: log the initial engine snapshot at mount.
  // Confirms the bridge round-trip is producing a real EngineStateDto,
  // not the old `{ groundZ, background, skydomeSlot }` stub. Stays as a
  // permanent dev-mode breadcrumb — cheap, and useful any time the
  // bridge surface grows.
  useEffect(() => {
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => console.log("[engine/state/snapshot]", s))
      .catch((err) => console.warn("[engine/state/snapshot] failed:", err));
  }, [bridge]);

  return (
    <BridgeContext.Provider value={bridge}>
      {/* One app-level tooltip provider: first hover waits 400ms;
          moving between tooltipped controls within 300ms opens instantly
          (the "sweep the toolbar" feel native title can't give). Values are
          feel-tunable — adjust at the user smoke if flagged. */}
      <Tooltip.Provider delayDuration={400} skipDelayDuration={300}>
        <div data-testid="app-shell" className="flex h-full w-full flex-col text-text">
          {/* Top bar */}
          <header className="flex h-10 shrink-0 items-center gap-2 border-b border-border bg-bg px-4 text-sm">
            <MenuBar
              bridge={bridge}
              onOpenImportEmittersDialog={() => setImportEmittersOpen(true)}
              onOpenAboutDialog={() => setAboutOpen(true)}
              onOpenRescaleDialog={() => setRescaleOpen(true)}
              onResetPanelLayout={resetPanelLayout}
            />
          </header>

          {/* Toolbar — 4 groups (File · Edit · View · Render) */}
          <Toolbar bridge={bridge} />

          {/* Main row — B1.4: PanelLayout owns the three-column +
              two-inner-vertical-split structure with draggable separators
              via react-resizable-panels@4.x. Sizes persist per-user under
              alo:layout:{outer:{2col,3col},left,center}. The five
              quadrant-* data-testids live on inner divs inside PanelLayout
              (preserved exactly so Modal.tsx's querySelector portal
              lookup and Playwright specs continue to work). */}
          <PanelLayout key={panelLayoutEpoch} bridge={bridge} />

          {/* Status bar */}
          <StatusBar bridge={bridge} />

          {/* Sub-dialogs (Screen 8 batch 1). Mounted at app level so menu
              triggers from anywhere can drive them and Radix portals don't
              fight clipping from intermediate scrollable parents. */}
          <AboutDialog open={aboutOpen} onOpenChange={setAboutOpen} />
          <RescaleDialog
            bridge={bridge}
            open={rescaleOpen}
            onOpenChange={setRescaleOpen}
          />
          <ImportEmittersDialog
            bridge={bridge}
            open={importEmittersOpen}
            onOpenChange={setImportEmittersOpen}
          />
          {/* ModNicknameDialog is mounted unconditionally; it observes its
              own Zustand atom for open state. Driven by `promptModNickname`
              (programmatic) or the `?demo=mod-nickname` route in App below. */}
          <ModNicknameDialog />
          {/* Save-changes prompt (Screen 8 Batch 3). Open state lives in
              the file-state atom; this mount is invisible while the
              pendingAction slot is null. Driven from any destructive op
              handler via `promptSaveChanges(...)`. */}
          <SaveChangesPrompt bridge={bridge} />
          {/* Screen 4 Batch B1 — emitter-tree context-menu modals. They
              observe the `tree-context` Zustand atom for open state; the
              EmitterTree row's ContextMenu items poke the atom to mount
              whichever one was chosen. Rename moved to inline editing in
              Batch C — no modal involvement. */}
          <IncrementIndexDialog bridge={bridge} />
          <RescaleEmitterDialog bridge={bridge} />
          <LinkGroupSettingsDialog bridge={bridge} />
          {/* Screen 4 Batch B2 — multi-select link-group assignment. */}
          <SetLinkGroupDialog bridge={bridge} />
          {/* — crash-recovery. Checks for an orphaned autosave on mount;
              a no-op when the host reports none (always so under the mock). */}
          <AutosaveRecoveryDialog bridge={bridge} />
          <FileOpErrorModal />
          <DeleteConfirmModal bridge={bridge} />
        </div>
      </Tooltip.Provider>
    </BridgeContext.Provider>
  );
}

// ?demo=mod-nickname — design-checkpoint gate for the Mod Nickname
// dialog. Mounts the dialog and fires `promptModNickname()` once on
// load so the dialog is immediately visible. Phase 3 Screen 8 Batch 4.
function ModNicknameDemo() {
  useEffect(() => {
    void promptModNickname().then((result) => {
      console.log("[demo:mod-nickname] resolved with:", result);
    });
  }, []);
  return (
    <div className="flex h-full w-full items-center justify-center bg-bg text-sm text-text-2">
      <span>Mod Nickname dialog demo — dismiss to log the result.</span>
      <ModNicknameDialog />
    </div>
  );
}

// ?demo=autosave-recovery — a11y gate. Renders the recovery dialog
// with a FIXED both-tiers orphan and a FIXED `nowMs`, so the age text is
// deterministic for the composition a11y golden (the real check-recovery is
// suppressed under --test-host, so the dialog can't be driven by a real scan).
const DEMO_AUTOSAVE_NOW_MS = 1_700_000_000_000;
const DEMO_AUTOSAVE_ORPHAN = {
  originalFilename: "fire.alo",
  recentMtimeMs: DEMO_AUTOSAVE_NOW_MS - 45_000,       // "45 seconds ago"
  stableMtimeMs: DEMO_AUTOSAVE_NOW_MS - 8 * 60_000,   // "8 minutes ago"
};
function AutosaveRecoveryDemo() {
  return (
    <div className="flex h-full w-full items-center justify-center bg-bg text-sm text-text-2">
      <span>Autosave recovery dialog demo.</span>
      <AutosaveRecoveryView
        orphan={DEMO_AUTOSAVE_ORPHAN}
        nowMs={DEMO_AUTOSAVE_NOW_MS}
        onChoose={(c) => console.log("[demo:autosave-recovery] choice:", c)}
        onDismiss={() => console.log("[demo:autosave-recovery] dismissed")}
      />
    </div>
  );
}

// Log the React-side baked hosting mode on app load, matching
// the host's startup `[host] hosting mode: ...` log line. If the two
// log lines don't agree, the editor will look broken (viewport
// placeholder visible where engine pixels should be, or DXGI engine
// visible behind an empty <canvas>) — these two lines side-by-side
// are the fastest path to spotting build/runtime mismatch.
// Full mode-consistency banner deferred to a follow-up dispatch per
// R2 scope-trim — see tasks/handoff.md "Known follow-ups."
function logReactHostingMode() {
  const fromImportMeta = (import.meta as { env?: Record<string, unknown> }).env?.VITE_HOSTING_MODE;
  const fromProcess = typeof process !== "undefined" && process.env
    ? process.env.VITE_HOSTING_MODE
    : undefined;
  const legacy = fromImportMeta === "legacy" || fromProcess === "legacy";
  const mode = legacy ? "legacy (architecture A)" : "composition (architecture C, default)";
  // eslint-disable-next-line no-console
  console.log(`[mode] React build mode: ${mode}`);
}

// App — root entry point. Routes to the primitives gallery when ?demo=primitives
// is present; otherwise renders the full editor shell.
export function App() {
  // Fires once per App mount (typically once per page load).
  // Gated on first render via useEffect+[] deps so dev-time React.StrictMode
  // double-mount doesn't double-log.
  useEffect(() => {
    logReactHostingMode();
  }, []);

  // The demo routes bypass AppShell and therefore its app-level
  // Tooltip.Provider — but demo'd components (ColorButton, TexturePalette)
  // mount Tips, and Radix Tooltip.Root THROWS without a Provider (the
  // ?demo=primitives gallery white-screened in the native harness). Wrap
  // every demo return so standalone mounts stay viable.
  if (DEMO_PARAM === "primitives") {
    return (
      <Tooltip.Provider delayDuration={400} skipDelayDuration={300}>
        <PrimitivesGallery />
      </Tooltip.Provider>
    );
  }
  if (DEMO_PARAM === "mod-nickname") {
    return (
      <Tooltip.Provider delayDuration={400} skipDelayDuration={300}>
        <ModNicknameDemo />
      </Tooltip.Provider>
    );
  }
  if (DEMO_PARAM === "autosave-recovery") {
    return (
      <Tooltip.Provider delayDuration={400} skipDelayDuration={300}>
        <AutosaveRecoveryDemo />
      </Tooltip.Provider>
    );
  }
  return <AppShell />;
}
