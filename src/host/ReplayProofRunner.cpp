#include "ReplayProofRunner.h"
#include "BridgeDispatcher.h"
#include "AlphaCompositor.h"
#include "LayoutBroker.h"
#include "HostRunUtil.h"
#include "StringConv.h"
#include "rendering/engine.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemIO.h"
#include "effect/UndoStack.h"
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <cstdarg>
#include <cstring>
#include "PreviewScenarioJson.h"
#include "host/third_party/nlohmann/json.hpp"
#include "ReplayHostileCrt.h"
#include "simulation/ParticleRandom.h"
#include "gamedata/GameObjectCatalog.h"
#include "gamedata/xml.h"
#include <wincrypt.h>
#include "WicEncode.h"
#include <chrono>
#include <set>
#include <stdexcept>

#pragma comment(lib, "advapi32.lib")

namespace host {
namespace {

using nlohmann::json;
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

json Vector(const D3DXVECTOR3& v) { return {v.x, v.y, v.z}; }
json Vector(const D3DXVECTOR4& v) { return {v.x, v.y, v.z, v.w}; }

// Hash the PNG bytes; the visibility oracle below independently decodes pixels.
std::string Hash(const std::wstring& path)
{
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return "";
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    BYTE digest[32] = {};
    DWORD size = sizeof(digest);
    const bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) return "";
    const char* hex = "0123456789abcdef";
    std::string result;
    for (BYTE byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

struct Pixels
{
    unsigned width = 0, height = 0;
    std::vector<unsigned char> bytes;
};

// WIC lives in WicEncode.cpp: its include order cannot coexist with engine.h.
bool Decode(const std::wstring& path, Pixels& out)
{
    return DecodePngToBgraWic(path, out.width, out.height, out.bytes);
}

// This private manager has no MEGs, mod stack, or engine catalog cache.
struct CatalogFiles : IFileManager
{
    IFile* getFile(const std::string& path) override
    {
        try
        {
            return new PhysicalFile(L"tests\\fixtures\\replay-catalog\\" + Utf8ToWide(path));
        }
        catch (...) { return nullptr; }
    }
};

bool Same(const json& a, const json& b, bool image = true)
{
    for (const char* field : {"frame", "draws", "state", "instances", "particles", "emitters",
                             "guard", "profile", "geometry", "instance", "completedUpdates"})
        if (a.at(field) != b.at(field)) return false;
    return !image || a.at("sha256") == b.at("sha256");
}

} // namespace

struct ReplayProofRunner::Impl
{
    Deps deps;
    std::wstring scenarioPath;
    std::filesystem::path out;
    PreviewScenario scenario, inForce;
    json report = {{"schemaVersion", 1}, {"exitCode", 0}, {"failure", nullptr},
                   {"experiments", json::object()}, {"preparation", json::array()}};
    json geometry, profile, run, baseline;
    std::set<int> checked;
    std::function<bool(int)> selectCapture;
    std::string prefix;
    std::wstring capturePath;
    int frame = -1, exitCode = 0;
    uint64_t resetUpdateCount = 0;
    Clock::time_point start = Clock::now();
    bool geometryLocked = false;

    Impl(Deps dependencies, const std::wstring& path, const std::wstring& directory)
        : deps(std::move(dependencies)), scenarioPath(path), out(directory) {}

    void Log(const char* format, ...)
    {
        char message[2048];
        va_list args;
        va_start(args, format);
        _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
        va_end(args);
        if (deps.log) deps.log(message);
    }

    bool Fail(int code, const char* hook, const std::string& message)
    {
        if (!exitCode)
        {
            exitCode = code;
            report["exitCode"] = code;
            report["failure"] = {{"hook", hook}, {"frame", frame}, {"message", message}};
            Log("[replay-proof] hook=%s frame=%d exit=%d %s\n", hook, frame, code, message.c_str());
        }
        return false;
    }

