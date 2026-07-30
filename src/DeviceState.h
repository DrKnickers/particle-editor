#pragma once
#include <d3d9.h>

// D3D9Ex device-state classification (2026-07 audit, an-audit-finding).
//
// The engine creates its device with CreateDeviceEx, so m_pDevice is an
// IDirect3DDevice9Ex. Both recovery paths nevertheless asked
// IDirect3DDevice9::TestCooperativeLevel — which Microsoft documents as
// "always returns S_OK in Direct3D 9Ex applications"
// (IDirect3DDevice9Ex::CheckDeviceState remarks). Every D3DERR_DEVICELOST and
// D3DERR_DEVICENOTRESET branch behind it was therefore UNREACHABLE: a driver
// update, TDR/GPU reset, or remote-desktop transition produced no recovery
// attempt at all, and the D3D9Ex specs that claim to cover device-lost
// recovery could not have been exercising it.
//
// CheckDeviceState is the Ex replacement. Microsoft further recommends calling
// it only when a present FAILS. The direct-D3D9 path therefore uses the suspect
// latch. The production editor renders through a separate D3D11 compositor and
// has no D3D9 Present result; a failed cross-device event query raises the same
// next-frame suspect latch before any further composed rendering.
//
// Pure and header-only (an HRESULT in, an enum out, no device calls) so
// tests/test_device_state.cpp can cover every documented code without a D3D9
// device — the same reason src/host/RecordOutputSafety.h is shaped this way.

namespace devicestate {

enum class Action
{
    Render,       // healthy — draw this frame
    SkipFrame,    // transient: don't draw, retry on a later frame
    Reset,        // normal swap-chain/device reset, then draw
    RecoverHung,  // one full release -> ResetEx -> reacquire attempt
    Fatal,        // reset cannot repair it; the device must be recreated
};

// Classify a CheckDeviceState (or Present/PresentEx) result.
//
// The HUNG / REMOVED split is the load-bearing part. Microsoft permits an
// application that must continue after D3DERR_DEVICEHUNG to destroy every
// video-memory object and reset the device. D3DERR_DEVICEREMOVED instead
// requires device destruction, adapter enumeration, and recreation.
inline Action ClassifyDeviceState(HRESULT hr)
{
    switch (hr)
    {
        case D3D_OK:
            return Action::Render;

        // Another app owns the fullscreen display, or our window is covered.
        // Presenting is wasted work, but nothing is broken.
        case S_PRESENT_OCCLUDED:
            return Action::SkipFrame;

        // The desktop display mode changed under us; the swap chain must be
        // rebuilt before the next present.
        case S_PRESENT_MODE_CHANGED:
            return Action::Reset;

        // Transient loss — the cause (lock screen, fullscreen switch, sleep)
        // clears on its own and a later frame resets successfully.
        case D3DERR_DEVICELOST:
            return Action::SkipFrame;

        // Legacy code path: CheckDeviceState on an Ex device does not return
        // this, but Present can, and the pre-Ex contract meant "ready to be
        // reset now". Kept so the classifier is total over what a caller may
        // actually receive.
        case D3DERR_DEVICENOTRESET:
            return Action::Reset;

        case D3DERR_DEVICEHUNG:
            return Action::RecoverHung;

        case D3DERR_DEVICEREMOVED:
            return Action::Fatal;

        // Recoverable in principle: another frame may succeed once whatever
        // consumed the memory releases it. Not a reset candidate.
        case D3DERR_OUTOFVIDEOMEMORY:
            return Action::SkipFrame;

        default:
            // Unknown SUCCESS codes are healthy by definition; unknown FAILURE
            // codes must never be mistaken for healthy, so they cost a frame
            // rather than rendering against a device in an unmodelled state.
            return SUCCEEDED(hr) ? Action::Render : Action::SkipFrame;
    }
}

// True when a direct D3D9 presentation result warrants a D3D9Ex
// CheckDeviceState probe on the next frame. DXGI Present1 belongs to the
// separate D3D11 composition device and is classified by Compositor.h instead.
//
// S_FALSE is deliberately healthy here: Compositor::CompositeEngineFrame uses
// it for "no engine visual/current shared texture this frame." Treating that
// expected no-op as device loss would poll CheckDeviceState every frame while
// the visual is detached.
inline bool ShouldCheckDeviceAfterPresent(HRESULT hr)
{
    return FAILED(hr) || hr == S_PRESENT_OCCLUDED || hr == S_PRESENT_MODE_CHANGED;
}

// True when the state is one no amount of Reset() will clear.
inline bool IsFatalDeviceState(HRESULT hr)
{
    return ClassifyDeviceState(hr) == Action::Fatal;
}

} // namespace devicestate
