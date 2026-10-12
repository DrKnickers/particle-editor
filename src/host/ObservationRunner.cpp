#include "ObservationRunner.h"
#include "ObservationSampleJson.h"
#include "ObservationSpecJson.h"
#include "BridgeDispatcher.h"
#include "StringConv.h"
#include "effect/ParticleAuthoring.h"
#include "rendering/engine.h"
#include "simulation/ObservationLifecycle.h"
#include "simulation/ParticleRandom.h"
#include "version.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <wincrypt.h>

namespace host {
namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

json OptionalFrame(const std::optional<int>& frame) { return frame ? json(*frame) : json(nullptr); }

// The adapter description is in the ANSI code page, not UTF-8.
std::string AnsiToUtf8(const char* text)
{
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 1) return "";
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), length);
    wide.resize(static_cast<size_t>(length) - 1);
    return WideToUtf8(wide);
}

json RenderProfile(const Engine& engine)
{
    const unsigned long long version = engine.GetAdapterDriverVersion();
    char driver[96];
    sprintf_s(driver, sizeof(driver), "%llu.%llu.%llu.%llu", version >> 48, (version >> 32) & 0xFFFF,
        (version >> 16) & 0xFFFF, version & 0xFFFF);
    return {{"adapter", {{"description", AnsiToUtf8(engine.GetAdapterDescription())},
            {"vendorId", engine.GetAdapterVendorId()}, {"deviceId", engine.GetAdapterDeviceId()},
            {"driverVersion", driver}}},
        {"displayFormat", static_cast<int>(engine.GetCreationDisplayFormat())},
        {"depthStencilFormat", static_cast<int>(engine.GetCreationDepthStencilFormat())},
        {"multisample", {{"type", static_cast<int>(engine.GetCreationMultiSampleType())},
            {"quality", engine.GetCreationMultiSampleQuality()}}},
        {"vertexProcessing", engine.GetVertexProcessingMode()}};
}

json Paths(const std::vector<std::wstring>& paths)
{
    json result = json::array();
    for (const auto& path : paths) result.push_back(WideToUtf8(std::filesystem::path(path).generic_wstring()));
    return result;
}

// Hash the canonical ordered mod-path JSON, including the empty unmodded list.
std::string HashBytes(const std::string& bytes)
{
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

std::string FramePath(const char* directory, const char* pattern, int frame)
{
    char name[64];
    sprintf_s(name, sizeof(name), pattern, frame);
    return std::string(directory) + "/" + name;
}

} // namespace

struct ObservationRunner::Impl
{
    ReplayRunDeps deps;
    Deps observation;
    std::wstring specPath;
    std::filesystem::path out;
    ObservationSpec spec;
    json capture = {{"schemaVersion", 1}, {"tool", PE_VERSION_STR}, {"lifecycle", {{"status", "failed"}}},
        {"preparation", json::array()}, {"frames", json::array()},
        {"stride", json::array()}, {"playback", json::array()}};
    ReplayRunContext context;
    std::vector<ObservationSample> samples;
    TelemetryByteBudget telemetryBudget{0};
    size_t emitterCount = 0;
    std::map<int, SelectedFrame> selected;
    std::map<int, int> playback;
    std::vector<std::filesystem::path> files, directories;
    std::uint64_t runtimeBaseline = 0, hardBaseline = 0, indexBaseline = 0, bytes = 0;
    std::uint64_t resetUpdateCount = 0;
    int frame = -1, exitCode = 0, pass = 0;
    bool accepted = false, validated = false;
    double passMs[2] = {};
    Clock::time_point passStart;
    bool passActive = false;