    bool Pump()
    {
        if (exitCode) return false;
        if (Clock::now() - start > std::chrono::minutes(10)) return Fail(5, "watchdog", "proof exceeded 10 minutes");
        MSG message = {};
        // Bound each drain too: a busy sender cannot defeat the wall-clock bound.
        for (int i = 0; i < 256 && PeekMessage(&message, nullptr, 0, 0, PM_REMOVE); ++i)
        {
            if (message.message == WM_QUIT) return Fail(5, "message-pump", "WM_QUIT during proof");
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        return !deps.webDead || Fail(5, "webview", "web process failed during proof");
    }

    json Geometry()
    {
        int x = 0, y = 0, w = 0, h = 0;
        if (!deps.alphaCompositor || !deps.layout.GetSceneRect(x, y, w, h)) return nullptr;
        const auto rt = deps.alphaCompositor->RenderTargetSizeForObservation();
        const auto crop = deps.alphaCompositor->ReadbackCropForObservation();
        return {{"rt", {rt.width, rt.height}}, {"viewport", {x, y, w, h}},
                {"crop", {crop.x, crop.y, crop.width, crop.height}}};
    }

    bool CheckGeometry(const char* hook)
    {
        if (exitCode) return false;
        if (!geometryLocked) return true;
        return (Geometry() == geometry && !deps.layout.IsSceneAnimActive()) ||
            Fail(7, hook, "render-target, viewport or readback crop changed");
    }

    json Profile()
    {
        auto& e = *deps.engine;
        const auto camera = e.GetCamera();
        json lights = json::array();
        for (auto type : {Engine::LT_SUN, Engine::LT_FILL1, Engine::LT_FILL2})
        {
            const auto& light = e.GetLight(type);
            lights.push_back({{"diffuse", Vector(light.Diffuse)}, {"specular", Vector(light.Specular)},
                {"position", Vector(light.Position)}, {"direction", Vector(light.Direction)}});
        }
        return {{"camera", {{"position", Vector(camera.Position)}, {"target", Vector(camera.Target)},
                              {"up", Vector(camera.Up)}}},
            {"background", e.GetBackground()}, {"ambient", Vector(e.GetAmbient())}, {"lights", lights},
            {"wind", Vector(e.GetWind())}, {"gravity", Vector(e.GetGravity())},
            {"ground", e.GetGround()}, {"bloom", e.GetBloom()}, {"bloomStrength", e.GetBloomStrength()},
            {"bloomCutoff", e.GetBloomCutoff()}, {"bloomSize", e.GetBloomSize()},
            {"heatDebug", e.GetHeatDebug()}, {"msaa", e.GetCurrentMsaaLevel()},
            {"skydomeSlot", e.GetSkydomeSlot()}, {"embeddedSkydome", e.IsSkydomeUsingEmbeddedResource()},
            {"primaryDome", e.GetSkydomePrimaryName()}, {"secondaryDome", e.GetSkydomeSecondaryName()},
            {"primaryDomeGpu", e.SkydomePrimaryHasGpuBuffers()},
            {"secondaryDomeGpu", e.SkydomeSecondaryHasGpuBuffers()},
            {"skydomeMeshGpu", e.SkydomeMeshHasGpuBuffers()}};
    }

    bool CheckSettings()
    {
        auto& e = *deps.engine;
        return (e.OverloadGuardEnabledForObservation() == inForce.overloadGuardEnabled &&
                e.OverloadGuardLimitForObservation() == inForce.overloadGuardLimit &&
                e.EstimatedLoadForObservation() == inForce.estimatedParticlesPerInstance && Profile() == profile) ||
            Fail(4, "settings-observation", "scenario settings or fixed profile changed");
    }

    ParticleReplayController::Hooks Hooks()
    {
        auto hooks = deps.buildHooks(L"");
        const auto admit = hooks.admitDevice;
        hooks.admitDevice = [this, admit]()
        {
            if (!Pump()) return false;
            if (admit()) return true;
            frame = frame < 0 ? 0 : frame + 1;
            return Fail(8, "admitDevice", "device admission failed");
        };
        const auto pin = hooks.pinSettings;
        hooks.pinSettings = [this, pin](const PreviewScenario& value)
        {
            frame = 0;
            inForce = value;
            return pin(value) || Fail(9, "pinSettings", "fixed profile or scenario settings failed");
        };
        const auto clear = hooks.clearInstances;
        hooks.clearInstances = [this, clear]()
        {
            clear();
            resetUpdateCount = deps.engine->CompletedFullUpdateCountForObservation();
        };
        const auto seed = hooks.seed;
        hooks.seed = [this, seed](std::uint32_t value)
        {
            seed(value);
            if (ParticleRandom::DrawCount() != 0) Fail(4, "seed", "draws before armSpawn must be zero");
        };
        const auto freeze = hooks.freezeClock;
        hooks.freezeClock = [this, freeze](float value) { frame = 0; freeze(value); };
        const auto arm = hooks.armSpawn;
        hooks.armSpawn = [this, arm](const PreviewScenario& value)
        {
            return !exitCode && (arm(value) || Fail(9, "armSpawn", "spawn hook failed"));
        };
        const auto step = hooks.stepClock;
        hooks.stepClock = [this, step]() { ++frame; step(); };
        const auto tick = hooks.tickSpawner;
        hooks.tickSpawner = [this, tick]() { return tick() || Fail(9, "tickSpawner", "spawn was refused or tick failed"); };
        const auto update = hooks.update;
        hooks.update = [this, update]()
        {
            if (!update()) return Fail(10, "update", "one completed full Update required");
            if (!CheckSettings()) return false;
            const int particles = deps.engine->GetNumParticles();
            run["particles"].push_back(particles);
            if (particles > run["peak"].get<int>()) { run["peak"] = particles; run["peakFrame"] = frame; }
            if (deps.engine->TakeSpawnRefusal(nullptr))
                run["estimateRefusals"] = run["estimateRefusals"].get<int>() + 1;
            if (deps.engine->IsSpawnOverloadActive())
                run["runtimeSuppressionUpdates"] = run["runtimeSuppressionUpdates"].get<int>() + 1;
            return true;
        };
        const auto render = hooks.render;
        hooks.render = [this, render]()
        {
            if (!CheckGeometry("render-before")) return false;
            if (!render()) return Fail(11, "render", "render, query or composition failed");
            return CheckGeometry("render-after");
        };
        hooks.readback = [this]()
        {
            if (!selectCapture || !selectCapture(frame)) return true;
            if (!CheckGeometry("readback-before")) return false;
            capturePath = (out / (prefix + "-" + std::to_string(frame) + ".png")).wstring();
            // BuildReplayHooks remains the authoritative readback wiring. Each selected
            // frame gets a new closure, so no experiment overwrites another's evidence.
            auto readback = deps.buildHooks(capturePath).readback;
            if (!readback || !readback()) return Fail(13, "readback", "PNG capture failed");
            return CheckGeometry("readback-after");
        };
        return hooks;
    }

    void BeginRun(const std::string& name, std::function<bool(int)> capture)
    {
        frame = -1;
        prefix = name;
        selectCapture = std::move(capture);
        capturePath.clear();
        run = {{"frames", json::array()}, {"particles", json::array()}, {"peak", 0}, {"peakFrame", 0},
               {"estimateRefusals", 0}, {"runtimeSuppressionUpdates", 0}};
    }

    json Sample(bool captured)
    {
        auto& e = *deps.engine;
        Engine::ReplayInstanceObservation instance = {};
        const bool hasInstance = e.GetReplayInstanceObservation(instance);
        if (e.GetNumInstances() > 1 || hasInstance != (e.GetNumInstances() == 1))
            Fail(4, "instance-observation", "instance observation does not match the single-spawn scene");
        json value = {{"frame", frame}, {"draws", ParticleRandom::DrawCount()}, {"state", ParticleRandom::StateValue()},
            {"instances", e.GetNumInstances()}, {"particles", e.GetNumParticles()}, {"emitters", e.GetNumEmitters()},
            {"guard", {{"enabled", e.OverloadGuardEnabledForObservation()}, {"limit", e.OverloadGuardLimitForObservation()},
                       {"estimate", e.EstimatedLoadForObservation()}}}, {"profile", Profile()}, {"geometry", Geometry()},
            {"instance", hasInstance ? json{{"position", Vector(instance.position)}, {"age", instance.age}} : json(nullptr)},
            {"completedUpdates", e.CompletedFullUpdateCountForObservation() - resetUpdateCount},
            {"png", captured ? WideToUtf8(capturePath) : ""}, {"sha256", captured ? Hash(capturePath) : ""}};
        if (captured && value["sha256"] == "") Fail(13, "hash", "captured PNG could not be hashed");
        return value;
    }

    bool Prepare(const ParticleReplayController::Hooks& hooks)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        int stable = 0, passes = 0;
        uint64_t previous = deps.engine->ParticleSystemChangeApplyCountForTesting();
        json observations = json::array();
        while (passes < 8 && Clock::now() < deadline)
        {
            if (!Pump()) return false;
            if (deps.layout.IsSceneAnimActive())
            {
                if (!hooks.render()) return false;
                Sleep(1);
                continue;
            }
            if (!geometryLocked)
            {
                geometry = Geometry();
                if (geometry.is_null() || geometry["rt"][0].get<int>() <= 0 ||
                    geometry["rt"][1].get<int>() <= 0 || geometry["viewport"][2].get<int>() <= 0 ||
                    geometry["viewport"][3].get<int>() <= 0 || geometry["crop"][2].get<int>() <= 0 ||
                    geometry["crop"][3].get<int>() <= 0)
                    return Fail(12, "preparation", "capture geometry unavailable");
                geometryLocked = true;
                report["geometry"] = geometry;
            }
            if (!hooks.admitDevice() || !hooks.render()) return false;
            ++passes;
            const auto applied = deps.engine->ParticleSystemChangeApplyCountForTesting();
            const bool pending = deps.engine->TextureReloadPendingForTesting() ||
                deps.engine->ParticleSystemChangePendingForObservation();
            observations.push_back({{"applyCount", applied}, {"pending", pending}});
            stable = !pending && applied == previous ? stable + 1 : 0;
            previous = applied;
            if (stable == 2)
            {
                if (Clock::now() >= deadline) break;
                report["preparation"].push_back({{"passes", observations}, {"settled", true}});
                return true;
            }
        }
        report["preparation"].push_back({{"passes", observations}, {"settled", false}});
        return Fail(12, "preparation", "two consecutive settled passes not reached within 8 passes / 10 seconds");
    }

