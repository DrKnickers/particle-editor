// D3D9Ex device-state classification (2026-07 audit).
//
// The defect this covers was not a wrong branch — it was an UNREACHABLE one.
// The engine creates its device with CreateDeviceEx, but both recovery paths
// asked IDirect3DDevice9::TestCooperativeLevel, which Microsoft documents as
// "always returns S_OK in Direct3D 9Ex applications". Every D3DERR_DEVICELOST /
// D3DERR_DEVICENOTRESET branch behind it was dead code, so a driver update,
// TDR/GPU reset, or RDP transition produced no recovery attempt at all.
//
// A real lost device cannot be induced from a unit test. DeviceRecovery.h's
// templated production port gives this test an executable fake-device seam for
// the exact CheckDeviceState / ResetEx door, while source bindings below prove
// both Engine callers and the host frame gate remain wired to it.

#include "DeviceState.h"
#include "DeviceRecovery.h"
#include "DeferredParticleSystemChange.h"
#include "host/Compositor.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using devicestate::Action;
using devicestate::ClassifyDeviceState;
using devicestate::IsFatalDeviceState;
using devicestate::ShouldCheckDeviceAfterPresent;
using host::ClassifyComposedFrameResult;
using host::ComposedFrameAction;
using host::ComposedFrameResult;

static int g_failures = 0;

