#include "host/ParticleReplayController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Controller = host::ParticleReplayController;
using Log = std::vector<std::string>;
static const Log kReset = { "admitDevice", "pinSettings", "clearInstances", "seed",
    "freezeClock", "armSpawn", "tickSpawner", "update" };
static const Log kAdvance = { "admitDevice", "stepClock", "tickSpawner", "update", "render", "readback" };

static PreviewScenario Scenario()
{
    PreviewScenario scenario;
    scenario.aloPath = L"effect.alo";
    return scenario;
}

struct Stub
{
    Log log;
    std::string fail;
    int failOccurrence = 1;
    int occurrences = 0;
    float clock = 0.0f;
    float baseline = -1.0f;
    D3DXVECTOR3 position{0, 0, 0};
    D3DXVECTOR3 origin{0, 0, 0};
    D3DXVECTOR3 velocity{0, 0, 0};
    int instances = 0;
    int updates = 0;
    int cap = 0;
    int budget = 0;
    bool latch = true;
    bool refusal = true;
    bool armed = false;
    std::uint32_t seedValue = 0;
    int state = 0;

    bool Call(const char* name)
    {
        log.emplace_back(name);
        return fail != name || ++occurrences != failOccurrence;
    }

    Controller::Hooks Hooks()
    {
        Controller::Hooks hooks;
        hooks.admitDevice = [this]() { return Call("admitDevice"); };
        hooks.pinSettings = [this](const PreviewScenario& s)
        {
            if (!Call("pinSettings")) return false;
            cap = s.overloadGuardLimit;
            // Model the estimate setter's stale refusal; clear must follow settings.
            refusal = true;
            return true;
        };
        hooks.clearInstances = [this]()
        {
            Call("clearInstances");
            instances = 0;
            baseline = -1.0f;
            updates = 0;
            budget = cap;
            latch = false;
            refusal = false;
        };
        hooks.seed = [this](std::uint32_t value)
        {
            Call("seed");
            seedValue = value;
            state = 0;
        };
        hooks.freezeClock = [this](float anchor) { Call("freezeClock"); clock = anchor; };
        hooks.armSpawn = [this](const PreviewScenario& s)
        {
            if (!Call("armSpawn")) return false;
            armed = true;
            origin = s.spawnPosition;
            velocity = s.spawnVelocity;
            return true;
        };
        hooks.stepClock = [this]() { Call("stepClock"); clock += 1.0f / 60.0f; };
        hooks.tickSpawner = [this]()
        {
            if (!Call("tickSpawner")) return false;
            if (armed) { instances = 1; position = origin; armed = false; }
            return true;
        };
        hooks.update = [this]()
        {
            if (!Call("update")) return false;
            if (instances != 1) return false;
            if (baseline < 0.0f) baseline = clock;
            const float age = clock - baseline;
            position.x = origin.x + velocity.x * age;
            position.y = origin.y + velocity.y * age;
            position.z = origin.z + velocity.z * age;
            ++updates;
            state += budget;
            return true;
        };
        hooks.render = [this]() { return Call("render"); };
        hooks.readback = [this]() { return Call("readback"); };
        return hooks;
    }
};

static void Append(Log& target, const Log& tail)
{
    target.insert(target.end(), tail.begin(), tail.end());
}

static bool Quantum(const Log& log)
{
    bool stepped = false;
    bool ticked = false;
    for (const std::string& name : log)
    {
        if (name == "stepClock")
        {
            if (stepped) return false;
            stepped = true;
            ticked = false;
        }
        if (name == "tickSpawner") ticked = true;
        if (name == "update" && ticked) stepped = false;
    }
    return !stepped;
}