    bool Sequential(const PreviewScenario& value, const std::string& name, int stride, json& result)
    {
        BeginRun(name, [this, stride](int f) { return checked.count(f) && f % stride == 0; });
        auto hooks = Hooks();
        ParticleReplayController controller(value, hooks);
        if (!controller.Reset() || !hooks.render() || !hooks.readback()) return false;
        run["frames"].push_back(Sample(selectCapture(0)));
        for (int f = 1; f <= value.maxFrames; ++f)
        {
            if (!controller.Advance(true)) return false;
            if (checked.count(f)) run["frames"].push_back(Sample(selectCapture(f)));
        }
        result = run;
        return !exitCode;
    }

    const json& Baseline(int f) const
    {
        for (const auto& sample : baseline.at("frames")) if (sample.at("frame") == f) return sample;
        throw std::runtime_error("baseline frame missing");
    }

    bool Compare(const json& result, const char* experiment, bool requireImage = true)
    {
        for (const auto& sample : result.at("frames"))
        {
            frame = sample.at("frame").get<int>();
            if (!Same(Baseline(frame), sample, requireImage && sample.at("sha256") != ""))
                return Fail(4, experiment, "checked frame differs from E1");
        }
        return !exitCode;
    }

    bool Hostile(json& counts)
    {
        CatalogFiles files;
        try
        {
            IFile* file = files.getFile("Data\\XML\\ReplayObjects.xml");
            if (!file) return Fail(14, "hostile-xml", "fixture XML unavailable");
            XMLTree xml;
            try { xml.parse(file); }
            catch (...) { file->Release(); throw; }
            file->Release();
            if (!xml.getRoot()) return Fail(14, "hostile-xml", "XML parse produced no root");
            counts["xmlParses"] = counts["xmlParses"].get<int>() + 1;
            GameObjectCatalog catalog;
            if (!BuildGameObjectCatalog(files, catalog) || catalog.objects.size() != 1 ||
                catalog.objects[0].name != "ReplayObject")
                return Fail(14, "hostile-catalog", "fresh synchronous fixture catalog must contain one object");
            counts["catalogBuilds"] = counts["catalogBuilds"].get<int>() + 1;
            counts["catalogObjects"] = counts["catalogObjects"].get<int>() + static_cast<int>(catalog.objects.size());
            uint64_t calls = 0;
            if (!RunReplayHostileCrt(37u + static_cast<uint32_t>(frame), calls) || !calls)
                return Fail(14, "hostile-crt", "CRT helper did not run");
            counts["crtCalls"] = counts["crtCalls"].get<uint64_t>() + calls;
            return true;
        }
        catch (...) { return Fail(14, "hostile-work", "fixture XML/catalog work failed"); }
    }

