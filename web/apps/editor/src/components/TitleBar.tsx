import { useEffect, useState, type CSSProperties, type ReactElement, type ReactNode } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { cn } from "@/lib/utils";
import { useRecordHeadless } from "@/lib/record-mode";
import { APP_NAME, UNTITLED_DOC } from "@/lib/window-title";

// WebView2 non-client region support: `app-region: drag` makes a region the
// window's draggable caption; `no-drag` opts back out (buttons, menus). Cast
// through `unknown` because it's not in React's CSSProperties.
const DRAG = { WebkitAppRegion: "drag" } as unknown as CSSProperties;
const NO_DRAG = { WebkitAppRegion: "no-drag" } as unknown as CSSProperties;

// Same basename split window-title.ts uses.
function basename(path: string): string {
  const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return idx >= 0 ? path.slice(idx + 1) : path;
}

function WinButton({
  label,
  close,
  onClick,
  children,
}: {
  label: string;
  close?: boolean;
  onClick: () => void;
  children: ReactNode;
}): ReactElement {
  return (
    <button
      type="button"
      aria-label={label}
      title={label}
      // Native window controls aren't in the app's tab order (keyboard users
      // reach them via Alt+Space → the system menu, which the HTCAPTION hit-test
      // provides). Keeps the app's tab cycle unchanged.
      tabIndex={-1}
      onClick={onClick}
      className={cn(
        "flex w-[46px] items-center justify-center text-text-2 transition-colors",
        // Theme-aware hover/pressed via the border tokens — clearly visible AND
        // correct-direction in both modes (lightens on the dark panel, darkens on
        // the light one; press goes one step further). Close keeps the universal
        // red with a darker pressed red.
        close
          ? "hover:bg-[#e81123] hover:text-white active:bg-[#c50f1f] active:text-white"
          : "hover:bg-border hover:text-text active:bg-border-2 active:text-text",
      )}
    >
      {children}
    </button>
  );
}

export interface TitleBarProps {
  bridge: Bridge;
  currentFilePath: string | null;
  dirty: boolean;
}

/**
 * The app's frameless custom title bar. Left: the real app mark + "Particle
 * Editor" + `● filename`. Right: native-style min / max-restore / close. The bar
 * (minus buttons) is the window's draggable caption (`app-region: drag`); the
 * buttons opt out (`no-drag`) and dispatch `window/*` host commands. Filename +
 * dirty mirror `document.title` (window-title.ts).
 *
 * In HEADLESS `--record` the interactive window controls are hidden (a close
 * button in a tutorial clip is noise) — the bar keeps mark + name + filename, so
 * a recorded clip is 1:1 with the live app minus the (non-clip) buttons.
 */
export function TitleBar({ bridge, currentFilePath, dirty }: TitleBarProps): ReactElement {
  const recording = useRecordHeadless();
  const [maximized, setMaximized] = useState(false);

  useEffect(() => bridge.on("window/state", (e) => setMaximized(e.payload.maximized)), [bridge]);

  const doc = currentFilePath ? basename(currentFilePath) : UNTITLED_DOC;
  const win = (kind: "window/minimize" | "window/maximize" | "window/close") =>
    void bridge.request({ kind, params: {} }).catch(() => {});

  return (
    <div
      data-testid="title-bar"
      className="flex h-[34px] shrink-0 select-none items-center gap-2.5 border-b border-border bg-panel pl-3 text-xs"
      style={DRAG}
    >
      {/* The real app mark — verbatim from src/Resources/icon-src/mark.svg. */}
      <svg width="20" height="20" viewBox="0 0 256 256" aria-hidden="true" className="shrink-0">
        <defs>
          <radialGradient id="tbParticle" cx="42%" cy="36%" r="62%">
            <stop offset="0" stopColor="#f4faff" />
            <stop offset="0.58" stopColor="#5ea1ff" />
            <stop offset="1" stopColor="#2f72e0" />
          </radialGradient>
        </defs>
        <path d="M49.98 161.68 C92.67 91.02 158.91 179.34 206.02 93.96" fill="none" stroke="#4691f4" strokeOpacity="0.3" strokeWidth="34.15" strokeLinecap="round" />
        <path d="M49.98 161.68 C92.67 91.02 158.91 179.34 206.02 93.96" fill="none" stroke="#7cb9ff" strokeWidth="18.25" strokeLinecap="round" />
        <path d="M126.34 133.34 C154.13 135.92 182.46 136.65 206.02 93.96" fill="none" stroke="#dcf0ff" strokeOpacity="0.61" strokeWidth="5.3" strokeLinecap="round" />
        <circle cx="49.98" cy="161.68" r="13.25" fill="#10223a" stroke="#83beff" strokeWidth="3" />
        <circle cx="206.02" cy="93.96" r="12.07" fill="#10223a" stroke="#83beff" strokeWidth="3" />
        <circle cx="132.42" cy="132.24" r="41.8" fill="url(#tbParticle)" />
      </svg>
      <span className="font-medium text-text">{APP_NAME}</span>
      <span className="h-4 w-px shrink-0 bg-border" aria-hidden="true" />
      {dirty && (
        <span className="text-accent" aria-hidden="true">
          ●
        </span>
      )}
      <span className="truncate text-text-2">{doc}</span>

      {/* Window controls — right-aligned, no-drag. Hidden in headless record. */}
      {!recording && (
        <div data-testid="window-controls" className="ml-auto flex h-full shrink-0 items-stretch" style={NO_DRAG}>
          <WinButton label="Minimize" onClick={() => win("window/minimize")}>
            <svg width="10" height="10" viewBox="0 0 10 10" aria-hidden="true">
              <line x1="0" y1="5" x2="10" y2="5" stroke="currentColor" strokeWidth="1" />
            </svg>
          </WinButton>
          <WinButton label={maximized ? "Restore" : "Maximize"} onClick={() => win("window/maximize")}>
            {maximized ? (
              <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden="true">
                <rect x="0.5" y="2.5" width="7" height="7" />
                <path d="M2.5 2.5 V0.5 H9.5 V7.5 H7.5" />
              </svg>
            ) : (
              <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1" aria-hidden="true">
                <rect x="0.5" y="0.5" width="9" height="9" />
              </svg>
            )}
          </WinButton>
          <WinButton label="Close" close onClick={() => win("window/close")}>
            <svg width="10" height="10" viewBox="0 0 10 10" stroke="currentColor" strokeWidth="1" aria-hidden="true">
              <line x1="0" y1="0" x2="10" y2="10" />
              <line x1="10" y1="0" x2="0" y2="10" />
            </svg>
          </WinButton>
        </div>
      )}
    </div>
  );
}