static void OrdersAndSeek()
{
    Stub stub;
    Controller controller(Scenario(), stub.Hooks());
    CHECK(controller.Frame() == -1, "no frame before reset");
    CHECK(controller.Reset() && controller.Frame() == 0 && stub.instances == 1 && stub.log == kReset,
          "reset order: seed before armSpawn, tickSpawner before update, no step or render");
    stub.log.clear();
    CHECK(controller.Advance(true) && controller.Frame() == 1 && stub.log == kAdvance,
          "advance order: admit, step, tickSpawner, update, render, readback");
    stub.log.clear();
    const Log updateOnly(kAdvance.begin(), kAdvance.begin() + 4);
    CHECK(controller.Advance(false) && controller.Frame() == 2 && stub.log == updateOnly,
          "advance without rendering omits render and readback only");
    for (int n : { 0, 1, 7 })
    {
        for (bool intermediate : { false, true })
        {
            stub.log.clear();
            Log expected = kReset;
            for (int i = 1; i <= n; ++i)
                Append(expected, intermediate || i == n ? kAdvance : updateOnly);
            if (n == 0) Append(expected, { "render", "readback" });
            CHECK(controller.SeekTo(n, intermediate) && controller.Frame() == n && stub.log == expected,
                  "seek 0/1/7 resets, renders intermediates by policy and destination exactly once");
            CHECK(stub.updates == n + 1, "one update per simulated frame plus reset");
            CHECK(Quantum(stub.log), "step quantum: one step followed by tick and update");
        }
    }
    const int accepted = controller.Frame();
    stub.log.clear();
    CHECK(!controller.SeekTo(-1, true) && !controller.SeekTo(601, false) &&
          controller.Frame() == accepted && stub.log.empty(), "seek cap rejects invalid destinations without hooks");
    CHECK(controller.SeekTo(600, false) && !controller.Advance(true) && controller.Frame() == 600,
          "advance respects scenario frame cap");
}

static void Failures()
{
    // Every fallible hook is rejected at every operation where it is invoked.
    for (const char* operation : { "reset", "advance", "seek" })
    {
        const Log& sequence = std::string(operation) == "reset" ? kReset : kAdvance;
        for (const std::string& hook : { "admitDevice", "pinSettings", "armSpawn", "tickSpawner", "update", "render", "readback" })
        {
            if (std::string(operation) == "advance" && (hook == "pinSettings" || hook == "armSpawn")) continue;
            if (std::string(operation) == "reset" && (hook == "render" || hook == "readback")) continue;
            Stub stub;
            Controller controller(Scenario(), stub.Hooks());
            CHECK(controller.SeekTo(3, false), "failure test establishes a prior accepted frame");
            stub.log.clear();
            stub.fail = hook;
            const int before = controller.Frame();
            bool ok;
            Log expected;
            if (std::string(operation) == "reset") ok = controller.Reset();
            else if (std::string(operation) == "advance") ok = controller.Advance(true);
            else ok = controller.SeekTo(7, true);
            if (std::string(operation) == "seek")
            {
                expected = kReset;
                Append(expected, kAdvance);
            }
            else expected = sequence;
            const auto failure = std::find(expected.begin(), expected.end(), hook);
            expected.erase(failure + 1, expected.end());
            const bool passed = !ok && stub.log == expected && controller.Frame() == before;
            std::printf("  failure case: %s/%s\n", operation, hook.c_str());
            CHECK(passed, "failing hook stops immediately with no later hook or frame acceptance");
        }
    }
    // Fail after earlier successful advances, and at the special frame-zero destination.
    for (const char* hook : { "admitDevice", "tickSpawner", "update", "render", "readback" })
    {
        for (bool intermediate : { false, true })
        {
            Stub stub;
            Controller controller(Scenario(), stub.Hooks());
            CHECK(controller.SeekTo(2, false), "late seek failure establishes prior frame");
            stub.log.clear();
            stub.fail = hook;
            stub.failOccurrence = std::string(hook) == "render" || std::string(hook) == "readback"
                ? (intermediate ? 3 : 1) : 4;
            CHECK(!controller.SeekTo(7, intermediate) && controller.Frame() == 2 && stub.log.back() == hook,
                  "late seek failure retains original frame after successful intermediates");
        }
    }
    for (const char* hook : { "render", "readback" })
    {
        Stub stub;
        Controller controller(Scenario(), stub.Hooks());
        CHECK(controller.Advance(false) == false && stub.log.empty(), "advance requires reset");
        stub.fail = hook;
        CHECK(!controller.SeekTo(0, false) && controller.Frame() == -1 && stub.log.back() == hook,
              "frame-zero destination failure accepts no frame");
    }
    Stub stub;
    auto hooks = stub.Hooks();
    hooks.readback = {};
    Controller controller(Scenario(), hooks);
    CHECK(controller.SeekTo(1, false) && stub.log.back() == "render", "readback is optional after successful render");
}

