// Production-oracle test for BridgeDispatcher's spawner state seams.
// The header-visible members below are the actual start/stop mutations and
// state resolver used by both bridge snapshot/event call sites; this harness
// deliberately avoids the D3D/WebView dispatcher translation unit.

#include "../src/host/AcceleratorBridge.h"
#include "../src/host/BridgeDispatcher.h"
#include "../src/host/LayoutBroker.h"

#include <cstdio>
#include <memory>

namespace host {

struct SpawnerConfigWiringTestAccess
{
    static std::unique_ptr<BridgeDispatcher> MakeDispatcher(
        LayoutBroker& layout, AcceleratorBridge& accel, Engine* engine = NULL)
    {
        return std::unique_ptr<BridgeDispatcher>(
            new BridgeDispatcher(layout, accel,
                                 BridgeDispatcher::SpawnerConfigTestTag{}, engine));
    }

    static nlohmann::json State(const BridgeDispatcher& dispatcher)
    {
        return dispatcher.BuildSpawnerStateJson();
    }

    static void Start(BridgeDispatcher& dispatcher, const nlohmann::json& params)
    {
        dispatcher.ApplySpawnerStart(params);
    }

    static void Stop(BridgeDispatcher& dispatcher)
    {
        dispatcher.ApplySpawnerStop();
    }
};

} // namespace host

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// Engine link stubs. The real SpawnerDriver is used so ApplySpawnerStop must
// cancel its armed/manual queue before it flips the shared config.
static int g_spawnCalls = 0;
ParticleSystemInstance* Engine::SpawnParticleSystem(const ParticleSystem&, Object3D*)
{
    ++g_spawnCalls;
    return NULL;
}

int Engine::ActiveSpawnerInstanceCount() const
{
    return 0;
}

alignas(Engine) static unsigned char g_engineStorage[sizeof(Engine)];
static Engine* FakeEngine() { return reinterpret_cast<Engine*>(g_engineStorage); }
alignas(16) static unsigned char g_systemStorage[16];
static const ParticleSystem* FakeSystem()
{
    return reinterpret_cast<const ParticleSystem*>(g_systemStorage);
}

static nlohmann::json OutOfRangeConfig()
{
    return {
        {"mode",              "unexpected"},
        {"enabled",           true},
        {"burstSize",         99},
        {"spacingSec",        -1.0f},
        {"intervalSec",       61.0f},
        {"position",          {10001.0f, -10001.0f, 10001.0f}},
        {"velocity",          {-10001.0f, 10001.0f, -10001.0f}},
        {"maxLifetimeSec",    -1.0f},
        {"jitterPosition",    {10001.0f, -10001.0f, 10001.0f}},
        {"acceleration",      {-10001.0f, 10001.0f, -10001.0f}},
        {"squiggleAmplitude", {10001.0f, -10001.0f, 10001.0f}},
        {"squiggleFrequency", 21.0f},
    };
}

static nlohmann::json ClampedConfig()
{
    return {
        {"mode",              "auto"},
        {"enabled",           true},
        {"burstSize",         SpawnerDriver::MAX_BURST_SIZE},
        {"spacingSec",        0.0f},
        {"intervalSec",       SpawnerDriver::MAX_INTERVAL_SEC},
        {"position",          {SpawnerDriver::JITTER_MAX, -SpawnerDriver::JITTER_MAX, SpawnerDriver::JITTER_MAX}},
        {"velocity",          {-SpawnerDriver::JITTER_MAX, SpawnerDriver::JITTER_MAX, -SpawnerDriver::JITTER_MAX}},
        {"maxLifetimeSec",    0.0f},
        {"jitterPosition",    {SpawnerDriver::JITTER_MAX, -SpawnerDriver::JITTER_MAX, SpawnerDriver::JITTER_MAX}},
        {"acceleration",      {-SpawnerDriver::JITTER_MAX, SpawnerDriver::JITTER_MAX, -SpawnerDriver::JITTER_MAX}},
        {"squiggleAmplitude", {SpawnerDriver::JITTER_MAX, -SpawnerDriver::JITTER_MAX, SpawnerDriver::JITTER_MAX}},
        {"squiggleFrequency", SpawnerDriver::SQUIGGLE_FREQ_MAX},
    };
}

