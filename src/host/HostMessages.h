#pragma once
#include <windows.h>

// Private WM_APP window messages shared between HostWindow.cpp (the wndproc) and
// BridgeDispatcher.cpp (which posts them). They can't live in HostWindow.cpp's
// anonymous namespace because BridgeDispatcher.cpp must reference them too.
//
//   WM_APP + 1  WM_APP_COMPOSITION_FALLBACK  (defined locally in HostWindow.cpp)
//   WM_APP + 2  WM_APP_QUIT_CONFIRMED        (below)
//   WM_APP + 3  WM_APP_VIEWPORT_BLUR         (below)

// Posted by the app/quit bridge handler AFTER the React Save/Discard/
// Cancel prompt has cleared. Its wndproc handler calls DestroyWindow (→WM_DESTROY,
// NOT WM_CLOSE), so a confirmed quit bypasses the dirty-close veto in WM_CLOSE.
static const UINT WM_APP_QUIT_CONFIRMED = WM_APP + 2;

// Posted (via InputDispatcher) on a genuine RENDERER viewport blur (window.blur:
// click-away, alt-tab from the browser). Distinct from the OS WM_KILLFOCUS the
// viewport receives from Win32 focus churn (which is deliberately suppressed to
// preserve a cursor-bound Shift spawn). The ViewportWndProc handler ENDS the
// cursor-bound spawn (and tears down an in-flight OBJECT_Z placement drag) so a
// real blur can't leak the attached preview (release-audit #7).
static const UINT WM_APP_VIEWPORT_BLUR = WM_APP + 3;

// [C3] Posted by PreviewEncodeWorker once per finished background preview
// encode. The wndproc handler calls BridgeDispatcher::DrainPreviewResults,
// which caches the dataUri + emits `textures/preview-ready` so the web
// refetches (now a cache hit). Posted from the worker thread — PostMessage
// is the documented cross-thread hand-back.
static const UINT WM_APP_PREVIEW_READY = WM_APP + 4;
