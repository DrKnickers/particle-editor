// InputDispatcher — Phase 2 input forwarding.
//
// Receives `viewport/input` bridge requests from the renderer's DOM
// event handlers on the in-DOM <canvas>, decodes the discriminated
// `type` payload into a Win32 message, and PostMessages it to the
// (hidden) viewport popup HWND. The engine's existing viewport
// WNDPROC (HostWindow.cpp:1075-1371) consumes the synthetic messages
// unchanged — it reads modifiers exclusively from wParam MK_* bits
// and decodes coords from MAKEPOINTS(lParam), so the host-side
// reconstruction is mechanical.
//
// Constructed alongside the AlphaCompositor in WM_CREATE: the viewport
// popup is hidden, so all camera/keyboard input arrives via the DOM
// <canvas> and is forwarded here rather than routed directly by the OS.
//
// All operations are UI-thread only. PostMessage on a window owned by
// the calling thread queues the message on that thread's message
// queue; the popup WNDPROC drains it on the next pump iteration.
//
// Note on hidden HWND + SetFocus: WM_RBUTTONDOWN's handler calls
// SetFocus(hwnd) at HostWindow.cpp:1156. SetFocus on a hidden window
// fails silently and the handler doesn't check the return value, so
// this is accepted — no fix needed. Focus stays on the WebView
// throughout, which is what keyboard forwarding from the renderer
// requires.

#ifndef HOST_INPUT_DISPATCHER_H
#define HOST_INPUT_DISPATCHER_H

#include <functional>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "third_party/nlohmann/json.hpp"

namespace host {

class InputDispatcher
{
public:
    // Optional logger callback. HostWindow binds a lambda that fans
    // through Log() so diagnostic lines land
    // in %LOCALAPPDATA%\AloParticleEditor\host.log.
    using LogFn = std::function<void(const std::string& line)>;

    explicit InputDispatcher(HWND viewportPopup) noexcept;

    ~InputDispatcher() = default;
    InputDispatcher(const InputDispatcher&)            = delete;
    InputDispatcher& operator=(const InputDispatcher&) = delete;

    void SetLogger(LogFn log) { m_log = std::move(log); }

    // Decode `params` (a ViewportInputEvent — see bridge-schema) and
    // PostMessage the matching Win32 message to the popup HWND.
    // Malformed payloads are silently ignored; a return of `false`
    // means the payload didn't match any known event type.
    bool Dispatch(const nlohmann::json& params);

private:
    HWND  m_viewport;
    LogFn m_log;
};

} // namespace host

#endif // HOST_INPUT_DISPATCHER_H
