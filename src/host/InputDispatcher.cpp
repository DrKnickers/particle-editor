// InputDispatcher implementation. See InputDispatcher.h for the
// design + lifecycle notes.

#include "InputDispatcher.h"

#include <cstdio>
#include <string>

namespace host {

namespace {

// Wrap an integer into a LPARAM as MAKELPARAM expects two 16-bit
// fields (x = LOWORD, y = HIWORD). Clamp into int16_t range so
// negative coords (cursor outside canvas during a captured drag)
// survive the round-trip via sign extension on the engine side
// (`(short)LOWORD(lp)` / `MAKEPOINTS(lp)`).
inline LPARAM PackXY(int x, int y) noexcept
{
    auto clamp = [](int v) -> WORD {
        if (v < INT16_MIN) v = INT16_MIN;
        else if (v > INT16_MAX) v = INT16_MAX;
        return static_cast<WORD>(static_cast<int16_t>(v));
    };
    return static_cast<LPARAM>(MAKELPARAM(clamp(x), clamp(y)));
}

} // namespace

InputDispatcher::InputDispatcher(HWND viewportPopup) noexcept
    : m_viewport(viewportPopup)
{
}

bool InputDispatcher::Dispatch(const nlohmann::json& params)
{
    if (!m_viewport) return false;
    if (!params.is_object()) return false;

    const std::string type = params.value("type", std::string{});
    if (type.empty()) return false;

    if (type == "mousemove")
    {
        int x = params.value("x", 0);
        int y = params.value("y", 0);
        int buttons = params.value("buttons", 0);
        PostMessageW(m_viewport, WM_MOUSEMOVE,
                     static_cast<WPARAM>(buttons), PackXY(x, y));
        return true;
    }

    if (type == "mousedown" || type == "mouseup")
    {
        int x = params.value("x", 0);
        int y = params.value("y", 0);
        int buttons = params.value("buttons", 0);
        const std::string button = params.value("button", std::string{"left"});
        UINT msg = WM_LBUTTONDOWN;
        const bool down = (type == "mousedown");
        if (button == "right")       msg = down ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        else if (button == "middle") msg = down ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        else                         msg = down ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        PostMessageW(m_viewport, msg,
                     static_cast<WPARAM>(buttons), PackXY(x, y));
        if (m_log)
        {
            char line[160];
            snprintf(line, sizeof(line),
                     "[ArchC-input] %s button=%s buttons=0x%x x=%d y=%d msg=0x%x",
                     type.c_str(), button.c_str(), buttons, x, y, msg);
            m_log(line);
        }
        return true;
    }

    if (type == "wheel")
    {
        int x = params.value("x", 0);
        int y = params.value("y", 0);
        int buttons = params.value("buttons", 0);
        int deltaY  = params.value("deltaY", 0);
        // wParam layout for WM_MOUSEWHEEL: HIWORD = wheel delta,
        // LOWORD = key/button MK_* bitmask. The engine handler at
        // HostWindow.cpp:1356 reads `(SHORT)HIWORD(wp) / WHEEL_DELTA`
        // for the notch count, so passing the renderer's quantised
        // (±WHEEL_DELTA per notch) delta directly is correct.
        WPARAM wp = MAKEWPARAM(static_cast<WORD>(buttons),
                               static_cast<WORD>(static_cast<int16_t>(deltaY)));
        // Win32 spec: lParam carries SCREEN coords for WM_MOUSEWHEEL.
        // The engine handler doesn't read lParam (only HIWORD(wp) is
        // used), so we pack popup-client coords for forward-compat
        // without a screen-coord round-trip via ClientToScreen.
        PostMessageW(m_viewport, WM_MOUSEWHEEL, wp, PackXY(x, y));
        return true;
    }

    if (type == "keydown")
    {
        int vk = params.value("vk", 0);
        bool repeat = params.value("repeat", false);
        // lParam: bit 0 (repeat count = 1), bit 30 (previous state = 1
        // if this is a repeat). The engine's WM_KEYDOWN guard at
        // HostWindow.cpp:1296 reads bit 30 and skips on repeat, so the
        // semantic is "first press" = bit 30 clear.
        LPARAM lp = repeat ? static_cast<LPARAM>(0x40000001)
                           : static_cast<LPARAM>(0x00000001);
        PostMessageW(m_viewport, WM_KEYDOWN, static_cast<WPARAM>(vk), lp);
        if (m_log)
        {
            char line[120];
            snprintf(line, sizeof(line),
                     "[ArchC-input] keydown vk=%d repeat=%d", vk, repeat ? 1 : 0);
            m_log(line);
        }
        return true;
    }

    if (type == "keyup")
    {
        int vk = params.value("vk", 0);
        // Standard WM_KEYUP lParam: bit 30 (previous state = 1) +
        // bit 31 (transition state = 1) + repeat count of 1.
        LPARAM lp = static_cast<LPARAM>(0xC0000001);
        PostMessageW(m_viewport, WM_KEYUP, static_cast<WPARAM>(vk), lp);
        if (m_log)
        {
            char line[120];
            snprintf(line, sizeof(line), "[ArchC-input] keyup vk=%d", vk);
            m_log(line);
        }
        return true;
    }

    if (type == "blur")
    {
        // window.blur → WM_KILLFOCUS. The engine's defensive cleanup
        // at HostWindow.cpp:1325 will kill any cursor-bound spawn.
        // wParam (gaining-focus HWND) is unused by the engine handler;
        // 0 is the canonical "no other window gets focus" value.
        PostMessageW(m_viewport, WM_KILLFOCUS, 0, 0);
        return true;
    }

    return false;  // unknown type — caller may log
}

} // namespace host
