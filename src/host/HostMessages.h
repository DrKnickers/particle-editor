#pragma once
#include <windows.h>

// Private WM_APP window messages shared between HostWindow.cpp (the wndproc) and
// BridgeDispatcher.cpp (which posts them). They can't live in HostWindow.cpp's
// anonymous namespace because BridgeDispatcher.cpp must reference them too.
//
//   WM_APP + 1  WM_APP_COMPOSITION_FALLBACK  (defined locally in HostWindow.cpp)
//   WM_APP + 2  WM_APP_QUIT_CONFIRMED        (below)

// Posted by the app/quit bridge handler AFTER the React Save/Discard/
// Cancel prompt has cleared. Its wndproc handler calls DestroyWindow (→WM_DESTROY,
// NOT WM_CLOSE), so a confirmed quit bypasses the dirty-close veto in WM_CLOSE.
static const UINT WM_APP_QUIT_CONFIRMED = WM_APP + 2;
