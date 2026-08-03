#pragma once

// One-way liveness token shared between an owner and the asynchronous callbacks
// it hands to a runtime it does not control.
//
// WebView2's two CREATION callbacks — environment-completed and
// composition-controller-completed — are one-shot. Unlike the add_*/remove_*
// event handlers they cannot be unsubscribed, so HostWindow's WM_DESTROY sweep,
// which carefully removes every registered handler precisely because "the
// lambdas capture this", has no way to reach them. They were the two the
// teardown never accounted for (2026-07 audit, A-OWN-005).
//
// The window is real because WM_DESTROY does not stop the message pump.
// PostQuitMessage only POSTS WM_QUIT, and WM_QUIT is delivered once the queue is
// otherwise empty — so a completion the runtime already queued still dispatches
// after the HWND, the engine and the compositor are gone. The environment
// callback would then build a Compositor on a destroyed window; the controller
// callback would run the whole shared setup against a half-torn owner.
//
// The contract is deliberately ONE-WAY: once retired, always retired. A guard
// that could be revived would let a late callback resurrect an owner that is
// already partway through teardown, which is the bug rather than the fix.
//
// Owner-side use:
//     m_startupGuard.Retire();                       // first thing in WM_DESTROY
// Callback-side use:
//     auto guard = m_startupGuard.Issue();           // before dispatching
//     ... Callback<...>([this, guard](...) -> HRESULT {
//             if (!guard.OwnerAlive()) return S_OK;  // decline; `this` is gone
//             ...
//     A declining callback must not log or touch anything through `this` —
//     including the owner's own Log() — which is why it returns bare.

#include <memory>
#include <utility>

namespace host
{

class StartupCallbackGuard
{
public:
    // A token captured BY VALUE into an async callback. It shares the owner's
    // liveness flag and keeps that flag (not the owner) alive, so reading it
    // after the owner is destroyed is well-defined and answers false.
    class Token
    {
    public:
        explicit Token(std::shared_ptr<const bool> live) : m_live(std::move(live)) {}

        bool OwnerAlive() const { return m_live && *m_live; }

    private:
        std::shared_ptr<const bool> m_live;
    };

    StartupCallbackGuard() : m_live(std::make_shared<bool>(true)) {}

    // Exactly one owner. A copy could outlive the retire and hand out tokens
    // that answer true after teardown, which is the failure this exists to stop.
    StartupCallbackGuard(const StartupCallbackGuard&) = delete;
    StartupCallbackGuard& operator=(const StartupCallbackGuard&) = delete;

    ~StartupCallbackGuard() { Retire(); }

    Token Issue() const { return Token(m_live); }

    // Idempotent and one-way. Called from WM_DESTROY — teardown begins long
    // before the destructor runs, and the whole hazard lives in that gap — and
    // again from the destructor for any owner that never saw WM_DESTROY.
    void Retire() { if (m_live) *m_live = false; }

    bool Alive() const { return m_live && *m_live; }

private:
    std::shared_ptr<bool> m_live;
};

}   // namespace host