static void StrideVelocityAndCaps()
{
    for (int stride : { 1, 2, 3, 4 })
    {
        Stub stub;
        auto hooks = stub.Hooks();
        int frame = 0;
        std::vector<int> captured;
        hooks.readback = [&]()
        {
            if (frame % stride == 0) { stub.Call("readback"); captured.push_back(frame); }
            return true;
        };
        Controller controller(Scenario(), hooks);
        CHECK(controller.SeekTo(0, false), "stride frame zero is rendered and captured");
        for (frame = 1; frame <= 12; ++frame)
            CHECK(controller.Advance(true), "stride advances one rendered frame");
        std::vector<int> expected{ 0 };
        for (int i = stride; i <= 12; i += stride) expected.push_back(i);
        CHECK(captured == expected && stub.updates == 13 && stub.state == 13 * 10000 &&
              std::count(stub.log.begin(), stub.log.end(), "render") == 13 && Quantum(stub.log),
              "output stride k changes capture selection only, with identical updates and renders");
    }
    PreviewScenario scenario = Scenario();
    scenario.spawnPosition = D3DXVECTOR3(10, 20, 30);
    scenario.spawnVelocity = D3DXVECTOR3(60, 120, -60);
    Stub moving;
    Controller controller(scenario, moving.Hooks());
    CHECK(controller.Reset() && moving.clock - moving.baseline == 0.0f &&
          moving.position.x == 10 && moving.position.y == 20 && moving.position.z == 30,
          "nonzero velocity reset establishes age zero and spawn position");
    CHECK(controller.Advance(false) && std::fabs(moving.clock - moving.baseline - 1.0f / 60.0f) < 1e-7f &&
          moving.position.x == 11 && moving.position.y == 22 && moving.position.z == 29,
          "nonzero velocity frame one has age 1/60 and position plus velocity/60");
    CHECK(controller.SeekTo(1, false) && moving.position.x == 11 && moving.position.y == 22 && moving.position.z == 29,
          "nonzero velocity seek agrees with sequential hook baseline");
    Stub reused;
    int freshHigh = 0;
    {
        Stub fresh;
        Controller high(Scenario(), fresh.Hooks());
        CHECK(high.SeekTo(7, false), "fresh high-cap hook baseline");
        freshHigh = fresh.state;
    }
    for (int cap : { 10000, 1000, 10000 })
    {
        scenario.overloadGuardLimit = cap;
        Controller capped(scenario, reused.Hooks());
        CHECK(capped.Reset() && reused.budget == cap && !reused.refusal && !reused.latch,
              "high-low-high reset refills new cap and drains latch/refusal after settings");
        CHECK(capped.SeekTo(7, false) && reused.state == 8 * cap,
              "high-low-high hook run uses current cap only");
        reused.latch = true;
        reused.refusal = true;
    }
    CHECK(reused.state == freshHigh, "final high-cap hook run equals fresh high");
}