    Impl(ReplayRunDeps dependencies, Deps extra, const std::wstring& path, const std::wstring& directory)
        : deps(std::move(dependencies)), observation(std::move(extra)), specPath(path), out(directory),
          context{deps,
              [this](int code, const char* hook, const std::string& message) { return Fail(code, hook, message); },
              [this]() { return exitCode != 0; }, capture["preparation"], {}, {}, {}, false, Clock::now(),
              [this](const json& locked) { capture["geometry"] = locked; }} {}

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
            capture["lifecycle"]["status"] = "failed";
            capture["failure"] = {{"hook", hook}, {"frame", frame}, {"exit", code}, {"message", message}};
            Log("[observe] hook=%s frame=%d exit=%d %s\n", hook, frame, code, message.c_str());
            fprintf(stderr, "[observe] hook=%s frame=%d exit=%d %s\n", hook, frame, code, message.c_str());
        }
        return false;
    }

    void BeginPass(int value)
    {
        pass = value;
        frame = -1;
        passStart = Clock::now();
        passActive = true;
        Log("[observe] pass=%d start\n", pass);
    }

    void EndPass()
    {
        if (!passActive) return;
        passActive = false;
        passMs[pass - 1] = std::chrono::duration<double, std::milli>(Clock::now() - passStart).count();
        Log("[observe] pass=%d end frame=%d wallMs=%.3f\n", pass, frame, passMs[pass - 1]);
    }

    bool AcceptDirectory()
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(out, error);
        if (error && error != std::errc::no_such_file_or_directory)
            return Fail(13, "output-directory", error.message());
        if (std::filesystem::exists(status) &&
            (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status) ||
             !std::filesystem::is_empty(out, error)))
            return Fail(13, "output-directory", "output directory must be absent or empty");
        if (error && error != std::errc::no_such_file_or_directory)
            return Fail(13, "output-directory", error.message());
        error.clear();
        std::filesystem::create_directories(out, error);
        if (error) return Fail(13, "output-directory", error.message());
        accepted = true;
        return true;
    }

    bool Directory(const char* relative)
    {
        if (context.failed()) return false;
        const auto path = out / relative;
        std::error_code error;
        if (!std::filesystem::create_directory(path, error))
            return Fail(13, "directory", error ? error.message() : "output subdirectory already exists");
        directories.push_back(path);
        return true;
    }

    void CleanQuotaOutputs()
    {
        // Never recurse: only paths this run created in the accepted empty directory.
        std::error_code error;
        for (const auto& path : files) std::filesystem::remove(path, error);
        for (auto it = directories.rbegin(); it != directories.rend(); ++it)
            if (it->filename() != L"logs") std::filesystem::remove(*it, error);
        files.clear();
        bytes = 0;
        capture["frames"] = json::array();
        capture["stride"] = json::array();
        capture["playback"] = json::array();
    }

    bool Account(const std::filesystem::path& path)
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) return Fail(13, "file-size", error.message());
        bytes += size;
        if (bytes <= spec.output.maxBytes) return true;
        CleanQuotaOutputs();
        return Fail(16, "quota", "actual native output bytes exceeded maxBytes");
    }

    bool Write(const char* relative, const std::string& text)
    {
        if (context.failed()) return false;
        const auto path = out / relative;
        files.push_back(path);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
        file.flush();
        const bool ok = file.good();
        file.close();
        if (!Account(path)) return false;
        return ok || Fail(13, "write", "output file write failed");
    }

    bool Sample()
    {
        if (context.failed() || !CheckGeometry(context, "sample")) return false;
        ObservationSample sample{frame, deps.engine->GetNumInstances(), deps.engine->GetNumEmitters(),
            deps.engine->GetNumParticles(), ParticleRandom::DrawCount(), ParticleRandom::StateValue(),
            deps.engine->RuntimeGuardRefusalCountForObservation() - runtimeBaseline,
            deps.engine->HardGuardRefusalCountForObservation() - hardBaseline,
            deps.engine->IndexCapRefusalCountForObservation() - indexBaseline,
            deps.engine->IsSpawnOverloadActive(), observation.spawnerIdle(),
            std::vector<int>(emitterCount, 0)};
        deps.engine->PerEmitterParticleCounts(sample.perEmitter);
        if (sample.perEmitter.size() != emitterCount)
            return Fail(4, "per-emitter", "an emitter instance has no authored emitter");
        if (!PerEmitterSumMatches(sample))
            return Fail(4, "per-emitter", "per-emitter counts do not sum to the particle count");
        if (pass == 1)
        {
            // Count the line before retaining the sample: memory stays within the budget.
            if (!telemetryBudget.Add(ObservationSampleLine(sample).size()))
                return Fail(16, "quota-telemetry", "telemetry bytes exceeded maxBytes at frame " + std::to_string(frame));
            samples.push_back(std::move(sample));
        }
        else if (pass == 2 && (frame < 0 || static_cast<size_t>(frame) >= samples.size() ||
                 ObservationSampleJson(sample) != ObservationSampleJson(samples[frame])))
            return Fail(15, "determinism", "pass 2 sample differs from pass 1 at frame " + std::to_string(frame));
        return true;
    }

    bool WantsCapture(int value) const { return selected.count(value) || playback.count(value); }

    bool Readback(const std::string& relative, std::string* hash = nullptr)
    {
        if (context.failed() || !CheckGeometry(context, "readback-before")) return false;
        const auto path = out / relative;
        files.push_back(path);
        // Fresh closure per output path retains the proof's authoritative alpha readback.
        const auto readback = deps.buildHooks(path.wstring()).readback;
        Log("[observe] readback frame=%d png=%s\n", frame, relative.c_str());
        if (!readback || !readback())
        {
            std::error_code error;
            if (std::filesystem::exists(path, error) && !Account(path)) return false;
            return Fail(13, "readback", "PNG capture failed");
        }
        if (!Account(path) || !CheckGeometry(context, "readback-after")) return false;
        if (hash)
        {
            *hash = Sha256(path.wstring());
            if (hash->empty()) return Fail(13, "hash", "PNG hash failed");
        }
        return true;
    }

    ParticleReplayController::Hooks Hooks()
    {
        auto hooks = deps.buildHooks(L"");
        const auto admit = hooks.admitDevice;
        hooks.admitDevice = [this, admit]()
        {
            if (!PumpMessages(context)) return false;
            // Dispatch can latch an incidental-render failure during the drain.
            if (context.failed()) return false;
            if (admit()) return true;
            frame = frame < 0 ? 0 : frame + 1;
            return Fail(8, "admitDevice", "device admission failed");
        };
        const auto pin = hooks.pinSettings;
        hooks.pinSettings = [this, pin](const PreviewScenario& value)
        {
            if (context.failed()) return false;
            frame = 0;
            context.inForce = value;
            return pin(value) || Fail(9, "pinSettings", "fixed profile or scenario settings failed");
        };
        const auto clear = hooks.clearInstances;
        hooks.clearInstances = [this, clear]()
        {
            if (context.failed()) return;
            clear();
            resetUpdateCount = deps.engine->CompletedFullUpdateCountForObservation();
            runtimeBaseline = deps.engine->RuntimeGuardRefusalCountForObservation();
            hardBaseline = deps.engine->HardGuardRefusalCountForObservation();
            indexBaseline = deps.engine->IndexCapRefusalCountForObservation();
        };
        const auto seed = hooks.seed;
        hooks.seed = [this, seed](std::uint32_t value)
        {
            if (context.failed()) return;
            seed(value);
            if (ParticleRandom::DrawCount() != 0) Fail(4, "seed", "draws before armSpawn must be zero");
        };
        const auto freeze = hooks.freezeClock;
        hooks.freezeClock = [this, freeze](float value) { if (!context.failed()) { frame = 0; freeze(value); } };
        const auto arm = hooks.armSpawn;
        hooks.armSpawn = [this, arm](const PreviewScenario& value)
        {
            return !context.failed() && (arm(value) || Fail(9, "armSpawn", "spawn hook failed"));
        };
        const auto step = hooks.stepClock;
        hooks.stepClock = [this, step]() { if (!context.failed()) { ++frame; step(); } };
        const auto tick = hooks.tickSpawner;
        hooks.tickSpawner = [this, tick]()
        {
            return !context.failed() && (tick() || Fail(9, "tickSpawner", "spawn tick failed"));
        };
        const auto update = hooks.update;
        hooks.update = [this, update]()
        {
            if (context.failed()) return false;
            if (!update()) return Fail(10, "update", "one completed full Update required");
            if (deps.engine->CompletedFullUpdateCountForObservation() != resetUpdateCount + static_cast<std::uint64_t>(frame) + 1)
                return Fail(4, "update-count", "incidental Update outside the replay controller");
            return CheckSettings(context) && (pass == 0 || Sample());
        };
        const auto render = hooks.render;
        hooks.render = [this, render]()
        {
            if (!CheckGeometry(context, "render-before")) return false;
            if (!render()) return Fail(11, "render", "render, query or composition failed");
            return CheckGeometry(context, "render-after");
        };
        hooks.readback = [this]()
        {
            if (context.failed()) return false;
            const auto chosen = selected.find(frame);
            if (chosen != selected.end())
            {
                const auto png = FramePath("selected-frames", "frame-%05d.png", frame);
                std::string hash;
                if (!Readback(png, &hash)) return false;
                capture["frames"].push_back({{"frame", frame}, {"reasons", chosen->second.reasons},
                    {"png", png}, {"sha256", hash}});
            }
            const auto play = playback.find(frame);
            if (play != playback.end())
            {
                if (!Readback(FramePath("playback-frames", "frame_%05d.png", play->second))) return false;
                capture["playback"].push_back({{"index", play->second}, {"frame", frame}});
            }
            return true;
        };
        return hooks;
    }

    json Summary(const ObservationLifecycle& lifecycle)
    {
        return {{"status", ObservationStatusString(lifecycle.Status())}, {"finalFrame", lifecycle.FinalFrame()},
            {"reviewWindow", {lifecycle.ReviewStartFrame(), lifecycle.ReviewEndFrame()}},
            {"endedBeforeReview", lifecycle.EndedBeforeReview()}, {"firstActive", OptionalFrame(lifecycle.FirstActiveFrame())},
            {"peak", OptionalFrame(lifecycle.PeakFrame())}, {"lastActive", OptionalFrame(lifecycle.LastActiveFrame())},
            {"limits", {
                {"runtimeGuard", {{"total", lifecycle.RuntimeGuardRefusals()}, {"firstFrame", OptionalFrame(lifecycle.FirstRuntimeGuardFrame())}}},
                {"hardGuard", {{"total", lifecycle.HardGuardRefusals()}, {"firstFrame", OptionalFrame(lifecycle.FirstHardGuardFrame())}}},
                {"indexCap", {{"total", lifecycle.IndexCapRefusals()}, {"firstFrame", OptionalFrame(lifecycle.FirstIndexCapFrame())}}}}}};
    }

    json EmitterSummary() const
    {
        const auto& emitters = deps.particleSystem->getEmitters();
        const auto landmarks = PerEmitterLandmarks(samples, emitterCount);
        json result = json::array();
        for (size_t i = 0; i < emitterCount; ++i)
            result.push_back({{"index", i}, {"name", emitters[i]->name},
                {"firstActive", OptionalFrame(landmarks[i].firstActive)}, {"peakFrame", OptionalFrame(landmarks[i].peakFrame)},
                {"lastActive", OptionalFrame(landmarks[i].lastActive)}, {"peakParticles", landmarks[i].peakParticles}});
        return result;
    }

    bool Execute()
    {
        std::ifstream input(std::filesystem::path(specPath), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::string error = input.bad() || !input.is_open() ? "spec file could not be read" : ParseObservationSpec(text, spec);
        if (error.empty()) error = ValidateObservationSpec(spec);
        if (!error.empty()) return Fail(2, "spec", error);
        validated = true;
        telemetryBudget = TelemetryByteBudget(spec.output.maxBytes);
        capture["spec"] = json::parse(SerializeObservationSpec(spec));
        Log("[observe] spec %s\n", capture["spec"].dump().c_str());
        if (!deps.engine) return Fail(8, "admitDevice", "engine unavailable");
        if (!WaitReady(context) || !PumpMessages(context) || !LoadDocument(context, spec.scenario.aloPath)) return false;
        const auto& emitters = deps.particleSystem->getEmitters();
        emitterCount = emitters.size();
        // Names go into the bundle verbatim; a replacement decode would merge distinct names.
        for (size_t i = 0; i < emitterCount; ++i)
            if (!IsValidUtf8(emitters[i]->name))
                return Fail(3, "emitter-name", "emitter " + std::to_string(i) + " name is not valid UTF-8");
        const std::string sourceHash = Sha256(spec.scenario.aloPath);
        if (sourceHash.empty()) return Fail(13, "source-hash", "source hash failed");
        capture["source"] = {{"path", WideToUtf8(std::filesystem::path(spec.scenario.aloPath).generic_wstring())},
            {"sha256", sourceHash}, {"bytes", std::filesystem::file_size(spec.scenario.aloPath)}};
        const auto mods = Paths(observation.selectedModPaths());
        const auto modHash = HashBytes(mods.dump());
        if (modHash.empty()) return Fail(13, "mod-path-hash", "selected mod path hash failed");
        capture["content"] = {{"gameRoots", Paths(observation.gameRoots())}, {"selectedModPaths", mods}, {"sha256", modHash}};
        capture["render"] = RenderProfile(*deps.engine);
        capture["random"] = {{"algorithm", "legacy-msvc"}, {"algorithmVersion", spec.scenario.algorithmVersion}, {"seed", spec.scenario.seed}};
        auto preparation = Hooks();
        if (!preparation.admitDevice() || !preparation.pinSettings(spec.scenario)) return false;
        preparation.clearInstances();
        preparation.seed(spec.scenario.seed);
        preparation.freezeClock(0);
        if (context.failed()) return false;
        context.profile = FixedProfile(*deps.engine);
        capture["profile"] = context.profile;
        if (!PrepareScene(context, preparation)) return false;
        if (!CheckGeometry(context, "resize-before")) return false;
        const auto before = deps.engine->CompletedFullUpdateCountForObservation();
        deps.settleResize();
        if (context.failed()) return false;
        if (deps.engine->CompletedFullUpdateCountForObservation() != before)
            return Fail(4, "resize-update-count", "SettleResize caused an incidental Update");
        if (!CheckGeometry(context, "resize-after")) return false;

        ObservationLifecycle lifecycle(spec);
        BeginPass(1);
        ParticleReplayController first(spec.scenario, Hooks());
        bool ok = first.Reset();
        if (ok) lifecycle.Push(samples.back());
        while (ok && lifecycle.Status() == ObservationStatus::Running)
        {
            ok = first.Advance(false);
            if (ok) lifecycle.Push(samples.back());
        }
        EndPass();
        if (!ok) return context.failed() ? false : Fail(9, "pass-1", "controller failed");
        capture["lifecycle"] = Summary(lifecycle);
        capture["lifecycle"]["emitters"] = EmitterSummary();
        Log("[observe] lifecycle=%s finalFrame=%d\n", ObservationStatusString(lifecycle.Status()), lifecycle.FinalFrame());

        // Measure one destination-only render; Reset in pass 2 restores the sample stream.
        const auto choices = SelectFrames(samples, spec);
        pass = 0;
        const int measuredFrame = lifecycle.PeakFrame().value_or(0);
        const auto scratchPath = out / L"preflight.png";
        struct ScratchFile
        {
            std::filesystem::path path;
            ~ScratchFile() { std::error_code error; std::filesystem::remove(path, error); }
        } scratch{scratchPath};
        auto measuredHooks = Hooks();
        const auto readback = deps.buildHooks(scratchPath.wstring()).readback;
        measuredHooks.readback = [this, readback]()
        {
            if (context.failed() || !CheckGeometry(context, "preflight-readback-before")) return false;
            if (!readback || !readback()) return Fail(13, "preflight-readback", "PNG capture failed");
            return CheckGeometry(context, "preflight-readback-after");
        };
        ParticleReplayController measured(spec.scenario, measuredHooks);
        if (!measured.SeekTo(measuredFrame, false))
            return context.failed() ? false : Fail(9, "quota-preflight", "controller seek failed");
        std::error_code sizeError, removeError;
        const auto measuredBytes = std::filesystem::file_size(scratchPath, sizeError);
        const bool removed = std::filesystem::remove(scratchPath, removeError);
        if (removeError || !removed) return Fail(13, "preflight-cleanup", removeError ? removeError.message() : "scratch PNG missing");
        if (sizeError) return Fail(13, "preflight-size", sizeError.message());
        // The scratch PNG is never a bundle file or part of the running byte total.
        Log("[observe] preflight frame=%d pngBytes=%llu\n", measuredFrame, static_cast<unsigned long long>(measuredBytes));
        std::uint64_t playbackCount = spec.output.playbackStride == 0 ? 0 :
            (lifecycle.ReviewEndFrame() - lifecycle.ReviewStartFrame()) / spec.output.playbackStride + 1;
        const auto estimate = ObservationPreflightBytes(measuredBytes, choices.size() + playbackCount, telemetryBudget.Bytes());
        Log("[observe] preflightBytes=%llu maxBytes=%llu\n", static_cast<unsigned long long>(estimate),
            static_cast<unsigned long long>(spec.output.maxBytes));
        if (estimate > spec.output.maxBytes) return Fail(16, "quota-preflight", "estimated native output bytes exceeded maxBytes");
        for (const auto& choice : choices)
        {
            selected.emplace(choice.frame, choice);
            if (std::find(choice.reasons.begin(), choice.reasons.end(), "stride") != choice.reasons.end())
                capture["stride"].push_back(choice.frame);
        }
        if (spec.output.playbackStride != 0)
            for (int f = lifecycle.ReviewStartFrame(), index = 0; f <= lifecycle.ReviewEndFrame(); f += spec.output.playbackStride)
                playback.emplace(f, index++);
        if (!Directory("selected-frames") || (!playback.empty() && !Directory("playback-frames"))) return false;
        BeginPass(2);
        // Frame 0 is rendered after Reset with no extra update, as the proof does.
        const auto captureHooks = Hooks();
        ParticleReplayController second(spec.scenario, captureHooks);
        ok = second.Reset() && captureHooks.render() && captureHooks.readback();
        for (int f = 1; ok && f <= lifecycle.FinalFrame(); ++f) ok = second.Advance(WantsCapture(f));
        EndPass();
        if (!ok) return context.failed() ? false : Fail(9, "pass-2", "controller failed");
        if (!Directory("telemetry")) return false;
        std::string telemetry;
        telemetry.reserve(static_cast<size_t>(telemetryBudget.Bytes()));
        for (const auto& sample : samples) telemetry += ObservationSampleLine(sample);
        return Write("telemetry/frames.jsonl", telemetry) && Write("telemetry/summary.json", capture["lifecycle"].dump(2));
    }

    bool WriteLog()
    {
        // The host flush callback makes the copy include the latest failure and pass lines.
        const auto path = observation.hostLogPath();
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!input.is_open() || input.bad()) return Fail(13, "log-copy", "per-process host log could not be read");
        std::error_code error;
        const auto directory = out / L"logs";
        if (std::filesystem::create_directory(directory, error)) directories.push_back(directory);
        else if (error) return Fail(13, "log-directory", error.message());
        const auto& ignored = deps.dispatcher.IgnoredReplayProofSettingsPushes();
        std::ofstream log(directory / L"observe.log", std::ios::binary | std::ios::trunc);
        log << text << "[observe] passWallMs=" << passMs[0] << "," << passMs[1]
            << " ignoredSettingsPushes=" << json{{"overloadGuard", ignored.overloadGuard},
                {"msaaLevel", ignored.msaaLevel}, {"estimatedLoad", ignored.estimatedLoad}}.dump() << "\n";
        log.flush();
        return log.good() || Fail(13, "log-write", "observe log write failed");
    }

    bool WriteCapture()
    {
        const auto marker = out / L"capture.json";
        if (!AtomicWrite(marker, capture.dump(2)))
        {
            auto temporary = marker;
            temporary += L".tmp";
            std::error_code error;
            std::filesystem::remove(temporary, error);
            return Fail(13, "capture-write", "atomic capture write failed");
        }
        return !validated || Account(marker);
    }

    int Run()
    {
        try
        {
            if (!AcceptDirectory()) return exitCode;
            Execute();
        }
        catch (const std::filesystem::filesystem_error& e) { Fail(13, "io", e.what()); }
        catch (const std::exception& e) { Fail(4, "exception", e.what()); }
        catch (...) { Fail(4, "exception", "unhandled engine or IO exception"); }
        EndPass();
        if (!accepted) return exitCode;
        // Completion marker is always last. Its actual bytes count too; quota failure
        // removes only our frames/telemetry and republishes the failed marker.
        try
        {
            WriteLog();
            if (!WriteCapture())
            {
                WriteLog();
                WriteCapture();
            }
        }
        catch (const std::exception& e)
        {
            Fail(13, "capture-write", e.what());
            // Best effort failed marker even when finalisation itself threw.
            try { WriteCapture(); } catch (...) {}
        }
        return exitCode;
    }
};

ObservationRunner::ObservationRunner(ReplayRunDeps deps, Deps extra, const std::wstring& specPath,
                                     const std::wstring& outputDirectory)
    : m_impl(std::make_unique<Impl>(std::move(deps), std::move(extra), specPath, outputDirectory)) {}
ObservationRunner::~ObservationRunner() = default;
int ObservationRunner::Run() { return m_impl->Run(); }
bool ObservationRunner::BeforeRender() { return CheckGeometry(m_impl->context, "composed-render-before"); }
bool ObservationRunner::AfterRender(bool succeeded)
{
    if (!CheckGeometry(m_impl->context, "composed-render-after")) return false;
    if (!succeeded && m_impl->context.geometryLocked)
        return m_impl->Fail(11, "composed-render", "incidental render/query/composition failed");
    return succeeded;
}

} // namespace host
