import type { ReactElement } from "react";
import { useRecordHeadless } from "@/lib/record-mode";
import { APP_NAME, UNTITLED_DOC } from "@/lib/window-title";

export interface RecordTitleStripProps {
  currentFilePath: string | null;
  dirty: boolean;
}

// Same basename split window-title.ts uses (kept local — a 3-liner, not worth a shared export).
function basename(path: string): string {
  const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return idx >= 0 ? path.slice(idx + 1) : path;
}

/**
 * Branded title strip shown at the top of the app in HEADLESS `--record` mode
 * (Stage 2, "Option B"). CapturePreview grabs client content only, so the native
 * Win32 title bar never appears in a recorded clip — this in-frame strip supplies
 * the app mark, name, and `● filename` a viewer needs for context. Filename +
 * dirty mirror `document.title` (window-title.ts) so the two can't drift.
 *
 * Renders nothing outside headless record (returns null), so it's inert for
 * normal use and for the legacy PrintWindow record path (which still captures the
 * real title bar — a strip there would double it).
 */
export function RecordTitleStrip({ currentFilePath, dirty }: RecordTitleStripProps): ReactElement | null {
  const headless = useRecordHeadless();
  if (!headless) return null;
  const doc = currentFilePath ? basename(currentFilePath) : UNTITLED_DOC;
  return (
    <div
      data-testid="record-title-strip"
      className="flex h-[34px] shrink-0 items-center gap-2.5 border-b border-border bg-panel px-3 text-xs"
    >
      {/* The real app mark — verbatim from src/Resources/icon-src/mark.svg (the
          source the .ico/favicon are built from). Gradient id namespaced to avoid
          a collision if another #particle exists on the page. */}
      <svg width="20" height="20" viewBox="0 0 256 256" aria-hidden="true" className="shrink-0">
        <defs>
          <radialGradient id="rtsParticle" cx="42%" cy="36%" r="62%">
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
        <circle cx="132.42" cy="132.24" r="41.8" fill="url(#rtsParticle)" />
      </svg>
      <span className="font-medium text-text">{APP_NAME}</span>
      <span className="h-4 w-px shrink-0 bg-border" aria-hidden="true" />
      {dirty && (
        <span className="text-accent" aria-hidden="true">
          ●
        </span>
      )}
      <span className="truncate font-mono text-text-2">{doc}</span>
    </div>
  );
}
