// window-title.ts — pure formatter for the document/window title.
//
// The titlebar pipeline: this string is assigned to `document.title` by
// App.tsx's title effect; the host mirrors it into the Win32 titlebar
// via ICoreWebView2 DocumentTitleChanged → SetWindowTextW
// (src/host/HostWindow.cpp, FinishWebView2ControllerSetup). Keeping the
// format here — not in C++ — preserves a single source of truth and
// keeps it unit-testable.
//
// Format (spec 2026-06-11-open-file-titlebar-rebrand §3.2):
//   - Clean, named    : `foo.alo — Particle Editor`
//   - Dirty, named    : `● foo.alo — Particle Editor`
//   - Clean, untitled : `Untitled.alo — Particle Editor`
//   - Dirty, untitled : `● Untitled.alo — Particle Editor`

export const APP_NAME = "Particle Editor";
export const UNTITLED_DOC = "Untitled.alo";

export function formatWindowTitle(
  currentFilePath: string | null,
  dirty: boolean,
): string {
  const doc = currentFilePath ? basename(currentFilePath) : UNTITLED_DOC;
  return `${dirty ? "● " : ""}${doc} — ${APP_NAME}`;
}

// Same split MenuBar's Recent Files uses; duplicated 3-liner rather than
// exporting from a component module.
function basename(path: string): string {
  const idx = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return idx >= 0 ? path.slice(idx + 1) : path;
}
