// AcceleratorBridge — pre-translates accelerator key combos before
// WebView2 swallows them in its child HWND.
//
// React calls `register-accelerators` with a list of combo strings
// (e.g. ["Ctrl+S", "Ctrl+Z", "Ctrl+Shift+Z", "Delete", "F5"]).
// The host subscribes to ICoreWebView2Controller::AcceleratorKeyPressed;
// on match it sets Handled=TRUE and emits an `accelerator/pressed` event
// back to React so the React shortcut layer can react without the browser
// ever seeing the raw key.
#ifndef HOST_ACCELERATOR_BRIDGE_H
#define HOST_ACCELERATOR_BRIDGE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace host {

class AcceleratorBridge
{
public:
    AcceleratorBridge() = default;

    // Called by BridgeDispatcher when a `register-accelerators` Request
    // arrives. Replaces any previously registered combo list entirely.
    void RegisterCombos(const std::vector<std::string>& combos);

    // Called from the ICoreWebView2Controller::AcceleratorKeyPressed
    // event handler. Returns true if the (vk, ctrl, shift, alt) tuple
    // matched a registered combo and the EmitFn was invoked; the caller
    // should set Handled=TRUE in that case so WebView2 does not bubble
    // the key to the page.
    using EmitFn = std::function<void(const std::string& combo)>;
    bool TryDispatch(UINT vk, bool ctrl, bool shift, bool alt, EmitFn emit) const;

    // For diagnostics / logging.
    const std::vector<std::string>& Registered() const { return m_combos; }

private:
    // Human-readable originals kept for logging.
    std::vector<std::string> m_combos;

    // Pre-parsed representation used for matching.
    struct Parsed
    {
        UINT        vk;
        bool        ctrl;
        bool        shift;
        bool        alt;
        std::string combo; // original string, echoed in the event payload
    };
    std::vector<Parsed> m_parsed;

    static Parsed ParseCombo(const std::string& combo);
};

} // namespace host

#endif // HOST_ACCELERATOR_BRIDGE_H
