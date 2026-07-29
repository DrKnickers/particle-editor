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
#include <filesystem>
#include <fstream>
#include <string>

using devicestate::Action;
using devicestate::ClassifyDeviceState;
using devicestate::IsFatalDeviceState;
using devicestate::ShouldCheckDeviceAfterPresent;

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

static std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

static bool Contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
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

    std::printf("=== present-result suspect predicate ===\n");
    ExpectBool("failed composed Present1 result requests a device probe",
               ShouldCheckDeviceAfterPresent(E_FAIL), true);
    ExpectBool("direct Present occlusion requests a device probe",
               ShouldCheckDeviceAfterPresent(S_PRESENT_OCCLUDED), true);
    ExpectBool("direct Present mode change requests a device probe",
               ShouldCheckDeviceAfterPresent(S_PRESENT_MODE_CHANGED), true);
    ExpectBool("S_FALSE no-visual frame does NOT request a device probe",
               ShouldCheckDeviceAfterPresent(S_FALSE), false);
    ExpectBool("D3D_OK does NOT request a device probe",
               ShouldCheckDeviceAfterPresent(D3D_OK), false);

    // The predicate above is not proof that production forwards either
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
        const std::string hostSource =
            ReadSource(repoRoot / "src" / "host" / "HostWindow.cpp");
        const std::string emitterHeader =
            ReadSource(repoRoot / "src" / "EmitterInstance.h");
        const std::string emitterSource =
            ReadSource(repoRoot / "src" / "EmitterInstance.cpp");
        const std::string instanceHeader =
            ReadSource(repoRoot / "src" / "ParticleSystemInstance.h");
        const std::string instanceSource =
            ReadSource(repoRoot / "src" / "ParticleSystemInstance.cpp");

        ExpectBool("production device-reset sources are readable",
                   !engineHeader.empty() && !engineSource.empty() &&
                   !renderSource.empty() && !hostSource.empty() &&
                   !emitterHeader.empty() && !emitterSource.empty() &&
                   !instanceHeader.empty() && !instanceSource.empty(), true);

        ExpectBool("Engine exposes one shared present-result observer",
                   Contains(engineHeader, "void NotifyPresentResult(HRESULT hr);"), true);
        ExpectBool("Engine observer uses the tested suspect predicate",
                   Contains(engineSource,
                            "if (devicestate::ShouldCheckDeviceAfterPresent(hr))") &&
                   Contains(engineSource, "m_presentSuspect = true;"), true);
        ExpectBool("direct D3D9 Present forwards its actual HRESULT",
                   Contains(renderSource, "NotifyPresentResult(presentHr);"), true);
        const size_t compositeResult =
            hostSource.find("const HRESULT compositeHr =");
        const size_t compositeNotify =
            hostSource.find("engine->NotifyPresentResult(compositeHr);", compositeResult);
        const std::string compositeBinding =
            compositeResult != std::string::npos && compositeNotify > compositeResult
                ? hostSource.substr(compositeResult, compositeNotify - compositeResult)
                : std::string();
        ExpectBool("composed Present1 forwards its actual HRESULT",
                   Contains(compositeBinding,
                            "m_compositor->CompositeEngineFrame(") &&
                   compositeNotify != std::string::npos, true);

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

        const size_t resetBegin = engineSource.find("void Engine::Reset()");
        const size_t resetEnd = engineSource.find("bool Engine::ResetForResize()", resetBegin);
        const std::string resetBody =
            resetBegin != std::string::npos && resetEnd > resetBegin
                ? engineSource.substr(resetBegin, resetEnd - resetBegin)
                : std::string();
        const size_t releasePos = resetBody.find("ReleaseInstanceTextures();");
        const size_t cacheLostPos = resetBody.find("m_textureManager.OnLostDevice();");
        const size_t resetPos = resetBody.find("m_pDevice->Reset(");
        const size_t reacquirePos = resetBody.find("ReacquireInstanceTextures();");
        ExpectBool("emitter refs release before cache loss and device Reset",
                   releasePos != std::string::npos &&
                   cacheLostPos != std::string::npos &&
                   resetPos != std::string::npos &&
                   releasePos < cacheLostPos && cacheLostPos < resetPos, true);
        ExpectBool("emitter textures reacquire only after Reset succeeds",
                   resetPos != std::string::npos &&
                   reacquirePos != std::string::npos &&
                   resetPos < reacquirePos, true);

        const std::string resizeResetBody =
            resetEnd != std::string::npos
                ? engineSource.substr(resetEnd)
                : std::string();
        ExpectBool("cheap ResetEx resize preserves live emitter texture refs",
                   !Contains(resizeResetBody, "ReleaseInstanceTextures();") &&
                   !Contains(resizeResetBody, "ReacquireInstanceTextures();"), true);
    }

    if (g_failures == 0)
    {
        std::printf("=== device state: ALL PASS ===\n");
        return 0;
    }
    std::printf("=== device state: %d FAILURE(S) ===\n", g_failures);
    return 1;
}
