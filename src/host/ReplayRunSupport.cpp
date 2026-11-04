#include "ReplayRunSupport.h"
#include "AlphaCompositor.h"
#include "LayoutBroker.h"
#include "rendering/engine.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemIO.h"
#include "effect/UndoStack.h"
#include <fstream>
#include <wincrypt.h>

#pragma comment(lib, "advapi32.lib")

namespace host
{
namespace
{

using nlohmann::json;
using Clock = std::chrono::steady_clock;

json Vector(const D3DXVECTOR3& v)
{
    return {v.x, v.y, v.z};
}
json Vector(const D3DXVECTOR4& v)
{
    return {v.x, v.y, v.z, v.w};
}

} // namespace

bool PumpMessages(ReplayRunContext& context)
{
    if (context.failed()) return false;
    if (Clock::now() - context.start > std::chrono::minutes(10))
        return context.fail(5, "watchdog", "proof exceeded 10 minutes");
    MSG message = {};
    // Bound each drain too: a busy sender cannot defeat the wall-clock bound.
    for (int i = 0; i < 256 && PeekMessage(&message, nullptr, 0, 0, PM_REMOVE); ++i)
    {
        if (message.message == WM_QUIT) return context.fail(5, "message-pump", "WM_QUIT during proof");
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return !context.deps.webDead || context.fail(5, "webview", "web process failed during proof");
}

json CaptureGeometry(ReplayRunContext& context)
{
    int x = 0, y = 0, w = 0, h = 0;
    if (!context.deps.alphaCompositor || !context.deps.layout.GetSceneRect(x, y, w, h)) return nullptr;
    const auto rt   = context.deps.alphaCompositor->RenderTargetSizeForObservation();
    const auto crop = context.deps.alphaCompositor->ReadbackCropForObservation();
    return {
        {"rt", {rt.width, rt.height}}, {"viewport", {x, y, w, h}}, {"crop", {crop.x, crop.y, crop.width, crop.height}}};
}

bool CheckGeometry(ReplayRunContext& context, const char* hook)
{
    if (context.failed()) return false;
    if (!context.geometryLocked) return true;
    return (CaptureGeometry(context) == context.geometry && !context.deps.layout.IsSceneAnimActive()) ||
           context.fail(7, hook, "render-target, viewport or readback crop changed");
}

json FixedProfile(Engine& e)
{
    const auto camera = e.GetCamera();
    json       lights = json::array();
    for (auto type : {Engine::LT_SUN, Engine::LT_FILL1, Engine::LT_FILL2})
    {
        const auto& light = e.GetLight(type);
        lights.push_back({{"diffuse", Vector(light.Diffuse)},
                          {"specular", Vector(light.Specular)},
                          {"position", Vector(light.Position)},
                          {"direction", Vector(light.Direction)}});
    }
    return {{"camera",
             {{"position", Vector(camera.Position)}, {"target", Vector(camera.Target)}, {"up", Vector(camera.Up)}}},
            {"background", e.GetBackground()},
            {"ambient", Vector(e.GetAmbient())},
            {"lights", lights},
            {"wind", Vector(e.GetWind())},
            {"gravity", Vector(e.GetGravity())},
            {"ground", e.GetGround()},
            {"bloom", e.GetBloom()},
            {"bloomStrength", e.GetBloomStrength()},
            {"bloomCutoff", e.GetBloomCutoff()},
            {"bloomSize", e.GetBloomSize()},
            {"heatDebug", e.GetHeatDebug()},
            {"msaa", e.GetCurrentMsaaLevel()},
            {"skydomeSlot", e.GetSkydomeSlot()},
            {"embeddedSkydome", e.IsSkydomeUsingEmbeddedResource()},
            {"primaryDome", e.GetSkydomePrimaryName()},
            {"secondaryDome", e.GetSkydomeSecondaryName()},
            {"primaryDomeGpu", e.SkydomePrimaryHasGpuBuffers()},
            {"secondaryDomeGpu", e.SkydomeSecondaryHasGpuBuffers()},
            {"skydomeMeshGpu", e.SkydomeMeshHasGpuBuffers()}};
}

bool CheckSettings(ReplayRunContext& context)
{
    auto& e = *context.deps.engine;
    return (e.OverloadGuardEnabledForObservation() == context.inForce.overloadGuardEnabled &&
            e.OverloadGuardLimitForObservation() == context.inForce.overloadGuardLimit &&
            e.EstimatedLoadForObservation() == context.inForce.estimatedParticlesPerInstance &&
            FixedProfile(e) == context.profile) ||
           context.fail(4, "settings-observation", "scenario settings or fixed profile changed");
}

bool PrepareScene(ReplayRunContext& context, const ParticleReplayController::Hooks& hooks)
{
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    int        stable = 0, passes = 0;
    uint64_t   previous     = context.deps.engine->ParticleSystemChangeApplyCountForTesting();
    json       observations = json::array();
    while (passes < 8 && Clock::now() < deadline)
    {
        if (!PumpMessages(context)) return false;
        if (context.deps.layout.IsSceneAnimActive())
        {
            if (!hooks.render()) return false;
            Sleep(1);
            continue;
        }
        if (!context.geometryLocked)
        {
            context.geometry = CaptureGeometry(context);
            if (context.geometry.is_null() || context.geometry["rt"][0].get<int>() <= 0 ||
                context.geometry["rt"][1].get<int>() <= 0 || context.geometry["viewport"][2].get<int>() <= 0 ||
                context.geometry["viewport"][3].get<int>() <= 0 || context.geometry["crop"][2].get<int>() <= 0 ||
                context.geometry["crop"][3].get<int>() <= 0)
                return context.fail(12, "preparation", "capture geometry unavailable");
            context.geometryLocked = true;
            context.recordGeometry(context.geometry);
        }
        if (!hooks.admitDevice() || !hooks.render()) return false;
        ++passes;
        const auto applied = context.deps.engine->ParticleSystemChangeApplyCountForTesting();
        const bool pending = context.deps.engine->TextureReloadPendingForTesting() ||
                             context.deps.engine->ParticleSystemChangePendingForObservation();
        observations.push_back({{"applyCount", applied}, {"pending", pending}});
        // Two consecutive unchanged, non-pending passes exclude deferred changes from replay.
        stable   = !pending && applied == previous ? stable + 1 : 0;
        previous = applied;
        if (stable == 2)
        {
            if (Clock::now() >= deadline) break;
            context.preparationLog.push_back({{"passes", observations}, {"settled", true}});
            return true;
        }
    }
    context.preparationLog.push_back({{"passes", observations}, {"settled", false}});
    return context.fail(12, "preparation", "two consecutive settled passes not reached within 8 passes / 10 seconds");
}

bool WaitReady(ReplayRunContext& context)
{
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!context.deps.uiReady || !context.deps.sceneRectSeen)
    {
        if (!PumpMessages(context)) return false;
        if (Clock::now() >= deadline)
            return context.fail(6, "readiness", "app/ready and layout/scene-rect not received in 10 seconds");
        Sleep(1);
    }
    return true;
}

bool LoadDocument(ReplayRunContext& context, const std::wstring& aloPath)
{
    std::string loadError;
    auto        document = LoadParticleSystem(aloPath, &loadError);
    if (!document) return context.fail(3, "load", loadError);
    context.deps.engine->Clear();
    context.deps.particleSystem = std::move(document);
    context.deps.undoStack.Clear();
    context.deps.engine->OnParticleSystemChanged(-1);
    context.deps.engine->ReloadTextures();
    return true;
}

// Hash the PNG bytes; the proof visibility oracle independently decodes pixels.
std::string Sha256(const std::wstring& path)
{
    std::ifstream     input(std::filesystem::path(path), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return "";
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash     = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    BYTE       digest[32] = {};
    DWORD      size       = sizeof(digest);
    const bool ok =
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) return "";
    const char* hex = "0123456789abcdef";
    std::string result;
    for (BYTE byte : digest)
    {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool AtomicWrite(const std::filesystem::path& destination, const std::string& bytes)
{
    auto temporary = destination;
    temporary += L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << bytes;
    file.flush();
    const bool written = file.good();
    file.close();
    return written &&
           MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace host