static bool DriverHasClampedConfig(const SpawnerDriver& driver)
{
    const SpawnerConfig& cfg = driver.GetConfig();
    return cfg.mode == SpawnerConfig::Mode::Auto
        && cfg.enabled
        && cfg.burstSize == SpawnerDriver::MAX_BURST_SIZE
        && cfg.spacingSec == 0.0f
        && cfg.intervalSec == SpawnerDriver::MAX_INTERVAL_SEC
        && cfg.maxLifetimeSec == 0.0f
        && cfg.position.x == SpawnerDriver::JITTER_MAX
        && cfg.position.y == -SpawnerDriver::JITTER_MAX
        && cfg.velocity.x == -SpawnerDriver::JITTER_MAX
        && cfg.velocity.y == SpawnerDriver::JITTER_MAX
        && cfg.jitterPosition.x == SpawnerDriver::JITTER_MAX
        && cfg.acceleration.x == -SpawnerDriver::JITTER_MAX
        && cfg.squiggleAmplitude.x == SpawnerDriver::JITTER_MAX
        && cfg.squiggleFrequency == SpawnerDriver::SQUIGGLE_FREQ_MAX;
}

int main()
{
    std::printf("test_bridge_spawner_state\n");
    host::LayoutBroker layout;
    host::AcceleratorBridge accel;

    // Driverless start must echo the parsed, clamped structure rather than the
    // raw params. This is the partial-wiring / Vitest contract.
    {
        std::unique_ptr<host::BridgeDispatcher> dispatcher =
            host::SpawnerConfigWiringTestAccess::MakeDispatcher(layout, accel);
        host::SpawnerConfigWiringTestAccess::Start(*dispatcher, OutOfRangeConfig());
        CHECK(host::SpawnerConfigWiringTestAccess::State(*dispatcher) == ClampedConfig(),
              "unbound start echoes every native-clamped field");

        SpawnerDriver delayedDriver;
        dispatcher->BindHostState(NULL, &delayedDriver, NULL);
        CHECK(DriverHasClampedConfig(delayedDriver),
              "a later-bound driver receives the dispatcher-owned config");
    }

    // Stop must also reflect correctly when the dispatcher has no driver at
    // all; this is the normal partial-wiring/Vitest path.
    {
        std::unique_ptr<host::BridgeDispatcher> dispatcher =
            host::SpawnerConfigWiringTestAccess::MakeDispatcher(layout, accel);
        host::SpawnerConfigWiringTestAccess::Start(*dispatcher, OutOfRangeConfig());

        nlohmann::json expectedStopped = ClampedConfig();
        expectedStopped["enabled"] = false;
        host::SpawnerConfigWiringTestAccess::Stop(*dispatcher);
        CHECK(host::SpawnerConfigWiringTestAccess::State(*dispatcher) == expectedStopped,
              "unbound stop reflects disabled and preserves config");
    }

    // EmitStatsTick's overload-refusal branch is deliberately one-line
    // wiring to this same mutation. Model its engine-bound/driver-unbound
    // state here so the config cannot echo enabled=true after that refusal.
    {
        std::unique_ptr<host::BridgeDispatcher> dispatcher =
            host::SpawnerConfigWiringTestAccess::MakeDispatcher(layout, accel, FakeEngine());
        host::SpawnerConfigWiringTestAccess::Start(*dispatcher, OutOfRangeConfig());
        nlohmann::json expectedStopped = ClampedConfig();
        expectedStopped["enabled"] = false;
        host::SpawnerConfigWiringTestAccess::Stop(*dispatcher);
        CHECK(host::SpawnerConfigWiringTestAccess::State(*dispatcher) == expectedStopped,
              "driver-unbound overload-refusal echo is disabled and preserves config");
    }

    // A bound driver receives the same config, and its queued manual work is
    // observably cancelled through the real SpawnerDriver instance.
    {
        std::unique_ptr<host::BridgeDispatcher> dispatcher =
            host::SpawnerConfigWiringTestAccess::MakeDispatcher(layout, accel);
        SpawnerDriver driver;
        dispatcher->BindHostState(NULL, &driver, NULL);

        nlohmann::json manual = ClampedConfig();
        manual["mode"] = "manual";
        host::SpawnerConfigWiringTestAccess::Start(*dispatcher, manual);
        CHECK(driver.GetConfig().mode == SpawnerConfig::Mode::Manual
           && driver.GetConfig().burstSize == SpawnerDriver::MAX_BURST_SIZE,
              "bound driver receives the dispatcher-owned clamped config");

        driver.Trigger(FakeSystem(), FakeEngine());
        driver.Trigger(FakeSystem(), FakeEngine());
        const int beforeStopTick = g_spawnCalls;
        host::SpawnerConfigWiringTestAccess::Stop(*dispatcher);
        driver.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == beforeStopTick,
              "stop cancels the driver's armed burst and pending manual queue");
        CHECK(!driver.GetConfig().enabled,
              "stop disables the bound driver from the shared config");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    return g_failed ? 1 : 0;
}
