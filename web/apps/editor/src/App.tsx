import { useCallback, useEffect, useMemo, useState } from "react";
import * as Tooltip from "@radix-ui/react-tooltip";
import { makeBridge } from "@/bridge";
import { signalAppReady } from "@/lib/app-ready";
import { emitClockCalibration } from "@/lib/perf-trace";
import { exposeBridgeForTests } from "@/bridge/expose";
import { PanelLayout, resetPanelLayoutStorage } from "@/components/PanelLayout";
import { StatusBar } from "@/components/StatusBar";
import { Toolbar } from "@/components/Toolbar";
import { MenuBar } from "@/components/MenuBar";
import { ImportEmittersDialog } from "@/screens/ImportEmittersDialog";
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
import { useFileState, useSeedFileState, promptSaveChanges, useFileStateStore } from "@/lib/file-state";
import { useSeedModStack } from "@/lib/mod-stack";
import { formatWindowTitle } from "@/lib/window-title";
import { BridgeContext } from "@/lib/bridge-context";
import { useBackingColorSync } from "@/lib/backing-color-sync";
import { useAppAccelerators } from "@/lib/use-app-accelerators";
import { applyMode, readStoredMode } from "@/lib/theme";
import { applyOverloadGuard, readOverloadGuard } from "@/lib/overload-guard";
import { applyMsaaLevel, readMsaaLevel } from "@/lib/msaa-quality";
import { applyModelShadows, readModelShadows } from "@/lib/model-shadows";
import { applySoftShadows, readSoftShadows } from "@/lib/soft-shadows";
import { RecordCursor } from "@/components/RecordCursor";
import { parseCursorMessage, postFrameAcked } from "@/lib/record-cursor-bridge";

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
    // Attach to window.bridge so Playwright (via CDP) and
    // anyone poking at DevTools can drive the bridge. Diagnostic-only —
    // no production code path reads window.bridge.
    exposeBridgeForTests(b);
    return b;
  }, []);

  // keep the host's DComp composition backing painted
  // the current theme `--bg` so transparent panel gaps / splitter seams /
  // rounded-corner wedges blend into the app shell instead of showing the
  // black host backing. Pushes on mount + on every theme change.
  useBackingColorSync(bridge);

  // Tool-panel + right-dock visibility now live inside
  // `PanelLayout`, which mounts the relevant child components directly.
  // The MenuBar drives the right-dock (Spawner / Lighting) via
  // `toggleDock` imported there, so AppShell no longer threads any
  // panel-open callbacks.
  const [aboutOpen, setAboutOpen] = useState(false);
  const [rescaleOpen, setRescaleOpen] = useState(false);
  const [importEmittersOpen, setImportEmittersOpen] = useState(false);

  // View → Reset panel layout. Clearing the localStorage keys
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

  // The bottom row is now an always-on
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

  // Subscribe to file-state events (dirty/changed,
  // recent/changed, engine/state/changed) and seed from snapshot +
  // file/recent/list on mount. Stays mounted for the app's lifetime.
  useSeedFileState(bridge);

  // Seed the mod-stack store from mods/list and subscribe to
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

  // Wire the legacy global keyboard accelerators to
  // the new UI's existing actions. The host (AcceleratorBridge) translates
  // the registered combos and emits `accelerator/pressed`; the hook routes
  // each to the same bridge call the matching menu item uses.
  useAppAccelerators(bridge);

  // Signal the native host once the editor chrome has actually painted. Headless
  // --capture mode gates its composite-window screenshot on this so it captures
  // real chrome (menubar/toolbar/docks) instead of a blank React surface. A
  // double requestAnimationFrame deterministically puts us past at least one real
  // browser frame — useEffect fires after commit but its timing relative to the
  // host's screenshot isn't guaranteed, so we don't rely on it alone; the host
  // adds its own settle, making this the belt to its suspenders. No-op without WebView2.
  useEffect(() => {
    let inner = 0;
    const outer = requestAnimationFrame(() => {
      inner = requestAnimationFrame(() => {
        emitClockCalibration("startup-ready");
        signalAppReady();
      });
    });
    return () => {
      cancelAnimationFrame(outer);
      if (inner) cancelAnimationFrame(inner);
    };
  }, []);

  // Verification hook: log the initial engine snapshot at mount.
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

  // Data-loss BLOCKER: the native frame-X / Alt-F4 on a dirty doc
  // emits `app/close-requested` from the host (it can't render the React
  // prompt itself). Pop the SAME Save/Discard/Cancel prompt File→Exit uses; on
  // Save-success or Discard it dispatches app/quit, and the host tears down via
  // WM_APP_QUIT_CONFIRMED → DestroyWindow. Cancel leaves the window open.
  useEffect(() => {
    const off = bridge.on("app/close-requested", () => {
      // Idempotent: a second frame-X (or File→Exit then X) while the prompt is
      // already open must not churn the pending action. Cancel is React-only —
      // the host keeps no close-pending state, so this guard lives here.
      if (useFileStateStore.getState().pendingAction) return;
      promptSaveChanges(async () => {
        await bridge.request({ kind: "app/quit", params: {} }).catch(() => {});
      });
    });
    return off;
  }, [bridge]);

  // --record synthetic cursor: the host pushes ui/cursor over WebView2; mirror it
  // into a fixed overlay and ack each frame (double-rAF) so the host's PrintWindow
  // grab captures a committed composite. Raw listener — NativeBridge ignores a
  // ui/cursor message (type is neither res nor evt), so there's no conflict.
  // Entirely dormant outside --record.
  const [recordCursor, setRecordCursor] = useState({ x: 0, y: 0, visible: false, pressed: false });
  useEffect(() => {
    const wv = window.chrome?.webview as
      | {
          addEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
          removeEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
        }
      | undefined;
    if (!wv?.addEventListener) return;
    const onMsg = (e: { data: unknown }) => {
      const c = parseCursorMessage(e.data);
      if (!c) return;
      setRecordCursor(c);
      const frame =
        typeof (e.data as { frame?: number })?.frame === "number" ? (e.data as { frame: number }).frame : 0;
      requestAnimationFrame(() => requestAnimationFrame(() => postFrameAcked(frame)));
    };
    wv.addEventListener("message", onMsg);
    return () => wv.removeEventListener?.("message", onMsg);
  }, []);

  return (
    <BridgeContext.Provider value={bridge}>
      <RecordCursor
        x={recordCursor.x}
        y={recordCursor.y}
        visible={recordCursor.visible}
        pressed={recordCursor.pressed}
      />
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

          {/* Main row — PanelLayout owns the three-column +
              two-inner-vertical-split structure with draggable separators
              via react-resizable-panels@4.x. Sizes persist per-user under
              alo:layout:{outer:{2col,3col},left,center}. The five
              quadrant-* data-testids live on inner divs inside PanelLayout
              (preserved exactly so Modal.tsx's querySelector portal
              lookup and Playwright specs continue to work). */}
          <PanelLayout key={panelLayoutEpoch} bridge={bridge} />

          {/* Status bar */}
          <StatusBar bridge={bridge} />

          {/* Sub-dialogs. Mounted at app level so menu
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
          {/* Save-changes prompt. Open state lives in
              the file-state atom; this mount is invisible while the
              pendingAction slot is null. Driven from any destructive op
              handler via `promptSaveChanges(...)`. */}
          <SaveChangesPrompt bridge={bridge} />
          {/* Emitter-tree context-menu modals. They
              observe the `tree-context` Zustand atom for open state; the
              EmitterTree row's ContextMenu items poke the atom to mount
              whichever one was chosen. Rename moved to inline editing —
              no modal involvement. */}
          <IncrementIndexDialog bridge={bridge} />
          <RescaleEmitterDialog bridge={bridge} />
          <LinkGroupSettingsDialog bridge={bridge} />
          {/* Multi-select link-group assignment. */}
          <SetLinkGroupDialog bridge={bridge} />
          {/* Crash-recovery. Checks for an orphaned autosave on mount;
              a no-op when the host reports none (always so under the mock). */}
          <AutosaveRecoveryDialog bridge={bridge} />
          <FileOpErrorModal />
          <DeleteConfirmModal bridge={bridge} />
        </div>
      </Tooltip.Provider>
    </BridgeContext.Provider>
  );
}

// ?demo=autosave-recovery — crash-recovery a11y gate. Renders the recovery dialog
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

// App — root entry point. Routes to the primitives gallery when ?demo=primitives
// is present; otherwise renders the full editor shell.
export function App() {
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
  if (DEMO_PARAM === "autosave-recovery") {
    return (
      <Tooltip.Provider delayDuration={400} skipDelayDuration={300}>
        <AutosaveRecoveryDemo />
      </Tooltip.Provider>
    );
  }
  return <AppShell />;
}
