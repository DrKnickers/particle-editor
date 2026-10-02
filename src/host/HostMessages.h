#pragma once
#include <windows.h>

// Every private WM_APP window message the host posts, in one table so the
// values can't collide. HostWindow.cpp owns the wndproc; BridgeDispatcher.cpp
// and the preview worker post some of these too.
//
//   WM_APP + 1  WM_APP_COMPOSITION_FATAL
//   WM_APP + 2  WM_APP_QUIT_CONFIRMED
//   WM_APP + 3  WM_APP_VIEWPORT_BLUR
//   WM_APP + 4  WM_APP_PREVIEW_READY
//   WM_APP + 5  WM_APP_WEB_DEAD

// Posted when composition setup fails after the async
// CreateCoreWebView2CompositionController dispatch, or when a live Present1
// reports a restart-required DXGI device state. wParam carries the failure
// HRESULT. Composition is a hard requirement, so the handler surfaces a clear
// fatal error and exits (FailFatalComposition) -- there is no fallback path.
static const UINT WM_APP_COMPOSITION_FATAL = WM_APP + 1;

// Posted by the app/quit bridge handler AFTER the React Save/Discard/
// Cancel prompt has cleared. Its wndproc handler calls DestroyWindow (→WM_DESTROY,
// NOT WM_CLOSE), so a confirmed quit bypasses the dirty-close veto in WM_CLOSE.
static const UINT WM_APP_QUIT_CONFIRMED = WM_APP + 2;

// Posted (via InputDispatcher) on a genuine RENDERER viewport blur (window.blur:
// click-away, alt-tab from the browser). Distinct from the OS WM_KILLFOCUS the
// viewport receives from Win32 focus churn (which is deliberately suppressed to
// preserve a cursor-bound Shift spawn). The ViewportWndProc handler ENDS the
// cursor-bound spawn (and tears down an in-flight OBJECT_Z placement drag) so a
// real blur can't leak the attached preview.
static const UINT WM_APP_VIEWPORT_BLUR = WM_APP + 3;

// Posted by PreviewEncodeWorker once per finished background preview
// encode. The wndproc handler calls BridgeDispatcher::DrainPreviewResults,
// which caches the dataUri + emits `textures/preview-ready` so the web
// refetches (now a cache hit). Posted from the worker thread — PostMessage
// is the documented cross-thread hand-back.
static const UINT WM_APP_PREVIEW_READY = WM_APP + 4;

// Posted by the ProcessFailed handler when the web layer is declared dead
// (WebViewCrashPolicy.h). Like WM_APP_COMPOSITION_FATAL, the handler runs on
// the message loop, off the WebView2 callback stack, because an interactive
// session shows a modal and destroys the window from it.
static const UINT WM_APP_WEB_DEAD = WM_APP + 5;