    bool Seeks(const PreviewScenario& value, const std::string& name, bool intermediate,
               json& result, bool hostile = false)
    {
        result = {{"frames", json::array()}};
        if (hostile)
        {
            result["xmlParses"] = 0;
            result["catalogBuilds"] = 0;
            result["catalogObjects"] = 0;
            result["crtCalls"] = 0;
        }
        for (int f : checked)
        {
            frame = f;
            if (hostile && !Hostile(result)) return false;
            BeginRun(name + "-seek" + std::to_string(f), [f](int current) { return current == f; });
            ParticleReplayController controller(value, Hooks());
            if (!controller.SeekTo(f, intermediate)) return false;
            result["frames"].push_back(Sample(true));
        }
        return !exitCode;
    }

    bool Difference(const json& a, const json& b, uint64_t& count)
    {
        if (!CheckGeometry("decode-before")) return false;
        Pixels first, second;
        if (!Decode(Utf8ToWide(a.at("png").get<std::string>()), first) ||
            !Decode(Utf8ToWide(b.at("png").get<std::string>()), second))
            return Fail(13, "decode", "PNG decode failed");
        if (first.width != second.width || first.height != second.height)
            return Fail(7, "decode", "decoded PNG dimensions differ");
        count = 0;
        for (size_t i = 0; i < first.bytes.size(); i += 4)
            if (memcmp(first.bytes.data() + i, second.bytes.data() + i, 4) != 0) ++count;
        return CheckGeometry("decode-after");
    }

