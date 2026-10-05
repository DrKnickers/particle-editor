#include "simulation/PreviewScenario.h"
#include "host/PreviewScenarioJson.h"
#include "host/third_party/nlohmann/json.hpp"
#include <cstdio>
#include <cstring>
#include <limits>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

static PreviewScenario ValidScenario()
{
    PreviewScenario scenario;
    scenario.aloPath = L"web/apps/editor/tests/fixtures/singleton-emitter.alo";
    return scenario;
}

template<class T>
static bool SameBits(T a, T b)
{
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

static bool SameScenario(const PreviewScenario& a, const PreviewScenario& b)
{
    return a.schemaVersion == b.schemaVersion && a.aloPath == b.aloPath &&
        a.seed == b.seed && a.algorithmVersion == b.algorithmVersion &&
        SameBits(a.clockAnchor, b.clockAnchor) &&
        SameBits(a.spawnPosition.x, b.spawnPosition.x) &&
        SameBits(a.spawnPosition.y, b.spawnPosition.y) &&
        SameBits(a.spawnPosition.z, b.spawnPosition.z) &&
        SameBits(a.spawnVelocity.x, b.spawnVelocity.x) &&
        SameBits(a.spawnVelocity.y, b.spawnVelocity.y) &&
        SameBits(a.spawnVelocity.z, b.spawnVelocity.z) &&
        a.maxFrames == b.maxFrames && a.overloadGuardEnabled == b.overloadGuardEnabled &&
        a.overloadGuardLimit == b.overloadGuardLimit &&
        SameBits(a.estimatedParticlesPerInstance, b.estimatedParticlesPerInstance);
}

static void Rejects(const PreviewScenario& scenario, const char* field)
{
    CHECK(ValidatePreviewScenario(scenario).find(field) != std::string::npos,
          "validation rejects with the field name");
}

int main()
{
    std::printf("test_preview_scenario\n");
    Rejects(PreviewScenario(), "aloPath");
    const PreviewScenario defaults = ValidScenario();
    CHECK(ValidatePreviewScenario(defaults).empty(), "explicit fixture path makes defaults valid");
    CHECK(defaults.schemaVersion == 1 && defaults.seed == ParticleRandom::kAutomationSeed &&
          defaults.algorithmVersion == ParticleRandom::kAlgorithmVersion &&
          SameBits(defaults.clockAnchor, 0.0f) &&
          defaults.spawnPosition == D3DXVECTOR3(0, 0, 0) &&
          defaults.spawnVelocity == D3DXVECTOR3(0, 0, 0) &&
          defaults.maxFrames == 600 && defaults.overloadGuardEnabled &&
          defaults.overloadGuardLimit == 10000 &&
          SameBits(defaults.estimatedParticlesPerInstance, 0.0), "all defaults match version 1");
    PreviewScenario scenario = defaults;
    scenario.schemaVersion = 2; Rejects(scenario, "schemaVersion");
    scenario = defaults; scenario.algorithmVersion = 0; Rejects(scenario, "algorithmVersion");
    const float invalidFloats[] = { 1.0f, -1.0f, (std::numeric_limits<float>::denorm_min)(),
        (std::numeric_limits<float>::infinity)(), -(std::numeric_limits<float>::infinity)(),
        (std::numeric_limits<float>::quiet_NaN)() };
    for (float value : invalidFloats)
    {
        scenario = defaults; scenario.clockAnchor = value; Rejects(scenario, "clockAnchor");
    }
    for (int value : { 0, 36001 })
    {
        scenario = defaults; scenario.maxFrames = value; Rejects(scenario, "maxFrames");
    }
    for (int value : { 1, 36000 })
    {
        scenario = defaults; scenario.maxFrames = value;
        CHECK(ValidatePreviewScenario(scenario).empty(), "maxFrames inclusive boundaries");
    }
    for (int value : { 999, 1000001 })
    {
        scenario = defaults; scenario.overloadGuardLimit = value; Rejects(scenario, "overloadGuardLimit");
    }
    for (int value : { 1000, 1000000 })
    {
        scenario = defaults; scenario.overloadGuardLimit = value;
        CHECK(ValidatePreviewScenario(scenario).empty(), "guard inclusive boundaries");
    }
    for (double value : { -1.0, (std::numeric_limits<double>::infinity)(),
                         -(std::numeric_limits<double>::infinity)(),
                         (std::numeric_limits<double>::quiet_NaN)() })
    {
        scenario = defaults; scenario.estimatedParticlesPerInstance = value;
        Rejects(scenario, "estimatedParticlesPerInstance");
    }
    for (int vector = 0; vector < 2; ++vector)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            for (float value : { -10000.0f, 10000.0f, -10001.0f, 10001.0f,
                                 (std::numeric_limits<float>::infinity)(),
                                 -(std::numeric_limits<float>::infinity)(),
                                 (std::numeric_limits<float>::quiet_NaN)() })
            {
                scenario = defaults;
                D3DXVECTOR3& position = vector == 0 ? scenario.spawnPosition : scenario.spawnVelocity;
                if (axis == 0) position.x = value;
                if (axis == 1) position.y = value;
                if (axis == 2) position.z = value;
                if (value == -10000.0f || value == 10000.0f)
                    CHECK(ValidatePreviewScenario(scenario).empty(), "vector inclusive boundary accepted");
                else Rejects(scenario, vector == 0 ? "spawnPosition" : "spawnVelocity");
            }
        }
    }

    using nlohmann::json;
    const json defaultJson = json::parse(host::SerializePreviewScenario(defaults));
    PreviewScenario parsed;
    CHECK(host::ParsePreviewScenario(defaultJson.dump(), parsed).empty() &&
          SameScenario(defaults, parsed), "defaults round trip every field");
    scenario = defaults;
    scenario.aloPath = L"C:\\effects\\\u96ea\U0001F680.alo";
    scenario.seed = 0xFFFFFFFFu;
    scenario.clockAnchor = -0.0f;
    scenario.spawnPosition = D3DXVECTOR3(0.1f, -0.0f, (std::numeric_limits<float>::denorm_min)());
    scenario.spawnVelocity = D3DXVECTOR3(-123.4567f, 9999.999f, -10000.0f);
    scenario.maxFrames = 36000;
    scenario.overloadGuardEnabled = false;
    scenario.overloadGuardLimit = 1000000;
    scenario.estimatedParticlesPerInstance = 12345.678901234567;
    CHECK(host::ParsePreviewScenario(host::SerializePreviewScenario(scenario), parsed).empty() &&
          SameScenario(scenario, parsed), "non-default fields, Unicode, subnormal and negative zero round trip");
    for (double value : { -0.0, (std::numeric_limits<double>::denorm_min)(),
                         (std::numeric_limits<double>::max)() })
    {
        scenario.estimatedParticlesPerInstance = value;
        CHECK(host::ParsePreviewScenario(host::SerializePreviewScenario(scenario), parsed).empty() &&
              SameScenario(scenario, parsed), "double edge cases round trip");
    }
    for (const char* field : { "unknown", "camera", "environment", "wind", "gravity",
                              "ground", "bloom", "content", "lifecycle" })
    {
        json input = defaultJson; input[field] = json::object();
        const PreviewScenario before = parsed;
        CHECK(host::ParsePreviewScenario(input.dump(), parsed).find(field) != std::string::npos,
              "unknown or deferred design key rejected by name");
        CHECK(SameScenario(before, parsed), "failure leaves output unchanged");
    }
    for (auto it = defaultJson.begin(); it != defaultJson.end(); ++it)
    {
        for (const json& wrong : { json(nullptr), json::object(), json("wrong") })
        {
            json input = defaultJson;
            input[it.key()] = it.key() == "aloPath" && wrong.is_string() ? json(false) : wrong;
            CHECK(host::ParsePreviewScenario(input.dump(), parsed).find(it.key()) != std::string::npos,
                  "every field rejects wrong types by name");
        }
    }
    for (const char* field : { "seed", "algorithmVersion", "schemaVersion", "maxFrames", "overloadGuardLimit" })
    {
        for (const json& value : { json(1.5), json(true), json(18446744073709551615ull) })
        {
            json input = defaultJson; input[field] = value;
            CHECK(host::ParsePreviewScenario(input.dump(), parsed).find(field) != std::string::npos,
                  "integer fields reject fractions, booleans and overflow");
        }
    }
    for (const json& value : { json(-1), json(4294967296ull) })
    {
        json input = defaultJson; input["seed"] = value;
        CHECK(host::ParsePreviewScenario(input.dump(), parsed).find("seed") != std::string::npos,
              "seed rejects negative and uint32 overflow");
    }
    for (const char* field : { "spawnPosition", "spawnVelocity" })
    {
        for (const json& value : { json::array({1, 2}), json::array({1, 2, 3, 4}),
                                  json::array({1, false, 3}), json::array({1, 10001, 3}) })
        {
            json input = defaultJson; input[field] = value;
            CHECK(host::ParsePreviewScenario(input.dump(), parsed).find(field) != std::string::npos,
                  "vectors reject bad length, element types and range");
        }
    }
    json input = defaultJson; input["clockAnchor"] = 1.0;
    CHECK(host::ParsePreviewScenario(input.dump(), parsed).find("clockAnchor") != std::string::npos,
          "JSON parsing invokes scenario validator");
    input["clockAnchor"] = 1.0e-100;
    CHECK(host::ParsePreviewScenario(input.dump(), parsed).find("clockAnchor") != std::string::npos,
          "nonzero JSON clock cannot underflow into the zero anchor");
    CHECK(host::ParsePreviewScenario("{\"aloPath\":\"fixture.alo\"}", parsed).empty(),
          "omitted fields retain defaults");
    for (const char* text : { "{", "[]", "null", "true", "{}" })
        CHECK(!host::ParsePreviewScenario(text, parsed).empty(), "malformed, non-object or empty scenario rejected");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
