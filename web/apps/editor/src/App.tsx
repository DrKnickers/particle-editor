import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { flushSync } from "react-dom";
import * as Tooltip from "@radix-ui/react-tooltip";
import { makeBridge } from "@/bridge";
import { signalAppReady } from "@/lib/app-ready";
import { emitClockCalibration, emitPerfTrace } from "@/lib/perf-trace";
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
import { parseCursorMessage, postFrameAcked, isRecordHeadlessMessage, commitAndAck } from "@/lib/record-cursor-bridge";
import { latchRecordModeFromMessage, markHeadless } from "@/lib/record-mode";
import { TitleBar } from "@/components/TitleBar";
import { evalRecordCursor } from "@/lib/record-cursor-eval";
import { applyRecordDrag, createRecordDragState, resetRecordDrag } from "@/lib/record-cursor-drag";
import { parseCursorTickMessage, parseCursorTrackMessage, type RecordCursorKey } from "@/lib/record-cursor-track";

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
    // Startup milestone spans for the perf lane (no-ops when tracing is off):
    // app.shell-mounted bracketed against app.first-interactive-paint measures the
    // critical startup window (perf-audit P1a).
    emitPerfTrace({ eventName: "app.shell-mounted", eventType: "instant" });
    let inner = 0;
    const outer = requestAnimationFrame(() => {
      inner = requestAnimationFrame(() => {
        emitClockCalibration("startup-ready");
        emitPerfTrace({ eventName: "app.first-interactive-paint", eventType: "instant" });
        signalAppReady();
      });
    });
    return () => {
      cancelAnimationFrame(outer);
      if (inner) cancelAnimationFrame(inner);
    };
  }, []);
  // (Removed the redundant startup engine/state/snapshot dev-log here — its data is
  // already fetched by the eager Toolbar/MenuBar/StatusBar consumers; the duplicate
  // was pure startup bridge fan-out, perf-audit P1a.)

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
  const recordCursorTrackRef = useRef<RecordCursorKey[] | null>(null);
  // --record HEADLESS mode (host sends ui/record-headless once): ack each frame
  // SYNCHRONOUSLY — flushSync the cursor/drag DOM commit, then post immediately,
  // dropping the double-rAF "proof of paint" wait (which stalls when the window
  // isn't presented / is minimized). The host's CapturePreview forces the paint,
  // so the rAF wait is redundant here. Legacy foreground record keeps the
  // double-rAF (so the golden-diff baseline is byte-for-byte unchanged).
  const recordHeadlessRef = useRef(false);
  // --record synthetic drag: turns the per-frame cursor press/move into REAL pointer
  // events so a clip can drive a live reorder gesture (lifted chip + make-room gap +
  // drop). See lib/record-cursor-drag.ts. Dormant outside --record (only the cursor
  // paths below touch it).
  const recordDragStateRef = useRef(createRecordDragState());
  useEffect(() => {
    const wv = window.chrome?.webview as
      | {
          addEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
          removeEventListener?: (e: string, h: (ev: { data: unknown }) => void) => void;
          postMessage?: (m: unknown) => void;
        }
      | undefined;
    if (!wv?.addEventListener) return;
    const onMsg = (e: { data: unknown }) => {
      // Latch record mode on the first record-cursor message (track or legacy
      // ui/cursor) — suppresses focus-pinned tooltips (Tip). One guarded wiring
      // point; a no-op for tick/other messages. See lib/record-mode.ts.
      latchRecordModeFromMessage(e.data);

      // Latch headless-capture mode (host → web, once, before the frame loop).
      if (isRecordHeadlessMessage(e.data)) {
        recordHeadlessRef.current = true;
        markHeadless(); // reactive latch → TitleBar hides its window controls
        return;
      }

      const track = parseCursorTrackMessage(e.data);
      if (track) {
        // A track swap mid-drag would strand a live synthetic gesture — abort it
        // (pointercancel, no commit) before the new track starts. See gap 1 /
        // invariant 2 in record-cursor-drag.ts.
        resetRecordDrag(recordDragStateRef.current);
        recordCursorTrackRef.current = track;
        return;
      }

      const tick = parseCursorTickMessage(e.data);
      if (tick) {
        const keys = recordCursorTrackRef.current;
        if (!keys) return;
        const cursor = evalRecordCursor(keys, tick.t);
        const ackMsg = JSON.stringify({
          type: "ui/frame-acked",
          frame: tick.frame,
          cursor: {
            x: cursor.x,
            y: cursor.y,
            vis: cursor.vis,
            press: cursor.press,
            resolved: cursor.resolved,
          },
        });
        // Apply the frame's cursor + a real drag from its press/move BEFORE the
        // ack, so the gesture's chip+gap are in the DOM by the time the host
        // grabs the frame. `ok` gates the down/move so an unresolved press never
        // clicks (0,0); point ("literal") targets are always ok, so they drive it
        // too (gaps 2 & 3).
        // Apply cursor + drag (headless flushSyncs BOTH so the drag chip/gap in
        // other components commit pre-ack), then ack. See commitAndAck.
        commitAndAck({
          headless: recordHeadlessRef.current,
          applyFrame: () => {
            setRecordCursor({ x: cursor.x, y: cursor.y, visible: cursor.vis, pressed: cursor.press });
            applyRecordDrag({ x: cursor.x, y: cursor.y, press: cursor.press, ok: cursor.ok }, recordDragStateRef.current);
          },
          post: () => wv.postMessage?.(ackMsg),
          flushSync,
        });
        return;
      }

      const c = parseCursorMessage(e.data);
      if (!c) return;
      // Legacy per-frame ("literal") cursor path drives the drag too — a literal
      // coordinate is always resolved (no ref to miss), so ok:true. Without this,
      // only the target-based track would reorder (gap 3). NOTE: this deprecated
      // protocol is always authored with real coords; parseCursorMessage coerces a
      // missing x/y to 0, so a hand-crafted literal message with pressed:true and no
      // coords would arm at (0,0) — an accepted latent gap for the legacy path only
      // (no dragging literal clip exists; new clips use the target track).
      const frame =
        typeof (e.data as { frame?: number })?.frame === "number" ? (e.data as { frame: number }).frame : 0;
      commitAndAck({
        headless: recordHeadlessRef.current,
        applyFrame: () => {
          setRecordCursor(c);
          applyRecordDrag({ x: c.x, y: c.y, press: c.pressed, ok: true }, recordDragStateRef.current);
        },
        post: () => postFrameAcked(frame),
        flushSync,
      });
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
          {/* The app's frameless custom title bar (replaces the native Win32
              caption). Always shown; window controls auto-hide in headless record. */}
          <TitleBar bridge={bridge} currentFilePath={currentFilePath} dirty={dirty} />
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