static const char* ActionName(Action a)
{
    switch (a)
    {
        case Action::Render:    return "Render";
        case Action::SkipFrame: return "SkipFrame";
        case Action::Reset:     return "Reset";
        case Action::RecoverHung: return "RecoverHung";
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

static void ExpectInt(const char* label, int got, int want)
{
    if (got == want) { std::printf("  ok: %s\n", label); return; }
    std::printf("  FAIL: %s (got %d, expected %d)\n", label, got, want);
    ++g_failures;
}

static void ExpectTrace(const char* label,
                        const std::vector<std::string>& got,
                        std::initializer_list<const char*> want)
{
    bool equal = got.size() == want.size();
    size_t i = 0;
    for (const char* item : want)
    {
        if (i >= got.size() || got[i] != item) equal = false;
        ++i;
    }
    if (equal) { std::printf("  ok: %s\n", label); return; }
    std::printf("  FAIL: %s (got", label);
    for (const std::string& item : got) std::printf(" %s", item.c_str());
    std::printf(")\n");
    ++g_failures;
}

struct FakeRecoveryDevice
{
    HRESULT checkResult  = D3D_OK;
    HRESULT legacyResult = D3D_OK;
    HRESULT resetResult  = D3D_OK;
    int checks = 0;
    int legacyChecks = 0;
    int resets = 0;
    bool sawZeroExtentInput = false;
    bool zeroedExtentOutput = false;
    std::function<void()> duringReset;
    std::vector<std::string>* trace = nullptr;

    HRESULT CheckDeviceState(HWND)
    {
        ++checks;
        if (trace) trace->push_back("check");
        return checkResult;
    }

    // Deliberately present with a different result so the exact
    // CheckDeviceState -> TestCooperativeLevel mutant is observable.
    HRESULT TestCooperativeLevel()
    {
        ++legacyChecks;
        if (trace) trace->push_back("legacy");
        return legacyResult;
    }

    HRESULT ResetEx(D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX*)
    {
        ++resets;
        if (trace) trace->push_back("reset");
        sawZeroExtentInput =
            parameters != nullptr &&
            parameters->BackBufferWidth == 0 &&
            parameters->BackBufferHeight == 0 &&
            parameters->BackBufferCount == 1;
        if (duringReset)
        {
            std::function<void()> callback = std::move(duringReset);
            callback();
        }
        if (parameters)
        {
            // Microsoft documents these in/out fields as zero on return.
            parameters->BackBufferWidth = 0;
            parameters->BackBufferHeight = 0;
            parameters->BackBufferCount = 0;
            zeroedExtentOutput = true;
        }
        return resetResult;
    }
};

struct FakeRecoveryOwner
{
    bool onDeviceThread = true;
    int releases = 0;
    int effectResets = 0;
    int extentRefreshes = 0;
    int reacquires = 0;
    HRESULT refreshResult = D3D_OK;
    D3DPRESENT_PARAMETERS parameters = {};
    std::vector<std::string>* trace = nullptr;

    bool IsDeviceRecoveryThread() const { return onDeviceThread; }
    D3DPRESENT_PARAMETERS GetDeviceRecoveryPresentationParameters() const
    {
        D3DPRESENT_PARAMETERS resetParameters = parameters;
        resetParameters.BackBufferWidth = 0;
        resetParameters.BackBufferHeight = 0;
        resetParameters.BackBufferCount = 1;
        resetParameters.Windowed = TRUE;
        return resetParameters;
    }
    void ReleaseDeviceResourcesForReset()
    {
        ++releases;
        if (trace) trace->push_back("release");
    }
    void ResetDeviceEffectsAfterReset()
    {
        ++effectResets;
        if (trace) trace->push_back("effects");
    }
    HRESULT RefreshPresentationParametersAfterReset()
    {
        ++extentRefreshes;
        if (trace) trace->push_back("dimensions");
        if (FAILED(refreshResult)) return refreshResult;
        parameters.BackBufferWidth = 1280;
        parameters.BackBufferHeight = 720;
        parameters.BackBufferCount = 1;
        return D3D_OK;
    }
    void ReacquireDeviceResourcesAfterReset()
    {
        ++reacquires;
        if (trace) trace->push_back("reacquire");
    }
};

static devicerecovery::Result RunRecovery(
    devicerecovery::State& state,
    FakeRecoveryDevice& device,
    FakeRecoveryOwner& owner,
    bool renderWhenOccluded = false)
{
    devicerecovery::D3D9ExRecoveryPort<FakeRecoveryDevice, FakeRecoveryOwner>
        port(&device, owner);
    return devicerecovery::RunDeviceRecoveryStep(
        state, port, renderWhenOccluded);
}

static std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    const std::istreambuf_iterator<char> end;
    std::string source(std::istreambuf_iterator<char>(input), end);
    source.erase(std::remove(source.begin(), source.end(), '\r'),
                 source.end());
    return source;
}

static bool Contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

static size_t CountOccurrences(const std::string& text, const char* needle)
{
    size_t count = 0;
    size_t position = 0;
    const size_t needleLength = std::char_traits<char>::length(needle);
    while (needleLength > 0 &&
           (position = text.find(needle, position)) != std::string::npos)
    {
        ++count;
        position += needleLength;
    }
    return count;
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

    // THE LOAD-BEARING SPLIT. HUNG gets the bounded full-resource recovery
    // coordinator; REMOVED still requires device recreation.
    Expect("DEVICEHUNG", D3DERR_DEVICEHUNG, Action::RecoverHung);
    Expect("DEVICEREMOVED", D3DERR_DEVICEREMOVED, Action::Fatal);

    // Totality: an unmodelled code must never be mistaken for healthy just
    // because we don't recognise it.
    Expect("unknown failure", E_FAIL, Action::SkipFrame);
    Expect("unknown success", S_FALSE, Action::Render);

    std::printf("=== fatal predicate ===\n");
    ExpectBool("DEVICEREMOVED is fatal", IsFatalDeviceState(D3DERR_DEVICEREMOVED), true);
    ExpectBool("DEVICEHUNG is recoverable once",
               IsFatalDeviceState(D3DERR_DEVICEHUNG), false);
    ExpectBool("DEVICELOST is NOT fatal (it recovers)",
               IsFatalDeviceState(D3DERR_DEVICELOST), false);
    ExpectBool("D3D_OK is NOT fatal", IsFatalDeviceState(D3D_OK), false);

    std::printf("=== direct D3D9 present-result suspect predicate ===\n");
    ExpectBool("failed direct Present requests a device probe",
               ShouldCheckDeviceAfterPresent(E_FAIL), true);
    ExpectBool("direct Present occlusion requests a device probe",
               ShouldCheckDeviceAfterPresent(S_PRESENT_OCCLUDED), true);
    ExpectBool("direct Present mode change requests a device probe",
               ShouldCheckDeviceAfterPresent(S_PRESENT_MODE_CHANGED), true);
    ExpectBool("S_FALSE no-visual frame does NOT request a device probe",
               ShouldCheckDeviceAfterPresent(S_FALSE), false);
    ExpectBool("D3D_OK does NOT request a device probe",
               ShouldCheckDeviceAfterPresent(D3D_OK), false);

    std::printf("=== composed D3D11 frame-result policy ===\n");
    ExpectBool("Present1 DEVICE_REMOVED requires restart",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::PresentResult(
                       DXGI_ERROR_DEVICE_REMOVED)) ==
                   ComposedFrameAction::RestartRequired,
               true);
    ExpectBool("Present1 DEVICE_HUNG requires restart",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::PresentResult(
                       DXGI_ERROR_DEVICE_HUNG)) ==
                   ComposedFrameAction::RestartRequired,
               true);
    ExpectBool("Present1 DEVICE_RESET requires restart",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::PresentResult(
                       DXGI_ERROR_DEVICE_RESET)) ==
                   ComposedFrameAction::RestartRequired,
               true);
    ExpectBool("Present1 S_OK continues",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::PresentResult(S_OK)) ==
                   ComposedFrameAction::Continue,
               true);
    ExpectBool("Present1 S_FALSE continues",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::PresentResult(S_FALSE)) ==
                   ComposedFrameAction::Continue,
               true);
    ExpectBool("Present1 DXGI_STATUS_OCCLUDED continues",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::PresentResult(
                       DXGI_STATUS_OCCLUDED)) ==
                   ComposedFrameAction::Continue,
               true);
    ExpectBool("no-frame S_FALSE continues",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::NoFrame()) ==
                   ComposedFrameAction::Continue,
               true);
    // Load-bearing overreach guard: the exact fatal numeric value is safe to
    // retry when it came from reopening a stale shared handle rather than
    // Present1 on the D3D11 device.
    ExpectBool("shared-handle DEVICE_REMOVED remains retryable",
               ClassifyComposedFrameResult(
                   ComposedFrameResult::SharedHandleFailure(
                       DXGI_ERROR_DEVICE_REMOVED)) ==
                   ComposedFrameAction::Retry,
               true);

    std::printf("=== executable production recovery door ===\n");
    {
        FakeRecoveryDevice directDevice;
        FakeRecoveryOwner directOwner;
        devicerecovery::State directRecovery;
        directDevice.checkResult = S_PRESENT_OCCLUDED;
        const devicerecovery::Result direct =
            RunRecovery(directRecovery, directDevice, directOwner);

        FakeRecoveryDevice composedDevice;
        FakeRecoveryOwner composedOwner;
        devicerecovery::State composedRecovery;
        composedDevice.checkResult = S_PRESENT_OCCLUDED;
        const devicerecovery::Result composed =
            RunRecovery(
                composedRecovery, composedDevice, composedOwner, true);

        ExpectBool("direct recovery skips the exact occluded state",
                   direct.outcome == devicerecovery::Outcome::SkipFrame,
                   true);
        ExpectBool("composed recovery renders the exact occluded state",
                   composed.outcome == devicerecovery::Outcome::Render,
                   true);
        ExpectInt("direct occlusion probes exactly once",
                  directDevice.checks, 1);
        ExpectInt("composed occlusion probes exactly once",
                  composedDevice.checks, 1);
    }
    {
        std::vector<std::string> trace;
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.trace = &trace;
        owner.trace = &trace;
        owner.parameters.BackBufferWidth = 640;
        owner.parameters.BackBufferHeight = 480;
        owner.parameters.BackBufferCount = 1;
        device.checkResult = D3DERR_DEVICEHUNG;
        device.legacyResult = D3D_OK;
        device.resetResult = D3D_OK;

        const devicerecovery::Result result =
            RunRecovery(recovery, device, owner);
        ExpectBool("HUNG recovery returns Render",
                   result.outcome == devicerecovery::Outcome::Render, true);
        ExpectBool("HUNG recovery records one attempted recovery",
                   result.attemptedRecovery, true);
        ExpectInt("real CheckDeviceState door called once", device.checks, 1);
        ExpectInt("legacy TestCooperativeLevel door never called",
                  device.legacyChecks, 0);
        ExpectInt("HUNG releases every owner once", owner.releases, 1);
        ExpectInt("HUNG calls ResetEx once", device.resets, 1);
        ExpectBool("ResetEx receives zero client-sized extents",
                   device.sawZeroExtentInput, true);
        ExpectBool("fake ResetEx models zeroed output extents",
                   device.zeroedExtentOutput, true);
        ExpectInt("successful ResetEx resets effects once",
                  owner.effectResets, 1);
        ExpectInt("successful ResetEx refreshes extents once",
                  owner.extentRefreshes, 1);
        ExpectInt("refreshed back-buffer width is specific",
                  static_cast<int>(owner.parameters.BackBufferWidth), 1280);
        ExpectInt("refreshed back-buffer height is specific",
                  static_cast<int>(owner.parameters.BackBufferHeight), 720);
        ExpectInt("successful ResetEx reacquires once", owner.reacquires, 1);
        ExpectTrace("HUNG ordering includes effects and extent refresh",
                    trace, {"check", "release", "reset", "effects",
                            "dimensions", "reacquire"});

        // One attempt per device lifetime: a later HUNG is terminal and must
        // not release/reset/reacquire a second time.
        trace.clear();
        const devicerecovery::Result repeated =
            RunRecovery(recovery, device, owner);
        ExpectBool("repeat HUNG after recovery is terminal",
                   repeated.outcome == devicerecovery::Outcome::Fatal, true);
        ExpectInt("repeat HUNG checks state once", device.checks, 2);
        ExpectInt("repeat HUNG does not release again", owner.releases, 1);
        ExpectInt("repeat HUNG does not ResetEx again", device.resets, 1);
        ExpectInt("repeat HUNG does not reset effects again",
                  owner.effectResets, 1);
        ExpectInt("repeat HUNG does not refresh extents again",
                  owner.extentRefreshes, 1);
        ExpectInt("repeat HUNG does not reacquire again", owner.reacquires, 1);
        ExpectTrace("repeat HUNG performs only the state probe",
                    trace, {"check"});
    }
    {
        std::vector<std::string> trace;
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.trace = &trace;
        owner.trace = &trace;
        device.checkResult = D3DERR_DEVICEREMOVED;
        device.resetResult = D3D_OK; // would expose an overreaching reset

        const devicerecovery::Result result =
            RunRecovery(recovery, device, owner);
        ExpectBool("REMOVED remains fatal",
                   result.outcome == devicerecovery::Outcome::Fatal, true);
        ExpectInt("REMOVED never releases for reset", owner.releases, 0);
        ExpectInt("REMOVED never calls ResetEx", device.resets, 0);
        ExpectInt("REMOVED never resets effects", owner.effectResets, 0);
        ExpectInt("REMOVED never refreshes extents", owner.extentRefreshes, 0);
        ExpectInt("REMOVED never reacquires", owner.reacquires, 0);
        ExpectTrace("REMOVED performs only CheckDeviceState", trace, {"check"});
    }
    {
        std::vector<std::string> trace;
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.trace = &trace;
        owner.trace = &trace;
        device.checkResult = D3DERR_DEVICEHUNG;
        device.resetResult = D3DERR_DEVICELOST;

        const devicerecovery::Result failed =
            RunRecovery(recovery, device, owner);
        ExpectBool("failed HUNG ResetEx is terminal",
                   failed.outcome == devicerecovery::Outcome::Fatal, true);
        ExpectInt("failed HUNG releases once", owner.releases, 1);
        ExpectInt("failed HUNG calls ResetEx once", device.resets, 1);
        ExpectInt("failed HUNG never resets effects", owner.effectResets, 0);
        ExpectInt("failed HUNG never refreshes extents", owner.extentRefreshes, 0);
        ExpectInt("failed HUNG never reacquires", owner.reacquires, 0);
        ExpectTrace("failed HUNG stops after ResetEx",
                    trace, {"check", "release", "reset"});

        trace.clear();
        const devicerecovery::Result second =
            RunRecovery(recovery, device, owner);
        ExpectBool("terminal second step stays fatal",
                   second.outcome == devicerecovery::Outcome::Fatal, true);
        ExpectInt("terminal second step makes no state call", device.checks, 1);
        ExpectInt("terminal second step makes no release", owner.releases, 1);
        ExpectInt("terminal second step makes no ResetEx", device.resets, 1);
        ExpectInt("terminal second step makes no effect reset",
                  owner.effectResets, 0);
        ExpectInt("terminal second step makes no extent refresh",
                  owner.extentRefreshes, 0);
        ExpectInt("terminal second step makes no reacquire", owner.reacquires, 0);
        ExpectTrace("terminal second step makes no device calls", trace, {});
    }
    {
        std::vector<std::string> trace;
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.trace = &trace;
        owner.trace = &trace;
        device.checkResult = D3DERR_DEVICEHUNG;
        device.resetResult = D3D_OK;
        owner.refreshResult = E_FAIL;

        const devicerecovery::Result failed =
            RunRecovery(recovery, device, owner);
        ExpectBool("extent refresh failure is terminal",
                   failed.outcome == devicerecovery::Outcome::Fatal, true);
        ExpectInt("extent refresh failure resets effects first",
                  owner.effectResets, 1);
        ExpectInt("extent refresh failure attempts one refresh",
                  owner.extentRefreshes, 1);
        ExpectInt("extent refresh failure never allocates resources",
                  owner.reacquires, 0);
        ExpectTrace("extent failure stops before resource reacquire",
                    trace, {"check", "release", "reset", "effects",
                            "dimensions"});
    }
    {
        std::vector<std::string> trace;
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.trace = &trace;
        owner.trace = &trace;
        device.checkResult = D3DERR_DEVICEHUNG;
        device.resetResult = D3D_OK;
        bool nestedRan = false;
        devicerecovery::Outcome nestedOutcome =
            devicerecovery::Outcome::Fatal;
        HRESULT nestedRecoveryResult = D3D_OK;
        device.duringReset = [&]()
        {
            nestedRan = true;
            const devicerecovery::Result nested =
                RunRecovery(recovery, device, owner);
            nestedOutcome = nested.outcome;
            nestedRecoveryResult = nested.recoveryResult;
        };

        const devicerecovery::Result outer =
            RunRecovery(recovery, device, owner);
        ExpectBool("ResetEx reentrant coordinator call ran", nestedRan, true);
        ExpectBool("reentrant call skips without corrupting outer state",
                   nestedOutcome == devicerecovery::Outcome::SkipFrame, true);
        ExpectBool("reentrant call reports E_PENDING",
                   nestedRecoveryResult == E_PENDING, true);
        ExpectBool("outer recovery still succeeds",
                   outer.outcome == devicerecovery::Outcome::Render, true);
        ExpectInt("reentrancy does not issue a second CheckDeviceState",
                  device.checks, 1);
        ExpectInt("reentrancy does not issue a second release",
                  owner.releases, 1);
        ExpectInt("reentrancy does not issue a second ResetEx",
                  device.resets, 1);
        ExpectInt("reentrancy still reacquires exactly once",
                  owner.reacquires, 1);
        ExpectTrace("reentrant recovery preserves one outer transaction",
                    trace, {"check", "release", "reset", "effects",
                            "dimensions", "reacquire"});
    }
    {
        std::vector<std::string> trace;
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.trace = &trace;
        owner.trace = &trace;
        devicerecovery::RecordResetExFailure(
            recovery, D3DERR_DEVICELOST);

        device.checkResult = D3DERR_DEVICELOST;
        const devicerecovery::Result lost =
            RunRecovery(recovery, device, owner);
        ExpectBool("failed resize ResetEx stays pending while LOST",
                   lost.outcome == devicerecovery::Outcome::SkipFrame, true);
        ExpectInt("pending LOST does not release owners", owner.releases, 0);
        ExpectInt("pending LOST does not call ResetEx", device.resets, 0);

        trace.clear();
        device.checkResult = D3D_OK;
        const devicerecovery::Result retry =
            RunRecovery(recovery, device, owner);
        ExpectBool("healthy probe requests ResetEx retry, never ordinary Reset",
                   retry.outcome == devicerecovery::Outcome::RetryResetEx,
                   true);
        ExpectInt("retry decision itself does not release", owner.releases, 0);
        ExpectInt("retry decision itself does not call ResetEx", device.resets, 0);
        ExpectTrace("pending retry performs only the allowed state probe",
                    trace, {"check"});

        devicerecovery::CompleteResetExRetry(recovery);
        trace.clear();
        const devicerecovery::Result healthy =
            RunRecovery(recovery, device, owner);
        ExpectBool("completed ResetEx retry reopens rendering",
                   healthy.outcome == devicerecovery::Outcome::Render, true);
        ExpectTrace("completed retry returns to normal state probing",
                    trace, {"check"});
    }
    {
        std::vector<std::string> trace;
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.trace = &trace;
        owner.trace = &trace;
        device.checkResult = D3DERR_DEVICEHUNG;
        owner.onDeviceThread = false;

        const devicerecovery::Result deferred =
            RunRecovery(recovery, device, owner);
        ExpectBool("off-thread HUNG defers",
                   deferred.outcome == devicerecovery::Outcome::SkipFrame, true);
        ExpectInt("off-thread HUNG does not release", owner.releases, 0);
        ExpectInt("off-thread HUNG does not ResetEx", device.resets, 0);
        ExpectInt("off-thread HUNG does not reset effects", owner.effectResets, 0);
        ExpectInt("off-thread HUNG does not refresh extents",
                  owner.extentRefreshes, 0);
        ExpectInt("off-thread HUNG does not reacquire", owner.reacquires, 0);

        owner.onDeviceThread = true;
        trace.clear();
        const devicerecovery::Result recovered =
            RunRecovery(recovery, device, owner);
        ExpectBool("render-thread retry recovers",
                   recovered.outcome == devicerecovery::Outcome::Render, true);
        ExpectTrace("render-thread retry performs the full ordered attempt",
                    trace, {"check", "release", "reset", "effects",
                            "dimensions", "reacquire"});
    }
    {
        FakeRecoveryDevice device;
        FakeRecoveryOwner owner;
        devicerecovery::State recovery;
        device.checkResult = S_PRESENT_MODE_CHANGED;
        device.resetResult = D3D_OK;
        const devicerecovery::Result result =
            RunRecovery(recovery, device, owner);
        ExpectBool("mode change stays on the ordinary Reset path",
                   result.outcome == devicerecovery::Outcome::ResetRequired,
                   true);
        ExpectInt("mode change does not consume HUNG ResetEx", device.resets, 0);
        ExpectInt("mode change does not release HUNG owners", owner.releases, 0);
    }

    std::printf("=== deferred authored-change replay ===\n");
    {
        particlesystemchange::DeferredReplay deferred;
        int immediateTrack = -777;
        const bool blocked = deferred.DeferIfBlocked(true, 17);
        if (!blocked) immediateTrack = 17;
        ExpectBool("blocked track is deferred", blocked, true);
        ExpectInt("blocked track is not applied immediately",
                  immediateTrack, -777);

        int replayTrack = -777;
        ExpectBool("blocked track has one replay", deferred.Take(replayTrack),
                   true);
        ExpectInt("blocked replay preserves exact track", replayTrack, 17);
        replayTrack = -777;
        ExpectBool("blocked track drains exactly once",
                   deferred.Take(replayTrack), false);
        ExpectInt("empty replay leaves output untouched", replayTrack, -777);

        immediateTrack = -777;
        const bool healthy = deferred.DeferIfBlocked(false, 23);
        if (!healthy) immediateTrack = 23;
        ExpectBool("healthy track is not deferred", healthy, false);
        ExpectInt("healthy track applies immediately", immediateTrack, 23);
        replayTrack = -777;
        ExpectBool("healthy track creates no replay",
                   deferred.Take(replayTrack), false);
        ExpectInt("healthy path leaves replay sentinel untouched",
                  replayTrack, -777);

        deferred.DeferIfBlocked(true, 17);
        deferred.DeferIfBlocked(true, 17);
        ExpectBool("same blocked track has one replay",
                   deferred.Take(replayTrack), true);
        ExpectInt("same blocked track stays specific", replayTrack, 17);

        deferred.DeferIfBlocked(true, 17);
        deferred.DeferIfBlocked(true, 29);
        ExpectBool("different blocked tracks have one replay",
                   deferred.Take(replayTrack), true);
        ExpectInt("different blocked tracks widen to all", replayTrack, -1);

        deferred.DeferIfBlocked(true, 17);
        deferred.DeferIfBlocked(true, -9);
        ExpectBool("structural change has one replay",
                   deferred.Take(replayTrack), true);
        ExpectInt("any negative track normalizes to all", replayTrack, -1);

        deferred.DeferIfBlocked(true, 41);
        deferred.Reset();
        replayTrack = -777;
        ExpectBool("document clear cancels old replay",
                   deferred.Take(replayTrack), false);
        ExpectInt("cancelled replay leaves output untouched",
                  replayTrack, -777);

        deferred.Queue(31);
        int appliedTrack = -777;
        bool replayBlocked = false;
        ExpectBool(
            "healthy replay completes",
            deferred.Replay(
                [&](int track) { appliedTrack = track; },
                [&]() { return replayBlocked; }),
            true);
        ExpectInt("healthy replay applies exact track", appliedTrack, 31);
        replayTrack = -777;
        ExpectBool("completed replay leaves no pending work",
                   deferred.Take(replayTrack), false);

        deferred.Queue(37);
        appliedTrack = -777;
        ExpectBool(
            "throwing replay aborts frame",
            deferred.Replay(
                [&](int track)
                {
                    appliedTrack = track;
                    throw 37;
                },
                []() { return false; }),
            false);
        ExpectInt("throwing replay attempted exact track", appliedTrack, 37);
        replayTrack = -777;
        ExpectBool("throwing replay remains pending",
                   deferred.Take(replayTrack), true);
        ExpectInt("throwing replay preserves exact track", replayTrack, 37);

        deferred.Queue(17);
        replayBlocked = false;
        ExpectBool(
            "reblocked replay aborts frame",
            deferred.Replay(
                [&](int)
                {
                    deferred.Queue(29);
                    replayBlocked = true;
                },
                [&]() { return replayBlocked; }),
            false);
        replayTrack = -777;
        ExpectBool("reblocked replay remains pending",
                   deferred.Take(replayTrack), true);
        ExpectInt("reentrant and original tracks widen to all",
                  replayTrack, -1);

        deferred.Queue(17);
        int preReloadTrack = -777;
        ExpectBool("full reload takes the pre-attempt authored batch",
                   deferred.Take(preReloadTrack), true);
        ExpectInt("pre-attempt batch keeps its exact track",
                  preReloadTrack, 17);
        const bool textureReloadApplying = true;
        int reentrantAppliedTrack = -777;
        const bool reentrantDeferred = deferred.DeferIfBlocked(
            textureReloadApplying, 29);
        if (!reentrantDeferred) reentrantAppliedTrack = 29;
        ExpectBool("reload-applying guard defers a reentrant edit",
                   reentrantDeferred, true);
        ExpectInt("reentrant edit is not applied inside full reload",
                  reentrantAppliedTrack, -777);
        ExpectBool("newer reentrant batch remains pending",
                   deferred.Pending(), true);
        replayTrack = -777;
        ExpectBool("next frame can take the newer batch",
                   deferred.Take(replayTrack), true);
        ExpectInt("newer batch remains exact rather than widening with old",
                  replayTrack, 29);
    }

    // The predicates above are not proof that production forwards either
    // presentation result, nor that Reset drops every owning emitter reference
    // before calling IDirect3DDevice9::Reset. Pin those call sites and the
    // release/reset/reacquire order because the native unit builder deliberately
    // does not link the D3D engine or host translation units.
    std::printf("=== production device-reset binding ===\n");
    {
        const std::filesystem::path repoRoot = std::filesystem::current_path();
        const std::string engineHeader =
            ReadSource(repoRoot / "src" / "engine.h");
        const std::string engineSource =
            ReadSource(repoRoot / "src" / "engine.cpp");
        const std::string renderSource =
            ReadSource(repoRoot / "src" / "engine_render.cpp");
        const std::string bridgeEngineSource =
            ReadSource(repoRoot / "src" / "host" /
                       "BridgeDispatch_Engine.cpp");
        const std::string bridgeAssetsSource =
            ReadSource(repoRoot / "src" / "host" /
                       "BridgeDispatch_Assets.cpp");
        const std::string modManagerSource =
            ReadSource(repoRoot / "src" / "ModManager.cpp");
        const std::string recoveryHeader =
            ReadSource(repoRoot / "src" / "DeviceRecovery.h");
        const std::string deferredChangeHeader =
            ReadSource(repoRoot / "src" /
                       "DeferredParticleSystemChange.h");
        const std::string hostSource =
            ReadSource(repoRoot / "src" / "host" / "HostWindow.cpp");
        const std::string layoutSource =
            ReadSource(repoRoot / "src" / "host" / "LayoutBroker.cpp");
        const std::string compositorHeader =
            ReadSource(repoRoot / "src" / "host" / "Compositor.h");
        const std::string compositorSource =
            ReadSource(repoRoot / "src" / "host" / "Compositor.cpp");
        const std::string managerHeader =
            ReadSource(repoRoot / "src" / "managers.h");
        const std::string mainSource =
            ReadSource(repoRoot / "src" / "main.cpp");
        const std::string emitterHeader =
            ReadSource(repoRoot / "src" / "EmitterInstance.h");
        const std::string emitterSource =
            ReadSource(repoRoot / "src" / "EmitterInstance.cpp");
        const std::string instanceHeader =
            ReadSource(repoRoot / "src" / "ParticleSystemInstance.h");
        const std::string instanceSource =
            ReadSource(repoRoot / "src" / "ParticleSystemInstance.cpp");
        const std::string environmentSource =
            ReadSource(repoRoot / "src" / "engine_environment.cpp");
        const std::string referenceSource =
            ReadSource(repoRoot / "src" / "engine_reference.cpp");

        ExpectBool("production device-reset sources are readable",
                   !engineHeader.empty() && !engineSource.empty() &&
                    !renderSource.empty() && !bridgeEngineSource.empty() &&
                    !bridgeAssetsSource.empty() && !modManagerSource.empty() &&
                    !recoveryHeader.empty() &&
                    !deferredChangeHeader.empty() &&
                    !hostSource.empty() && !layoutSource.empty() &&
                    !compositorHeader.empty() &&
                    !compositorSource.empty() && !managerHeader.empty() &&
                    !mainSource.empty() &&
                    !emitterHeader.empty() && !emitterSource.empty() &&
                    !instanceHeader.empty() && !instanceSource.empty() &&
                    !environmentSource.empty() && !referenceSource.empty(),
                    true);

        ExpectBool("production adapter instantiates the executable recovery port",
                   Contains(engineSource,
                            "D3D9ExRecoveryPort<IDirect3DDevice9Ex, Engine>") &&
                   Contains(engineSource,
                            "m_pAlphaCompositor != nullptr") &&
                   Contains(recoveryHeader,
                            "bool renderWhenOccluded = false") &&
                   Contains(recoveryHeader,
                            "renderWhenOccluded && "
                            "state == S_PRESENT_OCCLUDED"),
                   true);
        ExpectBool("recovery port binds CheckDeviceState, never the legacy probe",
                   Contains(recoveryHeader,
                            "return m_device->CheckDeviceState(nullptr);") &&
                   !Contains(recoveryHeader,
                             "return m_device->TestCooperativeLevel();"),
                   true);
        ExpectBool("render call site executes the shared recovery front door",
                    Contains(renderSource,
                             "if (!PrepareDeviceForFrame()) return false;"),
                    true);
        const size_t prepareBegin =
            engineSource.find("bool Engine::PrepareDeviceForFrame()");
        const size_t recoverBegin =
            engineSource.find("bool Engine::RecoverDeviceIfNeeded()");
        const std::string prepareBody =
            prepareBegin != std::string::npos && recoverBegin > prepareBegin
                ? engineSource.substr(prepareBegin, recoverBegin - prepareBegin)
                : std::string();
        const size_t prepareFatalReturn =
            prepareBody.find("return false;");
        const size_t prepareRecoveryCondition =
            prepareBody.find("if (m_presentSuspect ||");
        const size_t prepareRecoveryCall =
            prepareBody.find("if (!RecoverDeviceIfNeeded()) return false;");
        const size_t prepareBlockedCheck =
            prepareBody.find("if (DeviceCallsBlocked()) return false;",
                             prepareRecoveryCall);
        const size_t prepareTextureReplay =
            prepareBody.find(
                "if (!ReplayPendingTextureReload()) return false;",
                prepareBlockedCheck);
        const size_t prepareAuthoredReplay =
            prepareBody.find("return ReplayPendingParticleSystemChange();");
        ExpectBool("frame preparation recovers then reloads before authored replay",
                   prepareFatalReturn != std::string::npos &&
                   prepareRecoveryCondition != std::string::npos &&
                   prepareRecoveryCall != std::string::npos &&
                   prepareBlockedCheck != std::string::npos &&
                   prepareTextureReplay != std::string::npos &&
                   prepareAuthoredReplay != std::string::npos &&
                   Contains(prepareBody,
                            "m_presentSuspect ||") &&
                   Contains(prepareBody,
                            "m_deviceRecovery.phase == "
                            "devicerecovery::Phase::ResetExFailed ||") &&
                   Contains(prepareBody, "m_fullResetPending)") &&
                   prepareFatalReturn < prepareRecoveryCondition &&
                   prepareRecoveryCondition < prepareRecoveryCall &&
                   prepareRecoveryCall < prepareBlockedCheck &&
                   prepareBlockedCheck < prepareTextureReplay &&
                   prepareTextureReplay < prepareAuthoredReplay,
                   true);
        ExpectBool("Update uses recovery front door before simulation",
                   Contains(renderSource,
                            "void Engine::Update()\n{\n\t"
                            "if (!PrepareDeviceForFrame()) return;"),
                   true);
        const size_t recoverEnd =
            engineSource.find("void Engine::ReportFatalDeviceState", recoverBegin);
        const std::string recoverBody =
            recoverBegin != std::string::npos && recoverEnd > recoverBegin
                ? engineSource.substr(recoverBegin, recoverEnd - recoverBegin)
                : std::string();
        ExpectBool("non-render recovery call site executes the same coordinator",
                   Contains(recoverBody, "result = ProbeDeviceRecovery();"),
                   true);
        const size_t resetRequiredBegin =
            recoverBody.find("case devicerecovery::Outcome::ResetRequired:");
        const size_t retryResetExBegin =
            recoverBody.find("case devicerecovery::Outcome::RetryResetEx:",
                             resetRequiredBegin);
        const std::string resetRequiredBody =
            resetRequiredBegin != std::string::npos &&
            retryResetExBegin > resetRequiredBegin
                ? recoverBody.substr(resetRequiredBegin,
                                     retryResetExBegin - resetRequiredBegin)
                : std::string();
        const size_t resetRequiredProbe =
            resetRequiredBody.find("result = ProbeDeviceRecovery();");
        const size_t resetRequiredClearSuspect =
            resetRequiredBody.find("m_presentSuspect = false;",
                                   resetRequiredProbe);
        const size_t resetRequiredRender =
            resetRequiredBody.find("return true;",
                                   resetRequiredClearSuspect);
        ExpectBool("ordinary reset clears suspect latch before replay",
                   resetRequiredProbe != std::string::npos &&
                   resetRequiredClearSuspect != std::string::npos &&
                   resetRequiredRender != std::string::npos &&
                   resetRequiredProbe < resetRequiredClearSuspect &&
                   resetRequiredClearSuspect < resetRequiredRender,
                   true);
        ExpectBool("host gates query and composition on a completed render",
                   Contains(hostSource,
                            "const bool rendered = engine->Render();") &&
                   Contains(hostSource,
                            "if (rendered && m_compositor && m_compositor->IsReady())"),
                   true);
        const size_t prepareFramePos =
            hostSource.find("if (!engine->PrepareComposedFrame()) return;");
        const size_t spawnerTickPos =
            hostSource.find("spawnerDriver->Tick(", prepareFramePos);
        const size_t engineRenderPos =
            hostSource.find("const bool rendered = engine->Render();",
                            prepareFramePos);
        ExpectBool("host recovers before spawner/update/render D3D work",
                   prepareFramePos != std::string::npos &&
                   spawnerTickPos != std::string::npos &&
                   engineRenderPos != std::string::npos &&
                   prepareFramePos < spawnerTickPos &&
                   spawnerTickPos < engineRenderPos,
                   true);
        const size_t renderD3D9Begin =
            hostSource.find("void HostWindowImpl::RenderD3D9()");
        const size_t renderD3D9End =
            hostSource.find(
                "\nvoid HostWindowImpl::",
                renderD3D9Begin == std::string::npos
                    ? 0
                    : renderD3D9Begin + 1);
        const std::string renderD3D9Body =
            renderD3D9Begin != std::string::npos &&
                    renderD3D9End > renderD3D9Begin
                ? hostSource.substr(
                      renderD3D9Begin,
                      renderD3D9End - renderD3D9Begin)
                : std::string();
        const size_t issueFrameQueryPos =
            renderD3D9Body.find("engine->IssueEndFrameQuery();");
        const size_t waitFrameQueryPos =
            renderD3D9Body.find("engine->WaitEndFrameQuery();",
                               issueFrameQueryPos);
        const size_t compositeFramePos =
            renderD3D9Body.find(
                "m_compositor->CompositeEngineFrame(",
                waitFrameQueryPos);
        const size_t sharedHandlePos =
            renderD3D9Body.find(
                "engine->GetSharedTextureHandle()",
                compositeFramePos);
        ExpectBool("host waits before acquiring the shared handle for composite",
                   !renderD3D9Body.empty() &&
                   issueFrameQueryPos != std::string::npos &&
                   waitFrameQueryPos != std::string::npos &&
                   sharedHandlePos != std::string::npos &&
                   compositeFramePos != std::string::npos &&
                   CountOccurrences(
                       renderD3D9Body,
                       "engine->GetSharedTextureHandle()") == 1 &&
                   Contains(
                       renderD3D9Body,
                       "m_compositor->CompositeEngineFrame("
                       "engine->GetSharedTextureHandle())") &&
                   issueFrameQueryPos < waitFrameQueryPos &&
                   waitFrameQueryPos < compositeFramePos &&
                   compositeFramePos < sharedHandlePos,
                   true);
        ExpectBool("composed coordinator uses conditional frame admission",
                   Contains(engineHeader, "bool PrepareComposedFrame();") &&
                   Contains(engineSource,
                            "bool Engine::PrepareComposedFrame()\n"
                            "{\n"
                            "\t++m_composedFramePrepareCount;\n"
                            "\treturn PrepareDeviceForFrame();\n"
                            "}") &&
                   !Contains(engineSource,
                             "PrepareDeviceForFrame(bool probeHealthyDevice)") &&
                   Contains(engineSource,
                            "++m_deviceStateProbeCount;") &&
                   CountOccurrences(hostSource,
                                    "engine->PrepareComposedFrame()") == 1,
                   true);

        const size_t issueQueryBegin =
            engineSource.find("void Engine::IssueEndFrameQuery()");
        const size_t waitQueryBegin =
            engineSource.find("int Engine::WaitEndFrameQuery()",
                              issueQueryBegin);
        const size_t waitQueryEnd =
            engineSource.find("// adapter LUID accessor", waitQueryBegin);
        const std::string issueQueryBody =
            issueQueryBegin != std::string::npos &&
                    waitQueryBegin > issueQueryBegin
                ? engineSource.substr(
                      issueQueryBegin, waitQueryBegin - issueQueryBegin)
                : std::string();
        const std::string waitQueryBody =
            waitQueryBegin != std::string::npos &&
                    waitQueryEnd > waitQueryBegin
                ? engineSource.substr(
                      waitQueryBegin, waitQueryEnd - waitQueryBegin)
                : std::string();
        const size_t queryGetData =
            waitQueryBody.find(
                "queryResult =\n"
                "\t\t    m_pEndFrameQuery->GetData(");
        const size_t queryOverride =
            waitQueryBody.find(
                "m_endFrameQueryResultOverrideRemaining > 0",
                queryGetData);
        const size_t queryPending =
            waitQueryBody.find(
                "if (queryResult != S_FALSE) break;", queryOverride);
        const size_t queryTimeout =
            waitQueryBody.find(
                "++m_endFrameQueryTimeoutCount", queryPending);
        const size_t queryFailed =
            waitQueryBody.find("if (FAILED(queryResult))", queryTimeout);
        const size_t queryLatch =
            waitQueryBody.find("m_presentSuspect = true;", queryFailed);
        const size_t queryRelease =
            waitQueryBody.find(
                "SAFE_RELEASE(m_pEndFrameQuery);", queryLatch);
        ExpectBool("real query HRESULT latches next-frame recovery and releases",
                   Contains(issueQueryBody,
                            "++m_endFrameQueryCreateCount;") &&
                   queryGetData != std::string::npos &&
                   queryOverride != std::string::npos &&
                   queryPending != std::string::npos &&
                   queryTimeout != std::string::npos &&
                   queryFailed != std::string::npos &&
                   queryLatch != std::string::npos &&
                   queryRelease != std::string::npos &&
                   queryGetData < queryOverride &&
                   queryOverride < queryPending &&
                   queryPending < queryTimeout &&
                   queryTimeout < queryFailed &&
                   queryFailed < queryLatch &&
                   queryLatch < queryRelease,
                   true);
        ExpectBool("failed query suppresses the current shared-handle copy",
                   Contains(engineSource,
                            "HANDLE Engine::GetSharedTextureHandle() const\n"
                            "{\n"
                            "\tif (DeviceCallsBlocked()) return nullptr;") &&
                   Contains(engineHeader,
                            "return m_presentSuspect ||"),
                   true);
        ExpectBool("test host injects at the production query-result door",
                   Contains(engineHeader,
                            "InjectEndFrameQueryResultForTesting(") &&
                   Contains(bridgeEngineSource,
                            "action == \"inject-query-result\"") &&
                   Contains(bridgeEngineSource,
                            "m_engine->"
                            "InjectEndFrameQueryResultForTesting(") &&
                   Contains(bridgeEngineSource,
                            "result == \"s-false\"") &&
                   Contains(bridgeEngineSource,
                            "result == \"device-lost\"") &&
                   !Contains(bridgeEngineSource,
                             "m_engine->m_presentSuspect"),
                   true);

        ExpectBool("Engine exposes the direct D3D9 present observer",
                   Contains(engineHeader, "void NotifyPresentResult(HRESULT hr);"), true);
        ExpectBool("Engine observer uses the tested suspect predicate",
                   Contains(engineSource,
                            "if (devicestate::ShouldCheckDeviceAfterPresent(hr))") &&
                   Contains(engineSource, "m_presentSuspect = true;"), true);
        ExpectBool("direct D3D9 Present forwards its actual HRESULT",
                   Contains(renderSource, "NotifyPresentResult(presentHr);"), true);
        const size_t compositeResult =
            hostSource.find("const ComposedFrameResult compositeResult =");
        const size_t compositeClassify =
            hostSource.find("ClassifyComposedFrameResult(compositeResult)",
                            compositeResult);
        const size_t compositeFatalPost =
            hostSource.find("WM_APP_COMPOSITION_FALLBACK",
                            compositeClassify);
        const std::string compositeBinding =
            compositeResult != std::string::npos &&
                    compositeFatalPost > compositeResult
                ? hostSource.substr(compositeResult,
                                    compositeFatalPost - compositeResult)
                : std::string();
        ExpectBool("composed Present1 uses its typed composition policy",
                   Contains(compositeBinding,
                            "m_compositor->CompositeEngineFrame(") &&
                   compositeClassify != std::string::npos &&
                   compositeFatalPost != std::string::npos &&
                   !Contains(compositeBinding, "NotifyPresentResult("),
                   true);
        const size_t compositeFrameBegin = compositorSource.find(
            "ComposedFrameResult Compositor::CompositeEngineFrame(");
        const size_t refreshSharedHandleBegin = compositorSource.find(
            "HRESULT Compositor::RefreshEngineSharedHandle(",
            compositeFrameBegin);
        const size_t releaseSharedHandleBegin = compositorSource.find(
            "void Compositor::ReleaseEngineSharedHandle()",
            refreshSharedHandleBegin);
        const std::string compositeFrameBody =
            compositeFrameBegin != std::string::npos &&
                    refreshSharedHandleBegin > compositeFrameBegin
                ? compositorSource.substr(
                      compositeFrameBegin,
                      refreshSharedHandleBegin - compositeFrameBegin)
                : std::string();
        const std::string refreshSharedHandleBody =
            refreshSharedHandleBegin != std::string::npos &&
                    releaseSharedHandleBegin > refreshSharedHandleBegin
                ? compositorSource.substr(
                      refreshSharedHandleBegin,
                      releaseSharedHandleBegin - refreshSharedHandleBegin)
                : std::string();
        const size_t presentCall = compositeFrameBody.find(
            "m_impl->engineSwapChain->Present1(0, 0, &pp)");
        const size_t presentFailure = compositeFrameBody.find(
            "if (FAILED(hr))", presentCall);
        const size_t presentFailureReturn = compositeFrameBody.find(
            "return ComposedFrameResult::PresentResult(hr);",
            presentFailure);
        const size_t presentSuccessReturn = compositeFrameBody.rfind(
            "return ComposedFrameResult::PresentResult(hr);");
        const size_t refreshCacheCommit = refreshSharedHandleBody.find(
            "m_impl->engineHandleCached = sharedTexture;");
        const size_t refreshSuccessReturn = refreshSharedHandleBody.rfind(
            "return S_OK;");
        ExpectBool("compositor preserves shared-handle versus Present1 provenance",
                   Contains(compositorHeader,
                            "enum class ComposedFrameStage") &&
                   Contains(compositorHeader,
                            "ComposedFrameStage::SharedHandle") &&
                   Contains(compositorHeader,
                            "ComposedFrameStage::Present1") &&
                   Contains(compositeFrameBody,
                            "ComposedFrameResult::SharedHandleFailure(rhr)") &&
                   presentCall != std::string::npos &&
                   presentFailure != std::string::npos &&
                   presentFailureReturn != std::string::npos &&
                   presentSuccessReturn != std::string::npos &&
                   presentFailureReturn < presentSuccessReturn &&
                   CountOccurrences(
                       compositeFrameBody,
                       "return ComposedFrameResult::PresentResult(hr);") == 2,
                   true);
        ExpectBool("successful Present1 returns its exact typed production result",
                   presentSuccessReturn != std::string::npos &&
                   presentSuccessReturn > presentCall &&
                   !Contains(compositeFrameBody, "return S_OK;"),
                   true);
        ExpectBool("successful shared-handle refresh keeps its HRESULT contract",
                   refreshCacheCommit != std::string::npos &&
                   refreshSuccessReturn != std::string::npos &&
                   refreshSuccessReturn > refreshCacheCommit &&
                   CountOccurrences(refreshSharedHandleBody,
                                    "return S_OK;") == 1 &&
                   !Contains(refreshSharedHandleBody,
                             "ComposedFrameResult::"),
                   true);

        ExpectBool("EmitterInstance declares a pre-Reset texture release phase",
                   Contains(emitterHeader, "void  ReleaseDeviceTextures();"), true);
        const size_t emitterRelease =
            emitterSource.find("void EmitterInstance::ReleaseDeviceTextures()");
        const size_t emitterReacquire =
            emitterSource.find("void EmitterInstance::ReacquireDeviceTextures(");
        const std::string emitterReleaseBody =
            emitterRelease != std::string::npos && emitterReacquire > emitterRelease
                ? emitterSource.substr(emitterRelease, emitterReacquire - emitterRelease)
                : std::string();
        ExpectBool("pre-Reset release drops the color texture's +1 reference",
                   Contains(emitterReleaseBody, "SAFE_RELEASE(m_pColorTexture);"), true);
        ExpectBool("pre-Reset release drops the normal texture's +1 reference",
                   Contains(emitterReleaseBody, "SAFE_RELEASE(m_pNormalTexture);"), true);
        ExpectBool("post-Reset reacquire reuses the idempotent release phase",
                   emitterReacquire != std::string::npos &&
                   Contains(emitterSource.substr(emitterReacquire),
                            "ReleaseDeviceTextures();"), true);

        ExpectBool("ParticleSystemInstance declares texture release forwarding",
                   Contains(instanceHeader, "void ReleaseDeviceTextures();"), true);
        ExpectBool("ParticleSystemInstance forwards release to every emitter",
                   Contains(instanceSource, "emitter->ReleaseDeviceTextures();"), true);
        ExpectBool("Engine declares texture release forwarding",
                   Contains(engineHeader, "void ReleaseInstanceTextures();"), true);
        ExpectBool("Engine forwards release to every particle-system instance",
                   Contains(engineSource, "instance->ReleaseDeviceTextures();"), true);

        ExpectBool("ShaderManager exposes full cached-effect lifecycle",
                   Contains(managerHeader, "virtual void OnLostDevice() = 0;") &&
                   Contains(managerHeader, "virtual void OnResetDevice() = 0;") &&
                   Contains(mainSource, "void OnLostDevice() override") &&
                   Contains(mainSource, "void OnResetDevice() override") &&
                   Contains(mainSource, "std::set<Effect*> unique;"),
                   true);

        const size_t releaseBegin =
            engineSource.find("void Engine::ReleaseDeviceResourcesForReset()");
        const size_t effectResetBegin =
            engineSource.find("void Engine::ResetDeviceEffectsAfterReset()",
                              releaseBegin);
        const size_t refreshBegin =
            engineSource.find("HRESULT Engine::RefreshPresentationParametersAfterReset()",
                              effectResetBegin);
        const size_t reacquireBegin =
            engineSource.find("void Engine::ReacquireDeviceResourcesAfterReset()",
                              refreshBegin);
        const size_t resetBegin =
            engineSource.find("void Engine::Reset()", reacquireBegin);
        const size_t resetEnd =
            engineSource.find("bool Engine::ResetForResize()", resetBegin);
        const std::string releaseBody =
            releaseBegin != std::string::npos && effectResetBegin > releaseBegin
                ? engineSource.substr(releaseBegin,
                                      effectResetBegin - releaseBegin)
                : std::string();
        const std::string effectResetBody =
            effectResetBegin != std::string::npos && refreshBegin > effectResetBegin
                ? engineSource.substr(effectResetBegin,
                                      refreshBegin - effectResetBegin)
                : std::string();
        const std::string refreshBody =
            refreshBegin != std::string::npos && reacquireBegin > refreshBegin
                ? engineSource.substr(refreshBegin,
                                      reacquireBegin - refreshBegin)
                : std::string();
        const std::string reacquireBody =
            reacquireBegin != std::string::npos && resetBegin > reacquireBegin
                ? engineSource.substr(reacquireBegin,
                                      resetBegin - reacquireBegin)
                : std::string();
        const std::string resetBody =
            resetBegin != std::string::npos && resetEnd > resetBegin
                ? engineSource.substr(resetBegin, resetEnd - resetBegin)
                : std::string();
        const size_t compositorReleaseBegin =
            compositorSource.find(
                "void Compositor::ReleaseEngineSharedHandle() noexcept");
        const size_t compositorReleaseEnd =
            compositorSource.find("// ---------- scene-rect transform",
                                  compositorReleaseBegin);
        const std::string compositorReleaseBody =
            compositorReleaseBegin != std::string::npos &&
                    compositorReleaseEnd > compositorReleaseBegin
                ? compositorSource.substr(
                      compositorReleaseBegin,
                      compositorReleaseEnd - compositorReleaseBegin)
                : std::string();
        const size_t releasePos =
            releaseBody.find("ReleaseInstanceTextures();");
        const size_t cacheLostPos =
            releaseBody.find("m_textureManager.OnLostDevice();");
        const size_t aliasReleasePos =
            releaseBody.find("m_pCompositionCompositor->ReleaseEngineSharedHandle();");
        const size_t d3d9SharedReleasePos =
            releaseBody.find("m_pAlphaCompositor->ReleaseGpuResources();");
        const size_t releasePhasePos =
            resetBody.find("ReleaseDeviceResourcesForReset();");
        const size_t resetPos = resetBody.find("m_pDevice->Reset(");
        const size_t resetEffectsPos =
            resetBody.find("ResetDeviceEffectsAfterReset();");
        const size_t refreshExtentsPos =
            resetBody.find("RefreshPresentationParametersAfterReset()");
        const size_t reacquirePhasePos =
            resetBody.find("ReacquireDeviceResourcesAfterReset();");
        const size_t aliasClearPos =
            compositorReleaseBody.find("m_impl->d3d11Context->ClearState();");
        const size_t aliasFlushPos =
            compositorReleaseBody.find("m_impl->d3d11Context->Flush();");
        const size_t aliasResetPos =
            compositorReleaseBody.find("m_impl->sharedTexD3D11.Reset();");
        const size_t aliasHandleClearPos =
            compositorReleaseBody.find("m_impl->engineHandleCached = nullptr;");
        ExpectBool("emitter refs release before cache loss and device Reset",
                   releasePos != std::string::npos &&
                   cacheLostPos != std::string::npos &&
                   releasePos < cacheLostPos &&
                   releasePhasePos != std::string::npos &&
                   resetPos != std::string::npos &&
                   releasePhasePos < resetPos, true);
        ExpectBool("failed Reset cannot nest the full release phase",
                   Contains(releaseBody,
                            "if (m_deviceResourcesReleased) return;") &&
                   Contains(releaseBody,
                            "m_deviceResourcesReleased = true;") &&
                   Contains(reacquireBody,
                            "m_deviceResourcesReleased = false;"),
                   true);
        ExpectBool("D3D11 alias releases before its D3D9 shared texture",
                   aliasReleasePos != std::string::npos &&
                   d3d9SharedReleasePos != std::string::npos &&
                   aliasReleasePos < d3d9SharedReleasePos &&
                   Contains(compositorHeader,
                             "void ReleaseEngineSharedHandle() noexcept;") &&
                   aliasClearPos != std::string::npos &&
                   aliasFlushPos != std::string::npos &&
                   aliasResetPos != std::string::npos &&
                   aliasHandleClearPos != std::string::npos &&
                   aliasClearPos < aliasFlushPos &&
                   aliasFlushPos < aliasResetPos &&
                   aliasResetPos < aliasHandleClearPos,
                   true);
        ExpectBool("normal Reset orders full release, Reset, then reacquire",
                   releasePhasePos != std::string::npos &&
                   resetPos != std::string::npos &&
                   resetEffectsPos != std::string::npos &&
                   refreshExtentsPos != std::string::npos &&
                   reacquirePhasePos != std::string::npos &&
                   releasePhasePos < resetPos &&
                   resetPos < resetEffectsPos &&
                   resetEffectsPos < refreshExtentsPos &&
                   refreshExtentsPos < reacquirePhasePos, true);

        const size_t managerResetPos =
            effectResetBody.find("m_shaderManager.OnResetDevice();");
        const size_t shaderBindPos =
            reacquireBody.find("BindShaderTextures(m_pShaders[i]);");
        const size_t emitterReacquirePos =
            reacquireBody.find("ReacquireInstanceTextures();");
        ExpectBool("all cached effects reset before texture/device recreation",
                   managerResetPos != std::string::npos &&
                   shaderBindPos != std::string::npos &&
                   emitterReacquirePos != std::string::npos &&
                   shaderBindPos < emitterReacquirePos, true);

        ExpectBool("post-Reset extents come from the actual back buffer",
                   Contains(refreshBody, "m_pDevice->GetBackBuffer(") &&
                   Contains(refreshBody, "backBuffer->GetDesc(&desc)") &&
                   Contains(refreshBody,
                            "m_presentationParameters.BackBufferWidth  = desc.Width;") &&
                   Contains(refreshBody,
                            "m_presentationParameters.BackBufferHeight = desc.Height;"),
                   true);
        ExpectBool("normal Reset passes a disposable in-out parameter copy",
                   Contains(resetBody,
                            "GetDeviceRecoveryPresentationParameters();") &&
                   Contains(resetBody, "m_pDevice->Reset(&parameters)") &&
                   !Contains(resetBody,
                             "m_pDevice->Reset(&m_presentationParameters)"),
                   true);

        const std::initializer_list<const char*> releaseOwners = {
            "ReleaseBloomTargets();",
            "ReleaseShadowMaskTargets();",
            "SAFE_RELEASE(m_pDistortTexture);",
            "SAFE_RELEASE(m_pSceneTexture);",
            "SAFE_RELEASE(m_pDepthStencilSurface);",
            "SAFE_RELEASE(m_pMsaaColor);",
            "SAFE_RELEASE(m_pMsaaDepth);",
            "m_pDistortShader->OnLostDevice();",
            "m_shaderManager.OnLostDevice();",
            "m_pSkydomeEffect->OnLostDevice();",
            "m_pGroundEffect->OnLostDevice();",
            "SAFE_RELEASE(m_pGroundNormalTexture);",
            "SAFE_RELEASE(m_pGroundFlatNormalTexture);",
            "ReleaseSkydomeMeshBuffers();",
            "SAFE_RELEASE(m_pSkydomeTexture);",
            "m_skydomePrimaryMesh.ReleaseGpuResources();",
            "m_skydomeSecondaryMesh.ReleaseGpuResources();",
            "m_referenceObjectMesh.ReleaseGpuResources();",
            "a->mesh.ReleaseGpuResources();",
            "SAFE_RELEASE(m_pGroundTexture);",
            "m_pCompositionCompositor->ReleaseEngineSharedHandle();",
            "m_pAlphaCompositor->ReleaseGpuResources();",
            "ReleaseInstanceTextures();",
            "m_textureManager.OnLostDevice();",
            "SAFE_RELEASE(m_pEndFrameQuery);",
        };
        for (const char* owner : releaseOwners)
        {
            const std::string label =
                std::string("full release ledger contains ") + owner;
            ExpectBool(label.c_str(), Contains(releaseBody, owner), true);
        }
        ExpectBool("mesh effects are not double-lost outside ShaderManager",
                   !Contains(releaseBody, "m_skydomePrimaryMesh.OnLostDevice()") &&
                   !Contains(releaseBody, "m_skydomeSecondaryMesh.OnLostDevice()") &&
                   !Contains(releaseBody, "m_referenceObjectMesh.OnLostDevice()") &&
                   !Contains(releaseBody, "a->mesh.OnLostDevice()"),
                   true);

        const std::initializer_list<const char*> effectResetOwners = {
            "m_pDistortShader->OnResetDevice();",
            "m_shaderManager.OnResetDevice();",
            "m_pSkydomeEffect->OnResetDevice();",
            "m_pGroundEffect->OnResetDevice();",
        };
        for (const char* owner : effectResetOwners)
        {
            const std::string label =
                std::string("effect-reset ledger contains ") + owner;
            ExpectBool(label.c_str(), Contains(effectResetBody, owner), true);
        }
        ExpectBool("mesh effects are not double-reset outside ShaderManager",
                   !Contains(effectResetBody,
                             "m_skydomePrimaryMesh.OnResetEffects()") &&
                   !Contains(effectResetBody,
                             "m_skydomeSecondaryMesh.OnResetEffects()") &&
                   !Contains(effectResetBody,
                             "m_referenceObjectMesh.OnResetEffects()") &&
                   !Contains(effectResetBody, "a->mesh.OnResetEffects()"),
                   true);

        const std::initializer_list<const char*> reacquireOwners = {
            "BindShaderTextures(m_pShaders[i]);",
            "CreateGroundFlatNormal();",
            "CreateSkydomeMeshBuffers();",
            "ReloadGroundTexture();",
            "ReloadGroundNormalTexture();",
            "ReloadSkydomeTexture(m_skydomeIndex);",
            "m_skydomePrimaryMesh.CreateBuffers(",
            "m_skydomeSecondaryMesh.CreateBuffers(",
            "m_referenceObjectMesh.CreateBuffers(",
            "a->mesh.CreateBuffers(",
            "ReacquireInstanceTextures();",
            "ResetParameters();",
            "m_pAlphaCompositor->Resize(",
            "SetSceneViewportUnchecked(",
        };
        for (const char* owner : reacquireOwners)
        {
            const std::string label =
                std::string("reacquire ledger contains ") + owner;
            ExpectBool(label.c_str(), Contains(reacquireBody, owner), true);
        }

        const size_t resizeEnd =
            engineSource.find("HANDLE Engine::GetSharedTextureHandle()", resetEnd);
        const std::string resizeResetBody =
            resetEnd != std::string::npos && resizeEnd > resetEnd
                ? engineSource.substr(resetEnd, resizeEnd - resetEnd)
                : std::string();
        const size_t cheapResetExPos =
            resizeResetBody.find("m_pDevice->ResetEx(&parameters, NULL)");
        const size_t cheapRefreshPos =
            resizeResetBody.find("RefreshPresentationParametersAfterReset()");
        const size_t cheapReleasePos =
            resizeResetBody.find("ReleaseBloomTargets();");
        const size_t cheapRebuildPos =
            resizeResetBody.find("ResetParameters();");
        ExpectBool("cheap ResetEx resize preserves refs until reset succeeds",
                   !Contains(resizeResetBody, "ReleaseInstanceTextures();") &&
                   !Contains(resizeResetBody, "ReacquireInstanceTextures();") &&
                   cheapResetExPos != std::string::npos &&
                   cheapRefreshPos != std::string::npos &&
                   cheapReleasePos != std::string::npos &&
                   cheapRebuildPos != std::string::npos &&
                   cheapResetExPos < cheapRefreshPos &&
                   cheapRefreshPos < cheapReleasePos &&
                   cheapReleasePos < cheapRebuildPos,
                   true);
        ExpectBool("failed cheap ResetEx enters pending coordinator state",
                   Contains(resizeResetBody,
                            "devicerecovery::RecordResetExFailure(") &&
                   Contains(resizeResetBody, "m_presentSuspect = true;") &&
                   !Contains(resizeResetBody, "m_pDevice->Reset("),
                   true);

        const size_t failedResizeFallback =
            layoutSource.find("else if (!resetOk)");
        const size_t failedResizeFallbackEnd =
            layoutSource.find("if (!resetOk && rebuildFailedAfterReset)",
                              failedResizeFallback);
        const std::string failedResizeBody =
            failedResizeFallback != std::string::npos &&
            failedResizeFallbackEnd > failedResizeFallback
                ? layoutSource.substr(failedResizeFallback,
                                      failedResizeFallbackEnd -
                                          failedResizeFallback)
                : std::string();
        ExpectBool("LayoutBroker never ordinary-Resets after ResetEx failure",
                   Contains(failedResizeBody,
                            "m_engine->RecoverDeviceIfNeeded();") &&
                   !Contains(failedResizeBody, "m_engine->Reset();"),
                   true);

        ExpectBool("raw device exposure closes with the external D3D gate",
                   Contains(engineHeader,
                            "return DeviceCallsBlocked() ? nullptr : m_pDevice;") &&
                   Contains(engineHeader,
                            "return m_presentSuspect ||") &&
                   Contains(engineHeader, "m_deviceResetInProgress ||") &&
                   Contains(engineHeader, "m_fullResetPending ||") &&
                   Contains(engineHeader,
                            "m_deviceRecoveryWorkTestHold ||") &&
                   Contains(engineHeader,
                            "devicerecovery::Phase::ResetExFailed") &&
                   Contains(engineHeader,
                            "devicerecovery::Phase::Recovering") &&
                   Contains(engineHeader,
                            "devicerecovery::Phase::Terminal"),
                   true);
        ExpectBool("reset-only texture reacquire bypass is private and narrow",
                   Contains(engineHeader,
                            "friend class EmitterInstance;") &&
                   Contains(engineHeader,
                            "GetTextureForDeviceReset(") &&
                   Contains(emitterSource,
                            "engine.GetTextureForDeviceReset(") &&
                   Contains(engineSource,
                            "if (DeviceCallsBlocked()) return NULL;"),
                   true);
        const size_t testHoldBegin =
            engineSource.find(
                "bool Engine::SetDeviceRecoveryWorkHoldForTesting(bool hold)");
        const size_t prepareAfterTestHold =
            engineSource.find("bool Engine::PrepareDeviceForFrame()",
                              testHoldBegin);
        const std::string testHoldBody =
            testHoldBegin != std::string::npos &&
            prepareAfterTestHold > testHoldBegin
                ? engineSource.substr(testHoldBegin,
                                      prepareAfterTestHold - testHoldBegin)
                : std::string();
        ExpectBool("synthetic hold arms only from healthy state and releases idempotently",
                   Contains(testHoldBody,
                            "m_deviceRecoveryWorkTestHold = false;") &&
                   Contains(testHoldBody,
                            "if (m_deviceRecoveryWorkTestHold) return true;") &&
                   Contains(testHoldBody,
                            "if (DeviceCallsBlocked() || m_presentSuspect) "
                            "return false;") &&
                   Contains(testHoldBody,
                            "m_deviceRecoveryWorkTestHold = true;"),
                   true);

        const size_t changeBegin =
            engineSource.find("void Engine::OnParticleSystemChanged(int track)");
        const size_t applyChangeBegin =
            engineSource.find("void Engine::ApplyParticleSystemChanged(int track)",
                              changeBegin);
        const std::string changeBody =
            changeBegin != std::string::npos &&
            applyChangeBegin > changeBegin
                ? engineSource.substr(changeBegin,
                                      applyChangeBegin - changeBegin)
                : std::string();
        const size_t deferCall =
            changeBody.find(
                "if (m_deferredParticleSystemChange.DeferIfBlocked(");
        const size_t immediateCall =
            changeBody.find("ApplyParticleSystemChanged(track);");
        const bool productionDefersDuringTextureReload =
            Contains(
                changeBody,
                "if (m_deferredParticleSystemChange.DeferIfBlocked(\n"
                "\t        DeviceCallsBlocked() || m_presentSuspect ||\n"
                "\t        m_textureReloadApplying, track))\n"
                "\t\treturn;");
        ExpectBool("blocked authored change is queued at production door",
                   deferCall != std::string::npos &&
                   productionDefersDuringTextureReload &&
                   immediateCall != std::string::npos &&
                   deferCall < immediateCall,
                   true);
        particlesystemchange::DeferredReplay productionBoundReplay;
        productionBoundReplay.Queue(17);
        int productionOldTrack = -777;
        productionBoundReplay.Take(productionOldTrack);
        int productionReentrantApplied = -777;
        const bool productionReentrantDeferred =
            productionBoundReplay.DeferIfBlocked(
                productionDefersDuringTextureReload, 29);
        if (!productionReentrantDeferred) productionReentrantApplied = 29;
        ExpectInt("production reload guard prevents immediate reentrant apply",
                  productionReentrantApplied, -777);
        int productionNextTrack = -777;
        ExpectBool("production reload guard leaves reentrant work pending",
                   productionBoundReplay.Take(productionNextTrack), true);
        ExpectInt("production-bound next replay keeps exact new track",
                  productionNextTrack, 29);
        ExpectBool("authored-only door never schedules a texture reload",
                   !Contains(changeBody,
                             "m_textureReloadRequestGeneration") &&
                   !Contains(changeBody, "ReloadTextures("),
                   true);

        const size_t textureReplayBegin =
            engineSource.find("bool Engine::ReplayPendingTextureReload()");
        const size_t authoredReplayBegin =
            engineSource.find(
                "bool Engine::ReplayPendingParticleSystemChange()",
                textureReplayBegin);
        const std::string textureReplayBody =
            textureReplayBegin != std::string::npos &&
            authoredReplayBegin > textureReplayBegin
                ? engineSource.substr(textureReplayBegin,
                                      authoredReplayBegin -
                                          textureReplayBegin)
                : std::string();
        const size_t takeDeferred =
            textureReplayBody.find(
                "m_deferredParticleSystemChange.Take(deferredTrack);");
        const size_t applyGuardOn =
            textureReplayBody.find("m_textureReloadApplying = true;");
        const size_t performReload =
            textureReplayBody.find("completed = PerformTextureReload();");
        const size_t applyGuardOff =
            textureReplayBody.find("m_textureReloadApplying = false;",
                                   performReload);
        const size_t retryDecision =
            textureReplayBody.find(
                "if (!completed || !TextureReloadCanContinue())");
        const size_t retryTerminal =
            textureReplayBody.find("if (IsTerminalDeviceState())",
                                   retryDecision);
        const size_t retryMerge =
            textureReplayBody.find(
                "m_deferredParticleSystemChange.Queue(deferredTrack);",
                retryTerminal);
        const size_t generationCommit =
            textureReplayBody.find(
                "m_textureReloadAppliedGeneration = targetGeneration;");
        const size_t completionCount =
            textureReplayBody.find("++m_textureReloadApplyCount;",
                                   generationCommit);
        ExpectBool("texture replay snapshots and consumes only the old authored batch",
                   Contains(textureReplayBody,
                            "const uint64_t documentEpoch = "
                            "m_particleSystemDocumentEpoch;") &&
                   takeDeferred != std::string::npos &&
                   applyGuardOn != std::string::npos &&
                   performReload != std::string::npos &&
                   applyGuardOff != std::string::npos &&
                   takeDeferred < applyGuardOn &&
                   applyGuardOn < performReload &&
                   performReload < applyGuardOff,
                   true);
        ExpectBool("texture replay retains retryable work but never resurrects terminal work",
                   retryDecision != std::string::npos &&
                   retryTerminal != std::string::npos &&
                   retryMerge != std::string::npos &&
                   Contains(textureReplayBody,
                            "documentEpoch == "
                            "m_particleSystemDocumentEpoch") &&
                   retryDecision < retryTerminal &&
                   retryTerminal < retryMerge,
                   true);
        ExpectBool("successful reload commits one generation and leaves newer work pending",
                   generationCommit != std::string::npos &&
                   completionCount != std::string::npos &&
                   Contains(textureReplayBody,
                            "if (TextureReloadPendingForTesting()) "
                            "return false;") &&
                   Contains(textureReplayBody,
                            "if (m_deferredParticleSystemChange.Pending()) "
                            "return false;") &&
                   generationCommit < completionCount,
                   true);

        const size_t reloadBegin =
            renderSource.find("void Engine::ReloadTextures()");
        const size_t performBegin =
            renderSource.find("bool Engine::PerformTextureReload()",
                              reloadBegin);
        const size_t updateBegin =
            renderSource.find("void Engine::Update()", performBegin);
        const std::string reloadBody =
            reloadBegin != std::string::npos && performBegin > reloadBegin
                ? renderSource.substr(reloadBegin,
                                      performBegin - reloadBegin)
                : std::string();
        const std::string performBody =
            performBegin != std::string::npos && updateBegin > performBegin
                ? renderSource.substr(performBegin,
                                      updateBegin - performBegin)
                : std::string();
        const size_t terminalDiscard =
            reloadBody.find("if (IsTerminalDeviceState())");
        const size_t requestGeneration =
            reloadBody.find("++m_textureReloadRequestGeneration;");
        const size_t blockedReturn =
            reloadBody.find(
                "if (DeviceCallsBlocked() || m_presentSuspect) return;",
                requestGeneration);
        const size_t sharedReplay =
            reloadBody.find("ReplayPendingTextureReload();", blockedReturn);
        ExpectBool("public reload queues before retryable device blocking",
                   terminalDiscard != std::string::npos &&
                   requestGeneration != std::string::npos &&
                   blockedReturn != std::string::npos &&
                   sharedReplay != std::string::npos &&
                   terminalDiscard < requestGeneration &&
                   requestGeneration < blockedReturn &&
                   blockedReturn < sharedReplay,
                   true);
        ExpectBool("full reload uses the broad private apply and checks every phase",
                   Contains(performBody,
                            "ApplyParticleSystemChanged(-1);") &&
                   !Contains(performBody,
                             "OnParticleSystemChanged(-1);") &&
                   CountOccurrences(
                       performBody,
                       "if (!TextureReloadCanContinue()) return false;") >= 6 &&
                   Contains(performBody, "m_textureManager.Clear();") &&
                   Contains(performBody,
                            "ReloadSkydomeTexture(m_skydomeIndex);") &&
                   Contains(performBody, "RebuildSkydomeMeshes();") &&
                   Contains(performBody, "ResolveDesiredReference();") &&
                   Contains(performBody,
                            "RebuildReferenceObjectMesh();"),
                   true);

        const size_t debugBegin =
            bridgeEngineSource.find(
                "if (kind == \"debug/device-recovery-work\")");
        const size_t snapshotBegin =
            bridgeEngineSource.find(
                "// -------- engine/state/snapshot --------", debugBegin);
        const std::string debugBody =
            debugBegin != std::string::npos && snapshotBegin > debugBegin
                ? bridgeEngineSource.substr(debugBegin,
                                            snapshotBegin - debugBegin)
                : std::string();
        const size_t testHostGuard =
            debugBody.find("if (!m_testHost)");
        const size_t requireEngine =
            debugBody.find("ctx.RequireEngine(", testHostGuard);
        const size_t armHold =
            debugBody.find(
                "SetDeviceRecoveryWorkHoldForTesting(true)",
                requireEngine);
        const size_t releaseHold =
            debugBody.find(
                "SetDeviceRecoveryWorkHoldForTesting(false)",
                armHold);
        const size_t frameDoor =
            debugBody.find("m_engine->PrepareDeviceForFrame();",
                           releaseHold);
        const size_t queryInjection =
            debugBody.find(
                "InjectEndFrameQueryResultForTesting(",
                requireEngine);
        ExpectBool("test-host seam reaches only production recovery doors",
                   testHostGuard != std::string::npos &&
                   requireEngine != std::string::npos &&
                   armHold != std::string::npos &&
                   releaseHold != std::string::npos &&
                   frameDoor != std::string::npos &&
                   queryInjection != std::string::npos &&
                   Contains(debugBody, "action == \"arm\"") &&
                   Contains(debugBody, "action == \"release\"") &&
                   Contains(debugBody,
                            "action == \"inject-query-result\"") &&
                   Contains(debugBody, "action != \"query\"") &&
                   !Contains(debugBody, "ReplayPendingTextureReload") &&
                   !Contains(debugBody, "m_textureReloadApplyCount") &&
                   !Contains(debugBody,
                             "m_particleSystemChangeApplyCount") &&
                   !Contains(debugBody, "m_presentSuspect") &&
                   testHostGuard < requireEngine &&
                   requireEngine < armHold &&
                   armHold < releaseHold &&
                   releaseHold < frameDoor &&
                   frameDoor < queryInjection,
                   true);
        const size_t productionReloadBegin =
            bridgeEngineSource.find(
                "if (kind == \"engine/action/reload-textures\")");
        const size_t productionChangeBegin =
            bridgeEngineSource.find(
                "if (kind == "
                "\"engine/action/on-particle-system-changed\")",
                productionReloadBegin);
        const size_t productionChangeEnd =
            bridgeEngineSource.find(
                "// Advance the preview clock", productionChangeBegin);
        const std::string productionReloadBody =
            productionReloadBegin != std::string::npos &&
            productionChangeBegin > productionReloadBegin
                ? bridgeEngineSource.substr(
                      productionReloadBegin,
                      productionChangeBegin - productionReloadBegin)
                : std::string();
        const std::string productionChangeBody =
            productionChangeBegin != std::string::npos &&
            productionChangeEnd > productionChangeBegin
                ? bridgeEngineSource.substr(
                      productionChangeBegin,
                      productionChangeEnd - productionChangeBegin)
                : std::string();
        ExpectBool("native oracle drives the production request handlers",
                   Contains(productionReloadBody,
                            "m_engine->ReloadTextures();") &&
                   Contains(productionChangeBody,
                            "m_engine->OnParticleSystemChanged("
                            "params.value(\"track\", 0));"),
                   true);

        const size_t shaderReloadBegin =
            bridgeEngineSource.find(
                "if (kind == \"engine/action/reload-shaders\")");
        const size_t shaderReloadEnd =
            bridgeEngineSource.find(
                "if (kind == \"engine/action/reload-textures\")",
                shaderReloadBegin);
        const std::string shaderReloadBody =
            shaderReloadBegin != std::string::npos &&
                    shaderReloadEnd > shaderReloadBegin
                ? bridgeEngineSource.substr(
                      shaderReloadBegin,
                      shaderReloadEnd - shaderReloadBegin)
                : std::string();
        const size_t directShaderCall =
            shaderReloadBody.find("if (!m_engine->ReloadShaders())");
        const size_t directShaderError =
            shaderReloadBody.find(
                "ctx.SendErr(\"shader reload was refused or failed\")",
                directShaderCall);
        const size_t directShaderInvalidate =
            shaderReloadBody.find(
                "m_engine->InvalidateSkydomeListCache();",
                directShaderCall);
        const size_t directShaderSuccess =
            shaderReloadBody.find("ctx.SendOk(json::object());",
                                  directShaderInvalidate);
        ExpectBool("direct shader action propagates false before cache invalidation",
                   directShaderCall != std::string::npos &&
                   directShaderError != std::string::npos &&
                   directShaderInvalidate != std::string::npos &&
                   directShaderSuccess != std::string::npos &&
                   directShaderCall < directShaderError &&
                   directShaderError < directShaderInvalidate &&
                   directShaderInvalidate < directShaderSuccess,
                   true);

        const size_t setLayersBegin =
            bridgeAssetsSource.find(
                "if (kind == \"mods/set-layers\")");
        const size_t setLayersEnd =
            bridgeAssetsSource.find(
                "// -------- textures/browse --------", setLayersBegin);
        const std::string setLayersBody =
            setLayersBegin != std::string::npos &&
                    setLayersEnd > setLayersBegin
                ? bridgeAssetsSource.substr(
                      setLayersBegin, setLayersEnd - setLayersBegin)
                : std::string();
        const size_t layerBlockedGate =
            setLayersBody.find(
                "if (m_engine && m_engine->DeviceCallsBlocked())");
        const size_t layerParse =
            setLayersBody.find("std::vector<std::wstring> paths;");
        const size_t layerMutation =
            setLayersBody.find("m_modManager->SetLayerStack(");
        const size_t layerCacheClear =
            setLayersBody.find(
                "TexturePalette::ClearBridgeThumbCache();",
                layerMutation);
        ExpectBool("set-layers refuses blocked work before roots and caches",
                   layerBlockedGate != std::string::npos &&
                   layerParse != std::string::npos &&
                   layerMutation != std::string::npos &&
                   layerCacheClear != std::string::npos &&
                   Contains(setLayersBody,
                            "\"the rendering device is unavailable; the load order was not changed\"") &&
                   layerBlockedGate < layerParse &&
                   layerParse < layerMutation &&
                   layerMutation < layerCacheClear,
                   true);
        const size_t managerSetLayers =
            modManagerSource.find("bool ModManager::SetLayerStack(");
        const size_t managerResolve =
            modManagerSource.find(
                "modlayers::ResolveLayerStack(", managerSetLayers);
        const std::string managerGateBody =
            managerSetLayers != std::string::npos &&
                    managerResolve > managerSetLayers
                ? modManagerSource.substr(
                      managerSetLayers,
                      managerResolve - managerSetLayers)
                : std::string();
        ExpectBool("ModManager itself refuses before configured-root mutation",
                   Contains(managerGateBody,
                            "m_engine->DeviceCallsBlocked()") &&
                   Contains(managerGateBody, "return false;"),
                   true);

        const size_t replayBegin =
            engineSource.find(
                "bool Engine::ReplayPendingParticleSystemChange()");
        const size_t replayEnd =
            engineSource.find("void Engine::GetViewPort(", replayBegin);
        const std::string replayBody =
            replayBegin != std::string::npos && replayEnd > replayBegin
                ? engineSource.substr(replayBegin, replayEnd - replayBegin)
                : std::string();
        const size_t replayCall =
            replayBody.find("m_deferredParticleSystemChange.Replay(");
        const size_t applyDeferred =
            replayBody.find("ApplyParticleSystemChanged(track);", replayCall);
        const size_t reblockCheck =
            replayBody.find(
                "return DeviceCallsBlocked() || m_presentSuspect;",
                applyDeferred);
        ExpectBool("healthy frame drains production replay after recovery",
                   replayCall != std::string::npos &&
                   applyDeferred != std::string::npos &&
                   reblockCheck != std::string::npos &&
                   replayCall < applyDeferred &&
                   applyDeferred < reblockCheck &&
                   Contains(deferredChangeHeader,
                            "catch (...)\n        {\n"
                            "            Queue(track);\n"
                            "            return false;\n        }") &&
                   Contains(deferredChangeHeader, "if (blocked())") &&
                   Contains(deferredChangeHeader,
                            "Queue(track);\n            return false;"),
                   true);

        const size_t clearBegin =
            engineSource.find("void Engine::Clear()");
        const size_t clearEnd =
            engineSource.find("void Engine::SetOverloadGuard", clearBegin);
        const std::string clearBody =
            clearBegin != std::string::npos && clearEnd > clearBegin
                ? engineSource.substr(clearBegin, clearEnd - clearBegin)
                : std::string();
        ExpectBool("document clear cancels old deferred change",
                   clearBody.find(
                       "++m_particleSystemDocumentEpoch;") !=
                       std::string::npos &&
                   clearBody.find(
                       "m_deferredParticleSystemChange.Reset();") !=
                       std::string::npos &&
                   clearBody.find("m_instances.clear();") !=
                       std::string::npos &&
                   clearBody.find(
                       "++m_particleSystemDocumentEpoch;") <
                       clearBody.find(
                           "m_deferredParticleSystemChange.Reset();") &&
                   clearBody.find(
                       "m_deferredParticleSystemChange.Reset();") <
                       clearBody.find("m_instances.clear();") &&
                   !Contains(clearBody,
                             "m_textureReloadRequestGeneration") &&
                   !Contains(clearBody,
                             "m_textureReloadAppliedGeneration") &&
                   !Contains(clearBody, "m_textureReloadApplyCount"),
                   true);

        const size_t fatalBegin =
            engineSource.find("void Engine::ReportFatalDeviceState(");
        const size_t fatalEnd =
            engineSource.find("void Engine::NotifyPresentResult(", fatalBegin);
        const std::string fatalBody =
            fatalBegin != std::string::npos && fatalEnd > fatalBegin
                ? engineSource.substr(fatalBegin, fatalEnd - fatalBegin)
                : std::string();
        ExpectBool("terminal recovery discards deferred preview work",
                   Contains(fatalBody,
                            "m_deferredParticleSystemChange.Reset();") &&
                   Contains(fatalBody,
                            "m_textureReloadAppliedGeneration = "
                            "m_textureReloadRequestGeneration;"),
                   true);

        ExpectBool("Engine binds the executable deferred-change seam",
                   Contains(engineHeader,
                            "#include \"DeferredParticleSystemChange.h\"") &&
                   Contains(engineHeader,
                            "particlesystemchange::DeferredReplay "
                            "m_deferredParticleSystemChange;") &&
                   Contains(deferredChangeHeader,
                            "bool DeferIfBlocked(bool blocked, int track)") &&
                   Contains(deferredChangeHeader, "bool Take(int& track)") &&
                   Contains(deferredChangeHeader,
                            "bool Pending() const") &&
                   Contains(deferredChangeHeader,
                            "bool Replay(TApply apply, TBlocked blocked)"),
                   true);

        const auto functionHasDeviceGate =
            [](const std::string& source, const char* signature)
            {
                const size_t begin = source.find(signature);
                if (begin == std::string::npos) return false;
                const size_t count =
                    (std::min)(static_cast<size_t>(600),
                               source.size() - begin);
                return source.substr(begin, count).find(
                           "DeviceCallsBlocked()") != std::string::npos;
            };
        const std::initializer_list<const char*> engineGatedDoors = {
            "ParticleSystemInstance* Engine::SpawnParticleSystem(",
            "IDirect3DTexture9* Engine::GetTexture(",
            "void Engine::OnParticleSystemChanged(",
            "void Engine::GetViewPort(",
            "void Engine::SetCamera(",
            "std::vector<int> Engine::GetSupportedMsaaLevels() const",
            "HANDLE Engine::GetSharedTextureHandle() const",
            "void Engine::IssueEndFrameQuery()",
            "int Engine::WaitEndFrameQuery()",
            "LUID Engine::GetAdapterLuid() const",
            "void Engine::SetSceneViewport(",
        };
        for (const char* door : engineGatedDoors)
        {
            const std::string label =
                std::string("external D3D gate covers ") + door;
            ExpectBool(label.c_str(),
                       functionHasDeviceGate(engineSource, door), true);
        }

        const std::initializer_list<const char*> renderGatedDoors = {
            "bool Engine::ReloadShaders()",
            "void Engine::ReloadTextures()",
            "void Engine::DumpParticleDrawStateIfRequested(",
        };
        for (const char* door : renderGatedDoors)
        {
            const std::string label =
                std::string("render-TU D3D gate covers ") + door;
            ExpectBool(label.c_str(),
                       functionHasDeviceGate(renderSource, door), true);
        }
        ExpectBool("Render uses the recovery-aware gate, not a raw flag",
                   functionHasDeviceGate(renderSource,
                                         "bool Engine::Render()") ||
                   Contains(renderSource,
                            "if (!PrepareDeviceForFrame()) return false;"),
                   true);

        const std::initializer_list<const char*> environmentGatedDoors = {
            "bool Engine::SetGroundTexture(",
            "bool Engine::SetGroundSlotCustomPath(",
            "bool Engine::SetGroundSolidColor(",
            "void Engine::SetSkydomeEnvironment(",
            "bool Engine::SetSkydomeSlot(",
            "bool Engine::SetSkydomeCustomPath(",
        };
        for (const char* door : environmentGatedDoors)
        {
            const std::string label =
                std::string("environment D3D gate covers ") + door;
            ExpectBool(label.c_str(),
                       functionHasDeviceGate(environmentSource, door), true);
        }
        ExpectBool("reference upload door is externally gated",
                   functionHasDeviceGate(
                       referenceSource,
                       "void Engine::SetReferenceObject("),
                   true);
        ExpectBool("snapshot capture refuses blocked D3D9 state",
                   Contains(layoutSource,
                            "m_engine->DeviceCallsBlocked()") &&
                   Contains(layoutSource,
                            "CaptureSnapshotPng(") &&
                   Contains(layoutSource,
                            "CaptureSnapshotToFile("),
                   true);
        ExpectBool("ordinary and cheap resets establish reentrancy gate",
                   Contains(resetBody,
                            "m_deviceResetInProgress = true;") &&
                   Contains(resetBody,
                            "m_deviceResetInProgress = false;") &&
                   Contains(resizeResetBody,
                            "m_deviceResetInProgress = true;") &&
                   Contains(resizeResetBody,
                            "m_deviceResetInProgress = false;") &&
                   Contains(recoveryHeader,
                            "return { Outcome::SkipFrame, recovery.observedState,") &&
                   Contains(recoveryHeader, "E_PENDING, false };"),
                   true);
    }

    if (g_failures == 0)
    {
        std::printf("=== device state: ALL PASS ===\n");
        return 0;
    }
    std::printf("=== device state: %d FAILURE(S) ===\n", g_failures);
    return 1;
}