    bool Experiments()
    {
        auto& experiments = report["experiments"];
        if (!Sequential(scenario, "E1", 1, baseline)) return false;
        experiments["E1"] = baseline;
        const auto& zero = Baseline(0);
        const auto& one = Baseline(1);
        if (zero["instances"] != 1 || one["instances"] != 1 ||
            one["draws"].get<uint64_t>() == 0)
        {
            frame = zero["instances"] != 1 ? 0 : 1;
            return Fail(4, "positive-oracle", "frame 0 spawn / frame 1 RNG draws required");
        }
        // Delayed emitters need time to start; use the first captured checked frame.
        const json* firstParticle = nullptr;
        for (const auto& sample : baseline["frames"])
            if (sample["particles"].get<int>() > 0) { firstParticle = &sample; break; }
        if (!firstParticle || (*firstParticle)["frame"].get<int>() > 60)
        {
            frame = 60;
            return Fail(4, "positive-oracle", "nonzero particles at a checked frame by frame 60 required");
        }
        const int firstParticleFrame = (*firstParticle)["frame"].get<int>();
        report["positive"]["firstParticleFrame"] = firstParticleFrame;
        for (const auto* sample : {&zero, &one})
        {
            const int f = (*sample)["frame"].get<int>();
            frame = f;
            const float age = (*sample)["instance"]["age"].get<float>();
            const D3DXVECTOR3 expected = scenario.spawnPosition + scenario.spawnVelocity * (static_cast<float>(f) / 60.0f);
            const auto position = (*sample)["instance"]["position"];
            if (fabsf(age - static_cast<float>(f) / 60.0f) > 1.0e-7f ||
                fabsf(position[0].get<float>() - expected.x) > 1.0e-5f ||
                fabsf(position[1].get<float>() - expected.y) > 1.0e-5f ||
                fabsf(position[2].get<float>() - expected.z) > 1.0e-5f)
                return Fail(4, "instance-path", "frame 0/1 age or spawn position plus velocity/60 differs");
        }

        const int peak = baseline["peakFrame"].get<int>();
        BeginRun("peak", [peak](int f) { return f == peak; });
        ParticleReplayController peakController(scenario, Hooks());
        if (!peakController.SeekTo(peak, true)) return false;
        const json peakCapture = Sample(true);
        if (peakCapture["particles"] != baseline["peak"])
            return Fail(4, "peak-capture", "replayed peak population differs from E1");
        uint64_t firstDifference = 0, peakDifference = 0;
        const json empty = report["positive"]["empty"];
        if (!Difference(empty, *firstParticle, firstDifference) || !Difference(empty, peakCapture, peakDifference)) return false;
        report["positive"]["peakCapture"] = peakCapture;
        report["positive"]["firstParticleDifferentPixels"] = firstDifference;
        report["positive"]["peakDifferentPixels"] = peakDifference;
        if (!firstDifference || !peakDifference)
        {
            frame = !firstDifference ? firstParticleFrame : peak;
            return Fail(4, "pixels", "particles did not change empty-scene pixels");
        }

        json result;
        if (!Seeks(scenario, "E2", true, result)) return false;
        experiments["E2"] = result;
        if (!Compare(result, "E2")) return false;
        if (!Sequential(scenario, "E3", 1, result)) return false;
        experiments["E3"] = result;
        if (!Compare(result, "E3")) return false;
        if (!Seeks(scenario, "E4", true, result, true)) return false;
        experiments["E4"] = result;
        if (!Compare(result, "E4")) return false;

        frame = 0;
        if (!deps.particleSystem || deps.particleSystem->getEmitters().empty() || !deps.dispatchSync)
            return Fail(4, "E5-edit", "document or dispatcher unavailable");
        const auto original = UndoStack::Serialize(*deps.particleSystem);
        const float oldLifetime = deps.particleSystem->getEmitters()[0]->lifetime;
        const json edit = json::parse(deps.dispatchSync(json{
            {"type", "req"}, {"id", "replay-edit"}, {"kind", "emitters/set-properties"},
            {"params", {{"id", 0}, {"patch", {{"lifetime", oldLifetime + 1.0f}}}}}}.dump()));
        experiments["E5"] = {{"editResponse", edit}};
        if (!edit.value("ok", false) || !edit.contains("data") ||
            !edit["data"].contains("skipped") || !edit["data"]["skipped"].empty() ||
            !deps.particleSystem || UndoStack::Serialize(*deps.particleSystem) == original)
            return Fail(4, "E5-edit", "edit response or changed document oracle failed");
        experiments["E5"]["documentChanged"] = true;
        const json undo = json::parse(deps.dispatchSync(json{
            {"type", "req"}, {"id", "replay-undo"}, {"kind", "undo/perform"},
            {"params", {{"direction", "undo"}}}}.dump()));
        experiments["E5"]["undoResponse"] = undo;
        if (!undo.value("ok", false) || !undo.contains("data") || !undo["data"].value("applied", false) ||
            !deps.particleSystem || UndoStack::Serialize(*deps.particleSystem) != original)
            return Fail(4, "E5-undo", "undo must apply and restore serialized document");
        experiments["E5"]["documentRestored"] = true;
        auto hooks = Hooks();
        if (!Prepare(hooks) || !Seeks(scenario, "E5", true, result)) return false;
        experiments["E5"]["frames"] = result["frames"];
        if (!Compare(result, "E5")) return false;

        auto newSeed = scenario;
        ++newSeed.seed;
        if (!Sequential(newSeed, "E6-new-seed", 1, result)) return false;
        experiments["E6"] = {{"newSeed", newSeed.seed}, {"newSeedRun", result}};
        bool changed = false;
        for (const auto& sample : result["frames"])
            if (sample["frame"] == 1) changed = sample["state"] != one["state"];
        if (!changed)
        {
            frame = 1;
            return Fail(4, "E6-seed", "new seed did not change frame 1 RNG state");
        }
        if (!Seeks(scenario, "E6-restored", true, result)) return false;
        experiments["E6"]["frames"] = result["frames"];
        if (!Compare(result, "E6")) return false;

        experiments["E7"] = {{"runs", json::array()}};
        for (int stride : {1, 2, 3, 4})
        {
            if (!Sequential(scenario, "E7-stride" + std::to_string(stride), stride, result)) return false;
            result["stride"] = stride;
            experiments["E7"]["runs"].push_back(result);
            if (!Compare(result, "E7")) return false;
        }

        if (!Seeks(scenario, "E8", false, result)) return false;
        bool equivalent = true;
        for (const auto& sample : result["frames"])
            equivalent = Same(Baseline(sample["frame"].get<int>()), sample) && equivalent;
        result["equivalent"] = equivalent;
        experiments["E8"] = result;

        const auto name = std::filesystem::path(scenario.aloPath).stem().wstring();
        const bool guard = name == L"replay-guard";
        experiments["E9"] = {{"applicable", guard}};
        if (guard)
        {
            auto high = scenario;
            high.overloadGuardEnabled = true;
            high.overloadGuardLimit = 10000;
            high.estimatedParticlesPerInstance = 0;
            auto low = high;
            low.overloadGuardLimit = 1000;
            json highRun, lowRun, lowRepeat, highAgain;
            if (!Sequential(high, "E9-high", 1, highRun) || !Sequential(low, "E9-low", 1, lowRun) ||
                !Sequential(low, "E9-low-repeat", 1, lowRepeat) || !Sequential(high, "E9-high-again", 1, highAgain)) return false;
            experiments["E9"] = {{"applicable", true}, {"high", highRun}, {"low", lowRun},
                {"lowRepeat", lowRepeat}, {"highAgain", highAgain}};
            if (highRun["peak"].get<int>() <= 1000 || lowRun["runtimeSuppressionUpdates"].get<int>() <= 0 ||
                lowRun["frames"].back()["draws"] == highRun["frames"].back()["draws"])
                return Fail(4, "E9-guard", "high peak, low suppression or draw divergence missing");
            for (const auto* r : {&highRun, &lowRun, &lowRepeat, &highAgain})
                if ((*r)["estimateRefusals"] != 0) return Fail(4, "E9-estimate", "estimate refusal must stay zero");
            for (size_t i = 0; i < highRun["frames"].size(); ++i)
            {
                frame = highRun["frames"][i]["frame"].get<int>();
                if (!Same(highRun["frames"][i], highAgain["frames"][i]) ||
                    !Same(lowRun["frames"][i], lowRepeat["frames"][i]))
                    return Fail(4, "E9-repeat", "low repeat or high restoration differed");
            }
            if (highRun["particles"] != highAgain["particles"] || lowRun["particles"] != lowRepeat["particles"])
                return Fail(4, "E9-particles", "per-update particle counts differed");
        }

        experiments["E10"] = {{"timings", json::array()}};
        for (int f : checked)
        {
            for (bool intermediate : {true, false})
            {
                std::vector<double> times;
                for (int repetition = 0; repetition < 5; ++repetition)
                {
                    BeginRun("E10", [](int) { return false; });
                    ParticleReplayController controller(scenario, Hooks());
                    const auto before = Clock::now();
                    if (!controller.SeekTo(f, intermediate)) return false;
                    times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - before).count());
                }
                std::sort(times.begin(), times.end());
                experiments["E10"]["timings"].push_back({{"frame", f}, {"renderIntermediate", intermediate},
                    {"samplesMs", times}, {"medianMs", times[2]}, {"maximumMs", times.back()}});
            }
        }
        const bool zeroVelocity = scenario.spawnVelocity.x == 0 && scenario.spawnVelocity.y == 0 && scenario.spawnVelocity.z == 0;
        const bool timedRequired = (name == L"singleton-emitter" && zeroVelocity) || name == L"bump-asteroids";
        bool accepted = true;
        if (timedRequired)
        {
            accepted = false;
            for (const auto& timing : experiments["E10"]["timings"])
                if (timing["frame"] == 300 && timing["renderIntermediate"] == true)
                    accepted = timing["medianMs"].get<double>() <= 250.0;
        }
        report["acceptance"] = {{"latencyRequired", timedRequired}, {"thresholdMs", 250}, {"accepted", accepted}};

