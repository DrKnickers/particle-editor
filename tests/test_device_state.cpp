// D3D9Ex device-state classification (2026-07 audit, an-audit-finding).
//
// The defect this covers was not a wrong branch — it was an UNREACHABLE one.
// The engine creates its device with CreateDeviceEx, but both recovery paths
// asked IDirect3DDevice9::TestCooperativeLevel, which Microsoft documents as
// "always returns S_OK in Direct3D 9Ex applications". Every D3DERR_DEVICELOST /
// D3DERR_DEVICENOTRESET branch behind it was dead code, so a driver update,
// TDR/GPU reset, or RDP transition produced no recovery attempt at all.
//
// A real lost device cannot be induced from a unit test, and this box has no
// seam to force one. What CAN be pinned is the mapping — every documented
// CheckDeviceState return, including the two that must NOT be treated as
// recoverable — which is why src/DeviceState.h is a pure header.

#include "DeviceState.h"

#include <cstdio>

using devicestate::Action;
using devicestate::ClassifyDeviceState;
using devicestate::IsFatalDeviceState;

static int g_failures = 0;

static const char* ActionName(Action a)
{
    switch (a)
    {
        case Action::Render:    return "Render";
        case Action::SkipFrame: return "SkipFrame";
        case Action::Reset:     return "Reset";
        case Action::Fatal:     return "Fatal";
    }
    return "?";
}

static void Expect(const char* label, HRESULT hr, Action want)
{
    const Action got = ClassifyDeviceState(hr);
    if (got == want)
    {
        std::printf("  ok: %-22s 0x%08lx -> %s\n", label, (unsigned long)hr, ActionName(got));
        return;
    }
    std::printf("  FAIL: %-20s 0x%08lx -> %s (expected %s)\n",
                label, (unsigned long)hr, ActionName(got), ActionName(want));
    ++g_failures;
}

static void ExpectBool(const char* label, bool got, bool want)
{
    if (got == want) { std::printf("  ok: %s\n", label); return; }
    std::printf("  FAIL: %s (got %d, expected %d)\n", label, (int)got, (int)want);
    ++g_failures;
}

int main()
{
    std::printf("=== device-state classification ===\n");

    // Healthy.
    Expect("D3D_OK", D3D_OK, Action::Render);

    // Transient — the cause clears on its own; cost a frame and retry.
    Expect("DEVICELOST", D3DERR_DEVICELOST, Action::SkipFrame);
    // Occluded is a SUCCESS code. Treating it as healthy would keep presenting
    // into a covered window; treating it as a loss would be worse still —
    // an occluded window would look like a broken device.
    Expect("S_PRESENT_OCCLUDED", S_PRESENT_OCCLUDED, Action::SkipFrame);
    // Also recoverable-in-principle: another frame may succeed once whatever
    // consumed the memory releases it. Reset would not help.
    Expect("OUTOFVIDEOMEMORY", D3DERR_OUTOFVIDEOMEMORY, Action::SkipFrame);

    // Needs the swap chain rebuilt before the next present.
    Expect("S_PRESENT_MODE_CHANGED", S_PRESENT_MODE_CHANGED, Action::Reset);
    // Legacy code an Ex CheckDeviceState won't produce, but Present can.
    Expect("DEVICENOTRESET", D3DERR_DEVICENOTRESET, Action::Reset);

    // THE LOAD-BEARING SPLIT. A hung or removed adapter is not resettable:
    // classifying either as Reset would spin the render loop forever on a
    // recovery that cannot succeed, while looking like it was trying.
    Expect("DEVICEHUNG", D3DERR_DEVICEHUNG, Action::Fatal);
    Expect("DEVICEREMOVED", D3DERR_DEVICEREMOVED, Action::Fatal);

    // Totality: an unmodelled code must never be mistaken for healthy just
    // because we don't recognise it.
    Expect("unknown failure", E_FAIL, Action::SkipFrame);
    Expect("unknown success", S_FALSE, Action::Render);

    std::printf("=== fatal predicate ===\n");
    ExpectBool("DEVICEREMOVED is fatal", IsFatalDeviceState(D3DERR_DEVICEREMOVED), true);
    ExpectBool("DEVICEHUNG is fatal",    IsFatalDeviceState(D3DERR_DEVICEHUNG),    true);
    ExpectBool("DEVICELOST is NOT fatal (it recovers)",
               IsFatalDeviceState(D3DERR_DEVICELOST), false);
    ExpectBool("D3D_OK is NOT fatal", IsFatalDeviceState(D3D_OK), false);

    if (g_failures == 0)
    {
        std::printf("=== device state: ALL PASS ===\n");
        return 0;
    }
    std::printf("=== device state: %d FAILURE(S) ===\n", g_failures);
    return 1;
}