static std::string ReadSource(const char* path)
{
    std::ifstream input(std::filesystem::current_path() / path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

static std::string Body(const std::string& source, const char* signature)
{
    const size_t begin = source.find(signature);
    if (begin == std::string::npos) return {};
    const size_t end = source.find("\n}", begin);
    return end == std::string::npos ? std::string() : source.substr(begin, end - begin);
}

static int Count(const std::string& source, const char* needle)
{
    int count = 0;
    size_t pos = 0;
    while ((pos = source.find(needle, pos)) != std::string::npos) { ++count; pos += std::string(needle).size(); }
    return count;
}

static void SourcePins()
{
    const std::string wiring = ReadSource("src/host/HostWindow_Replay.cpp");
    CHECK(!wiring.empty() && Count(wiring, "StepPreviewFrames(1)") == 1 &&
          Count(wiring, "spawnerDriver->Tick(1.0f / 60.0f, particleSystem.get(), engine.get())") == 1 &&
          Count(wiring, "engine->Update();") == 1 &&
          Count(wiring, "CompletedFullUpdateCountForObservation()") == 2 &&
          wiring.find("== before + 1") != std::string::npos,
          "host authoritative triplet has literal one-frame clock, constant tick and one observed full update");
    CHECK(wiring.find("return RenderComposedFrame(false);") != std::string::npos,
          "host replay render hook is render-only");
    const size_t guard = wiring.find("engine->SetOverloadGuard(");
    const size_t estimate = wiring.find("engine->SetEstimatedLoad(");
    const size_t profile = wiring.find("if (!m_fixedCaptureView()) return false;");
    const std::string clear = Body(wiring, "hooks.clearInstances =");
    CHECK(guard < estimate && estimate < profile && profile != std::string::npos &&
          clear.find("engine->Clear();") < clear.find("engine->InvalidatePausedIdleSkip();") &&
          clear.find("engine->InvalidatePausedIdleSkip();") < clear.find("engine->TakeSpawnRefusal(nullptr);") &&
          clear.find("engine->TakeSpawnRefusal(nullptr);") != std::string::npos,
          "host reset pins guard, estimate, shared profile then clear, idle invalidation and refusal drain");
    const std::string host = ReadSource("src/host/HostWindow.cpp");
    const std::string wrapper = Body(host, "void HostWindowImpl::RenderD3D9()");
    const std::string frame = Body(host, "bool HostWindowImpl::RenderComposedFrame(bool simulate)");
    CHECK(wrapper.find("RenderComposedFrame(m_runMode != RunMode::ReplayProof);") != std::string::npos &&
          Count(wrapper, "RenderComposedFrame(") == 1 && Count(wrapper, "Update") == 0,
          "RenderD3D9 wrapper passes simulate false in ReplayProof");
    CHECK(frame.find("if (simulate && spawnerDriver && particleSystem && m_runMode != RunMode::Record)") != std::string::npos &&
          frame.find("if (simulate) m_mouseCursor.UpdateVelocity();") != std::string::npos &&
          frame.find("if (simulate) engine->Update();") != std::string::npos &&
          Count(frame, "spawnerDriver->Tick(") == 1 && Count(frame, "m_mouseCursor.UpdateVelocity();") == 1 &&
          Count(frame, "engine->Update();") == 1,
          "simulate guards every tick/cursor/update and preserves Record tick skip");
    const size_t resizeAt = host.find("case WM_WINDOWPOSCHANGED:");
    const size_t resizeEnd = host.find("\n    case ", resizeAt + 1);
    const std::string resizeDispatch = resizeAt == std::string::npos ? std::string() : host.substr(resizeAt, resizeEnd - resizeAt);
    const std::string resize = Body(host, "void HostWindowImpl::OnMainWindowPosChanged(");
    const std::string settle = Body(ReadSource("src/host/HostWindow_WebView2.cpp"), "void HostWindowImpl::SettleResize(");
    // A real resize callback during a proof run must count zero Updates in PR C's running-exe test.
    CHECK(resizeDispatch.find("OnMainWindowPosChanged(hwnd, lp);") != std::string::npos &&
          Count(resizeDispatch, "RenderComposedFrame(") == 0 &&
          !resize.empty() && Count(resize, "RenderD3D9();") == 1 && Count(resize, "RenderComposedFrame(") == 0 &&
          !settle.empty() && Count(settle, "RenderD3D9();") == 1 && Count(settle, "RenderComposedFrame(") == 0,
          "WM_WINDOWPOSCHANGED and SettleResize reach frames exclusively through wrapper");
    const std::string proof = ReadSource("src/host/ReplayProofRunner.cpp");
    const size_t resizeBefore = proof.find("const uint64_t before = deps.engine->CompletedFullUpdateCountForObservation();");
    const size_t resizeCall = proof.find("deps.settleResize();", resizeBefore);
    const size_t resizeAfter = proof.find("const uint64_t after = deps.engine->CompletedFullUpdateCountForObservation();", resizeCall);
    CHECK(resizeBefore < resizeCall && resizeCall < resizeAfter && resizeAfter != std::string::npos &&
          proof.find("if (before != after) return Fail(4, \"resize-update-count\"", resizeAfter) != std::string::npos &&
          host.find("SettleResize(\"replay-proof\");") != std::string::npos,
          "proof invokes real SettleResize and fails on an incidental completed Update");
    CHECK(proof.find("deps.buildHooks(capturePath).readback") != std::string::npos &&
          proof.find("std::to_string(frame) + \".png\"") != std::string::npos &&
          frame.find("proof->BeforeRender()") != std::string::npos &&
          frame.find("proof->AfterRender(succeeded)") != std::string::npos,
          "proof keeps authoritative readback with per-frame paths and brackets incidental renders");

    const std::string dispatch = ReadSource("src/host/BridgeDispatch_Engine.cpp");
    for (const auto& setting : std::vector<std::vector<const char*>>{
        { "engine/set/overload-guard", "overloadGuard", "SetOverloadGuard(", "m_overloadGuardCached       = true;" },
        { "engine/set/msaa-level", "msaaLevel", "SetMsaaLevel(", "ctx.RequireEngine(" },
        { "engine/set/estimated-load", "estimatedLoad", "SetEstimatedLoad(", "m_estimatedLoadCached       = true;" } })
    {
        const size_t begin = dispatch.find(std::string("if (kind == \"") + setting[0] + "\")");
        const size_t end = dispatch.find("\n    if (kind ==", begin + 1);
        const std::string handler = begin == std::string::npos ? std::string() : dispatch.substr(begin, end - begin);
        const std::string ignore = std::string("if (IgnoreReplayProofSettingsPush(m_replayProofIgnoredPushes.") + setting[1] + "))";
        const size_t gate = handler.find(ignore);
        const size_t reply = handler.find("ctx.SendOk(json::object());", gate);
        const size_t stop = handler.find("return true;", reply);
        CHECK(gate < reply && reply < stop && stop < handler.find(setting[2]) && stop < handler.find(setting[3]) &&
              stop < handler.find("params.value("),
              "proof settings handler counts and acknowledges before reading values, changing caches or calling engine");
    }
    const std::string setEngine = Body(ReadSource("src/host/BridgeDispatcher.cpp"), "void BridgeDispatcher::SetEngine(");
    CHECK(setEngine.find("if (m_engine && !m_replayProofOwnsSettings && m_overloadGuardCached)") <
              setEngine.find("m_engine->SetOverloadGuard(") &&
          setEngine.find("if (m_engine && !m_replayProofOwnsSettings && m_estimatedLoadCached)") <
              setEngine.find("m_engine->SetEstimatedLoad("),
          "SetEngine skips both cached settings reapplies during proof ownership");
    const size_t isolation = host.find("dispatcher->SetReplayProofOwnsSettings(m_runMode == RunMode::ReplayProof);");
    CHECK(isolation < host.find("dispatcher->SetEngine(engine.get());", isolation) &&
          isolation < host.find("HRESULT hr = InitWebView2();"),
          "host enables proof settings ownership before engine binding and WebView mount pushes");
    CHECK(proof.find("if (!CheckSettings()) return false;") != std::string::npos &&
          proof.find("deps.dispatcher.IgnoredReplayProofSettingsPushes()") != std::string::npos,
          "proof retains per-update settings checks and records ignored pushes");
    CHECK(proof.find("emitter->colorTexture =") == std::string::npos &&
          proof.find("emitter->normalTexture =") == std::string::npos,
          "proof uses loaded fixtures without replacing their textures");
}

int main()
{
    std::printf("test_particle_replay_controller\n");
    OrdersAndSeek();
    Failures();
    StrideVelocityAndCaps();
    SourcePins();
    std::printf("%s (%d failures)\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===", g_failed);
    return g_failed ? 1 : 0;
}