        const bool weather = name == L"replay-weather";
        experiments["E11"] = {{"applicable", weather}};
        if (weather)
        {
            result = {{"frames", json::array()}};
            for (int f : checked)
            {
                auto camera = deps.engine->GetCamera();
                camera.Position += D3DXVECTOR3(100, 0, 0);
                deps.engine->SetCamera(camera);
                BeginRun("E11-seek" + std::to_string(f), [f](int current) { return current == f; });
                ParticleReplayController controller(scenario, Hooks());
                if (!controller.SeekTo(f, true)) return false;
                result["frames"].push_back(Sample(true));
            }
            experiments["E11"]["frames"] = result["frames"];
            if (!Compare(result, "E11")) return false;
        }
        return !exitCode;
    }

    bool Execute()
    {
        std::ifstream input(std::filesystem::path(scenarioPath), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::string error = input.bad() || !input.is_open()
            ? "scenario file could not be read" : ParsePreviewScenario(text, scenario);
        if (!error.empty()) return Fail(2, "scenario", error);
        report["scenario"] = json::parse(SerializePreviewScenario(scenario));
        for (int f : {0, 1, 2, 15, 60, 300, scenario.maxFrames}) if (f <= scenario.maxFrames) checked.insert(f);
        report["checkedFrames"] = checked;
        report["limits"] = "one spawner-owned instance; fixed golden camera/environment; default wind/gravity";
        if (!deps.engine) return Fail(8, "admitDevice", "engine unavailable");
        Log("[particle-rng] mode=replay-proof algorithm=legacy-msvc-v%u seed=0x%08X\n",
            scenario.algorithmVersion, scenario.seed);
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!deps.uiReady || !deps.sceneRectSeen)
        {
            if (!Pump()) return false;
            if (Clock::now() >= deadline) return Fail(6, "readiness", "app/ready and layout/scene-rect not received in 10 seconds");
            Sleep(1);
        }
        std::string loadError;
        auto document = LoadParticleSystem(scenario.aloPath, &loadError);
        if (!document) return Fail(3, "load", loadError);
        deps.engine->Clear();
        deps.particleSystem = std::move(document);
        deps.undoStack.Clear();
        deps.engine->OnParticleSystemChanged(-1);
        deps.engine->ReloadTextures();
        BeginRun("empty", [](int) { return true; });
        auto hooks = Hooks();
        if (!hooks.admitDevice() || !hooks.pinSettings(scenario)) return false;
        hooks.clearInstances();
        hooks.seed(scenario.seed);
        hooks.freezeClock(0);
        profile = Profile();
        report["profile"] = profile;
        if (!Prepare(hooks)) return false;
        const uint64_t before = deps.engine->CompletedFullUpdateCountForObservation();
        if (!CheckGeometry("resize-before")) return false;
        deps.settleResize();
        const uint64_t after = deps.engine->CompletedFullUpdateCountForObservation();
        report["resize"] = {{"before", before}, {"after", after}, {"unchanged", before == after}};
        if (before != after) return Fail(4, "resize-update-count", "SettleResize caused an incidental Update");
        if (!CheckGeometry("resize-after") || !hooks.render() || !hooks.readback()) return false;
        report["positive"] = {{"empty", {{"png", WideToUtf8(capturePath)}, {"sha256", Hash(capturePath)}}}};
        if (report["positive"]["empty"]["sha256"] == "") return Fail(13, "hash-empty", "empty-scene PNG unavailable");
        return Experiments();
    }

    int Run()
    {
        try
        {
            std::error_code error;
            std::filesystem::create_directories(out, error);
            if (error) Fail(13, "report-directory", error.message());
            else Execute();
        }
        catch (const std::exception& e) { Fail(4, "exception", e.what()); }
        catch (...) { Fail(4, "exception", "unhandled engine or IO exception"); }
        const auto& ignored = deps.dispatcher.IgnoredReplayProofSettingsPushes();
        report["ignoredSettingsPushes"] = {{"overloadGuard", ignored.overloadGuard},
            {"msaaLevel", ignored.msaaLevel}, {"estimatedLoad", ignored.estimatedLoad}};
        report["wallMs"] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto temporary = out / L"report.json.tmp";
        const auto destination = out / L"report.json";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << report.dump(2);
        file.flush();
        const bool written = file.good();
        file.close();
        if (!written || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            Fail(13, "report-write", "atomic report write failed");
            return exitCode;
        }
        Log("[replay-proof] result=%s exit=%d frame=%d\n", exitCode ? "FAIL" : "PASS", exitCode, frame);
        return exitCode;
    }
};

ReplayProofRunner::ReplayProofRunner(Deps deps, const std::wstring& scenarioPath,
                                   const std::wstring& outputDirectory)
    : m_impl(std::make_unique<Impl>(std::move(deps), scenarioPath, outputDirectory)) {}

ReplayProofRunner::~ReplayProofRunner() = default;
int ReplayProofRunner::Run() { return m_impl->Run(); }
bool ReplayProofRunner::BeforeRender() { return m_impl->CheckGeometry("composed-render-before"); }
bool ReplayProofRunner::AfterRender(bool succeeded)
{
    if (!m_impl->CheckGeometry("composed-render-after")) return false;
    // Startup callbacks can render before geometry is established. Once locked,
    // incidental resize/message renders have the same success bar as hook renders.
    if (!succeeded && m_impl->geometryLocked)
        return m_impl->Fail(11, "composed-render", "incidental render/query/composition failed");
    return succeeded;
}

} // namespace host
