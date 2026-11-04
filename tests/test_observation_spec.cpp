#include "simulation/ObservationSpec.h"
#include "host/ObservationSpecJson.h"
#include "host/third_party/nlohmann/json.hpp"
#include <cstdio>
#include <cstring>
#include <limits>

static int g_failed = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL: %s\n", msg);                                                                          \
        }                                                                                                              \
    } while (0)

static ObservationSpec ValidSpec()
{
    ObservationSpec spec;
    spec.scenario.aloPath = L"web/apps/editor/tests/fixtures/singleton-emitter.alo";
    return spec;
}

template <class T> static bool SameBits(T a, T b)
{
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

static bool SameSpec(const ObservationSpec& a, const ObservationSpec& b)
{
    if (a.hasCamera != b.hasCamera) return false;
    for (size_t i = 0; i < 3; ++i)
        if (!SameBits(a.camera.position[i], b.camera.position[i]) ||
            !SameBits(a.camera.target[i], b.camera.target[i]) || !SameBits(a.camera.up[i], b.camera.up[i]))
            return false;
    const PreviewScenario& x = a.scenario;
    const PreviewScenario& y = b.scenario;
    return a.schemaVersion == b.schemaVersion && x.schemaVersion == y.schemaVersion && x.aloPath == y.aloPath &&
           x.seed == y.seed && x.algorithmVersion == y.algorithmVersion && SameBits(x.clockAnchor, y.clockAnchor) &&
           SameBits(x.spawnPosition.x, y.spawnPosition.x) && SameBits(x.spawnPosition.y, y.spawnPosition.y) &&
           SameBits(x.spawnPosition.z, y.spawnPosition.z) && SameBits(x.spawnVelocity.x, y.spawnVelocity.x) &&
           SameBits(x.spawnVelocity.y, y.spawnVelocity.y) && SameBits(x.spawnVelocity.z, y.spawnVelocity.z) &&
           x.maxFrames == y.maxFrames && x.overloadGuardEnabled == y.overloadGuardEnabled &&
           x.overloadGuardLimit == y.overloadGuardLimit &&
           SameBits(x.estimatedParticlesPerInstance, y.estimatedParticlesPerInstance) &&
           a.lifecycle.type == b.lifecycle.type && a.lifecycle.quietTailFrames == b.lifecycle.quietTailFrames &&
           a.lifecycle.warmupFrames == b.lifecycle.warmupFrames &&
           a.lifecycle.reviewFrames == b.lifecycle.reviewFrames && a.output.strideInterval == b.output.strideInterval &&
           a.output.playbackStride == b.output.playbackStride && a.output.maxBytes == b.output.maxBytes &&
           a.hasContentLayers == b.hasContentLayers && a.contentLayers == b.contentLayers;
}

static void Rejects(const ObservationSpec& spec, const char* field)
{
    CHECK(ValidateObservationSpec(spec).find(field) != std::string::npos, "validation rejects with the field name");
}

static void RejectsJson(const std::string& text, ObservationSpec& parsed, const char* field)
{
    const ObservationSpec before = parsed;
    CHECK(host::ParseObservationSpec(text, parsed).find(field) != std::string::npos,
          "JSON rejects with the field name");
    CHECK(SameSpec(before, parsed), "JSON failure leaves every output field untouched");
}

int main()
{
    std::printf("test_observation_spec\n");
    using Type = ObservationSpec::Lifecycle::Type;
    using nlohmann::json;
    const ObservationSpec defaults = ValidSpec();
    CHECK(ValidateObservationSpec(defaults).empty(), "explicit fixture path makes defaults valid");
    CHECK(defaults.schemaVersion == 1 && defaults.lifecycle.type == Type::Oneshot &&
              defaults.lifecycle.quietTailFrames == 15 && defaults.lifecycle.warmupFrames == 0 &&
              defaults.lifecycle.reviewFrames == 1 && defaults.output.strideInterval == 15 &&
              defaults.output.playbackStride == 0 && defaults.output.maxBytes == 209715200ull,
          "observation defaults match schema 1");
    CHECK(ObservationRunCap(defaults) == defaults.scenario.maxFrames, "oneshot cap is the scenario cap");
    ObservationSpec spec = defaults;
    spec.schemaVersion   = 2;
    Rejects(spec, "schemaVersion");
    spec.scenario.aloPath.clear();
    CHECK(ValidateObservationSpec(spec).find("aloPath") != std::string::npos,
          "scenario validation precedes observation validation");
    spec                = defaults;
    spec.lifecycle.type = static_cast<Type>(2);
    Rejects(spec, "type");
    for (int value : {0, 3601, 36001})
    {
        spec                    = defaults;
        spec.scenario.maxFrames = value;
        Rejects(spec, "maxFrames");
    }
    for (int value : {1, 3600})
    {
        spec                       = defaults;
        spec.scenario.maxFrames    = value;
        spec.output.strideInterval = 600;
        CHECK(ValidateObservationSpec(spec).empty(), "observe maxFrames inclusive boundaries");
    }
    for (Type type : {Type::Oneshot, Type::Continuous})
    {
        for (int value : {0, 601, 1, 600})
        {
            spec                           = defaults;
            spec.lifecycle.type            = type;
            spec.lifecycle.quietTailFrames = value;
            if (value == 1 || value == 600)
                CHECK(ValidateObservationSpec(spec).empty(), "quiet tail inclusive bounds apply to both types");
            else
                Rejects(spec, "quietTailFrames");
        }
    }
    for (int value : {0, 601, 1, 600})
    {
        spec                       = defaults;
        spec.scenario.maxFrames    = 1;
        spec.output.strideInterval = value;
        if (value == 1 || value == 600)
            CHECK(ValidateObservationSpec(spec).empty(), "stride interval inclusive boundaries");
        else
            Rejects(spec, "strideInterval");
    }
    for (int value : {-1, 5, 0, 1, 2, 3, 4})
    {
        spec                       = defaults;
        spec.output.playbackStride = value;
        if (value < 0 || value > 4)
            Rejects(spec, "playbackStride");
        else
            CHECK(ValidateObservationSpec(spec).empty(), "all playback stride values accepted");
    }
    for (std::uint64_t value :
         {0ull, 1048575ull, 1048576ull, 2147483648ull, 2147483649ull, (std::numeric_limits<std::uint64_t>::max)()})
    {
        spec                 = defaults;
        spec.output.maxBytes = value;
        if (value == 1048576ull || value == 2147483648ull)
            CHECK(ValidateObservationSpec(spec).empty(), "maxBytes inclusive boundaries");
        else
            Rejects(spec, "maxBytes");
    }
    spec                       = defaults;
    spec.output.strideInterval = 1;
    spec.scenario.maxFrames    = 62;
    CHECK(ValidateObservationSpec(spec).empty(), "64 stride-frame bound is inclusive");
    spec.scenario.maxFrames = 63;
    Rejects(spec, "strideInterval");
    spec.scenario.maxFrames    = 125;
    spec.output.strideInterval = 2;
    CHECK(ValidateObservationSpec(spec).empty(), "stride bound uses integer division");
    spec.scenario.maxFrames = 126;
    Rejects(spec, "strideInterval");
    spec                = defaults;
    spec.lifecycle.type = Type::Continuous;
    CHECK(ValidateObservationSpec(spec).empty() && spec.lifecycle.quietTailFrames == 15,
          "continuous keeps default quiet tail");
    spec.lifecycle.warmupFrames = -1;
    Rejects(spec, "warmupFrames");
    spec.lifecycle.warmupFrames = 0;
    spec.lifecycle.reviewFrames = 0;
    Rejects(spec, "reviewFrames");
    spec.lifecycle.warmupFrames = 599;
    spec.lifecycle.reviewFrames = 1;
    CHECK(ValidateObservationSpec(spec).empty() && ObservationRunCap(spec) == 600,
          "continuous sum equal to maxFrames is valid and is the cap");
    spec.lifecycle.reviewFrames = 2;
    Rejects(spec, "reviewFrames");
    spec.lifecycle.warmupFrames = (std::numeric_limits<int>::max)();
    spec.lifecycle.reviewFrames = (std::numeric_limits<int>::max)();
    Rejects(spec, "warmupFrames");
    spec                        = defaults;
    spec.lifecycle.type         = Type::Continuous;
    spec.lifecycle.warmupFrames = 100;
    spec.lifecycle.reviewFrames = 62;
    spec.output.strideInterval  = 1;
    CHECK(ValidateObservationSpec(spec).empty() && ObservationRunCap(spec) == 162,
          "continuous stride count uses reviewFrames and cap uses the sum");
    spec.lifecycle.reviewFrames = 63;
    Rejects(spec, "strideInterval");

    ObservationSpec parsed;
    const json      defaultJson = json::parse(host::SerializeObservationSpec(defaults));
    CHECK(host::ParseObservationSpec(defaultJson.dump(), parsed).empty() && SameSpec(defaults, parsed),
          "every default field round trips");
    spec                               = defaults;
    spec.scenario.aloPath              = L"C:\\effects\\\u96ea\U0001F680.alo";
    spec.scenario.seed                 = 0xFFFFFFFFu;
    spec.scenario.clockAnchor          = -0.0f;
    spec.scenario.spawnPosition        = D3DXVECTOR3(0.1f, -0.0f, (std::numeric_limits<float>::denorm_min)());
    spec.scenario.spawnVelocity        = D3DXVECTOR3(-123.4567f, 9999.999f, -10000.0f);
    spec.scenario.maxFrames            = 3600;
    spec.scenario.overloadGuardEnabled = false;
    spec.scenario.overloadGuardLimit   = 1000000;
    spec.lifecycle.type                = Type::Continuous;
    spec.lifecycle.quietTailFrames     = 600;
    spec.lifecycle.warmupFrames        = 200;
    spec.lifecycle.reviewFrames        = 3400;
    spec.output.strideInterval         = 600;
    spec.output.playbackStride         = 4;
    spec.output.maxBytes               = 2147483648ull;
    for (double value :
         {12345.678901234567, -0.0, (std::numeric_limits<double>::denorm_min)(), (std::numeric_limits<double>::max)()})
    {
        spec.scenario.estimatedParticlesPerInstance = value;
        CHECK(host::ParseObservationSpec(host::SerializeObservationSpec(spec), parsed).empty() &&
                  SameSpec(spec, parsed),
              "non-default fields, Unicode and floating edge bits round trip");
    }
    for (const char* level : {"", "scenario", "lifecycle", "output"})
    {
        json input = defaultJson;
        if (*level)
            input[level]["unknown"] = 0;
        else
            input["unknown"] = 0;
        RejectsJson(input.dump(), parsed, "unknown");
    }
    for (const char* level : {"scenario", "lifecycle", "output"})
    {
        for (const json& wrong : {json(nullptr), json::array(), json("wrong"), json(true)})
        {
            json input   = defaultJson;
            input[level] = wrong;
            RejectsJson(input.dump(), parsed, level);
        }
    }
    for (const char* level : {"", "lifecycle", "output"})
    {
        const json& object = *level ? defaultJson.at(level) : defaultJson;
        for (auto it = object.begin(); it != object.end(); ++it)
        {
            if (it.key() == "scenario" || it.key() == "lifecycle" || it.key() == "output") continue;
            for (const json& wrong : {json(nullptr), json::object(), json(true), json(1.5), json("wrong")})
            {
                json input = defaultJson;
                if (std::string(level) == "lifecycle") input["lifecycle"]["type"] = "continuous";
                json& target     = *level ? input[level] : input;
                target[it.key()] = wrong;
                RejectsJson(input.dump(), parsed, it.key().c_str());
            }
            if (it.key() == "type") continue;
            for (const json& wrong : {json(-1), json(18446744073709551615ull)})
            {
                json input = defaultJson;
                if (std::string(level) == "lifecycle") input["lifecycle"]["type"] = "continuous";
                json& target     = *level ? input[level] : input;
                target[it.key()] = wrong;
                RejectsJson(input.dump(), parsed, it.key().c_str());
            }
        }
    }
    json input                     = defaultJson;
    input["scenario"]["maxFrames"] = 3601;
    RejectsJson(input.dump(), parsed, "maxFrames");
    input                      = defaultJson;
    input["lifecycle"]["type"] = "loop";
    RejectsJson(input.dump(), parsed, "type");
    input["lifecycle"]["type"]         = "continuous";
    input["lifecycle"]["warmupFrames"] = 600;
    RejectsJson(input.dump(), parsed, "reviewFrames");
    input                             = defaultJson;
    input["output"]["strideInterval"] = 1;
    RejectsJson(input.dump(), parsed, "strideInterval");
    CHECK(host::ParseObservationSpec("{\"scenario\":{\"aloPath\":\"fixture.alo\"}}", parsed).empty() &&
              parsed.lifecycle.type == Type::Oneshot && parsed.output.maxBytes == defaults.output.maxBytes,
          "omitted observation fields retain defaults");
    for (const char* text : {"{", "[]", "null", "true", "{}"})
    {
        const ObservationSpec before = parsed;
        CHECK(!host::ParseObservationSpec(text, parsed).empty(), "malformed or missing scenario rejected");
        CHECK(SameSpec(before, parsed), "malformed input leaves output unchanged");
    }

    // Schema 2 = contentLayers: an explicit stack (empty = unmodded control).
    ObservationSpec layered  = defaults;
    layered.schemaVersion    = 2;
    layered.hasContentLayers = true;
    CHECK(ValidateObservationSpec(layered).empty(), "schema 2 with an empty stack is valid");
    layered.contentLayers = {"C:\\gen\\root", "d:/mods/Other Mod/", "C:\\\xE9\x9B\xAA"};
    CHECK(ValidateObservationSpec(layered).empty(), "drive paths of either slash and UTF-8 are valid");
    CHECK(host::ParseObservationSpec(host::SerializeObservationSpec(layered), parsed).empty() &&
              SameSpec(layered, parsed),
          "contentLayers round trip in order");
    CHECK(json::parse(host::SerializeObservationSpec(defaults)).count("contentLayers") == 0,
          "schema 1 serializes without contentLayers");
    spec               = layered;
    spec.schemaVersion = 1;
    Rejects(spec, "contentLayers requires schemaVersion 2");
    spec                  = layered;
    spec.hasContentLayers = false;
    spec.contentLayers.clear();
    Rejects(spec, "schemaVersion 2 requires contentLayers");
    for (const char* bad : {"", "C:", "gen\\root", "\\gen\\root", "\\\\server\\share", "1:\\x", "C:x\\y"})
    {
        spec               = layered;
        spec.contentLayers = {bad};
        Rejects(spec, "contentLayers[0]");
    }
    spec               = layered;
    spec.contentLayers = {"C:\\gen\x01root"};
    Rejects(spec, "control characters");
    spec               = layered;
    spec.contentLayers = {"C:\\gen\troot"};
    Rejects(spec, "control characters");
    spec               = layered;
    spec.contentLayers = {"C:\\gen\\root "};
    Rejects(spec, "space-ended folder name");
    spec               = layered;
    spec.contentLayers = {"C:\\Gen\\Root", "c:/gen/root/"};
    Rejects(spec, "contentLayers[1] repeats");
    spec               = layered;
    spec.contentLayers = {"C:\\" + std::string(158, 'a')};
    Rejects(spec, "contentLayers[0]");
    spec.contentLayers = {"C:\\" + std::string(157, 'a')};
    CHECK(ValidateObservationSpec(spec).empty(), "160-byte layer path is the inclusive bound");
    // Win32 trims a component's trailing dots and spaces for the folder but not
    // for files below it, so these would pass a folder check and load nothing.
    for (const char* bad : {"C:\\gen .", "C:\\gen.", "C:\\gen..", "C:\\gen. \\x", "C:\\gen\\.", "C:\\gen\\..\\x",
                            "C:\\gen\\\\x", "C:/gen//x", "C:\\gen \\x"})
    {
        spec               = layered;
        spec.contentLayers = {bad};
        Rejects(spec, "space-ended folder name");
    }
    for (const char* good : {"C:\\", "C:/", "C:\\gen\\", "C:\\a.b\\c d"})
    {
        spec               = layered;
        spec.contentLayers = {good};
        CHECK(ValidateObservationSpec(spec).empty(), "roots, trailing separators and inner dots or spaces are valid");
    }
    spec = layered;
    spec.contentLayers.clear();
    for (int i = 0; i < 16; ++i) spec.contentLayers.push_back("C:\\layer" + std::to_string(i));
    CHECK(ValidateObservationSpec(spec).empty(), "16 layers is the inclusive bound");
    spec.contentLayers.push_back("C:\\layer16");
    Rejects(spec, "at most 16");
    const json layeredJson = json::parse(host::SerializeObservationSpec(layered));
    for (const json& wrong :
         {json(nullptr), json::object(), json("C:\\gen"), json(true), json::array({1}), json::array({nullptr})})
    {
        json input             = layeredJson;
        input["contentLayers"] = wrong;
        RejectsJson(input.dump(), parsed, "contentLayers");
    }
    json input2             = layeredJson;
    input2["contentLayers"] = json::array();
    for (int i = 0; i < 17; ++i) input2["contentLayers"].push_back("C:\\layer" + std::to_string(i));
    RejectsJson(input2.dump(), parsed, "at most 16");
    input2                  = layeredJson;
    input2["schemaVersion"] = 1;
    RejectsJson(input2.dump(), parsed, "contentLayers requires schemaVersion 2");
    input2 = layeredJson;
    input2.erase("contentLayers");
    RejectsJson(input2.dump(), parsed, "schemaVersion 2 requires contentLayers");

    CHECK(defaultJson.count("camera") == 0, "absent camera serializes without a camera key");
    ObservationSpec withCamera = defaults;
    withCamera.hasCamera       = true;
    withCamera.camera.position = {0.1f, -500, 250};
    withCamera.camera.target   = {-0.0f, 0, 0};
    withCamera.camera.up       = {0, 0, 2};
    for (int schema : {1, 2})
    {
        withCamera.schemaVersion    = schema;
        withCamera.hasContentLayers = schema == 2;
        const json cameraJson       = json::parse(host::SerializeObservationSpec(withCamera));
        CHECK(cameraJson["camera"].size() == 3 && cameraJson["camera"]["position"].size() == 3,
              "camera serializes all three vectors");
        CHECK(host::ParseObservationSpec(cameraJson.dump(), parsed).empty() && SameSpec(withCamera, parsed),
              "camera round trips in either schema with float bits intact");
        ObservationSpec again;
        CHECK(host::ParseObservationSpec(host::SerializeObservationSpec(parsed), again).empty() &&
                  SameSpec(parsed, again),
              "parsed camera serializes and parses again unchanged");
    }
    const json cameraJson = json::parse(host::SerializeObservationSpec(withCamera));
    input                 = cameraJson;
    input.erase("contentLayers");
    RejectsJson(input.dump(), parsed, "schemaVersion 2 requires contentLayers");
    input                  = cameraJson;
    input["schemaVersion"] = 1;
    RejectsJson(input.dump(), parsed, "contentLayers requires schemaVersion 2");
    for (const char* field : {"position", "target", "up"})
    {
        input = cameraJson;
        input["camera"].erase(field);
        RejectsJson(input.dump(), parsed, (std::string("camera.") + field).c_str());
        for (const json& wrong : {json(nullptr), json(true), json(1), json("vector"), json::object(), json::array(),
                                  json::array({0, 1}), json::array({0, 1, 2, 3})})
        {
            input                  = cameraJson;
            input["camera"][field] = wrong;
            RejectsJson(input.dump(), parsed, (std::string("camera.") + field).c_str());
        }
        for (size_t i = 0; i < 3; ++i)
        {
            for (const json& wrong : {json(nullptr), json(true), json("1"), json::object(), json::array()})
            {
                input                     = cameraJson;
                input["camera"][field][i] = wrong;
                RejectsJson(input.dump(), parsed, (std::string("camera.") + field).c_str());
            }
            input                     = cameraJson;
            input["camera"][field][i] = 1e100;
            RejectsJson(input.dump(), parsed, "out of range");
            for (float bad : {std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()})
            {
                spec         = withCamera;
                auto& vector = std::strcmp(field, "position") == 0 ? spec.camera.position
                               : std::strcmp(field, "target") == 0 ? spec.camera.target
                                                                   : spec.camera.up;
                vector[i]    = bad;
                Rejects(spec, (std::string("camera.") + field).c_str());
            }
        }
    }
    input                   = cameraJson;
    input["camera"]["zoom"] = 2;
    RejectsJson(input.dump(), parsed, "camera.zoom");
    for (const json& wrong : {json(nullptr), json(true), json(1), json("camera"), json::array()})
    {
        input           = cameraJson;
        input["camera"] = wrong;
        RejectsJson(input.dump(), parsed, "camera must be a JSON object");
    }
    input                       = cameraJson;
    input["camera"]["position"] = input["camera"]["target"];
    RejectsJson(input.dump(), parsed, "must differ by at least 0.01");
    input                 = cameraJson;
    input["camera"]["up"] = {0, 0, 0};
    RejectsJson(input.dump(), parsed, "nonzero length");
    for (int sign : {-1, 1})
    {
        input                       = cameraJson;
        input["camera"]["position"] = {0, -500, 250};
        input["camera"]["up"]       = {0, 500 * sign, -250 * sign};
        RejectsJson(input.dump(), parsed, "parallel");
    }
    // The engine works in floats: huge or vanishing cameras are refused, not passed on.
    spec                 = withCamera;
    const float largest  = (std::numeric_limits<float>::max)();
    spec.camera.position = {largest, 0, 0};
    spec.camera.target   = {-largest, 0, 0};
    spec.camera.up       = {0, 1, 0};
    Rejects(spec, "camera.position must be within");
    spec               = withCamera;
    spec.camera.target = {0, 0, 2.0e6f};
    Rejects(spec, "camera.target must be within");
    spec           = withCamera;
    spec.camera.up = {0, 0, 2.0e6f};
    Rejects(spec, "camera.up must be within");
    spec                 = withCamera;
    spec.camera.position = {1.0e6f, -1.0e6f, 1.0e6f};
    spec.camera.target   = {-1.0e6f, 1.0e6f, -1.0e6f};
    CHECK(ValidateObservationSpec(spec).empty(), "the largest allowed camera validates");
    spec                 = withCamera;
    const float smallest = (std::numeric_limits<float>::denorm_min)();
    spec.camera.position = {0, 0, 0};
    spec.camera.target   = {smallest, 0, 0};
    spec.camera.up       = {0, 1, 0};
    Rejects(spec, "at least 0.01");
    spec.camera.target = {0.02f, 0, 0};
    spec.camera.up     = {0, smallest, 0};
    Rejects(spec, "nonzero length");
    // Nearly parallel is as degenerate as parallel; a modest tilt is fine.
    spec                 = withCamera;
    spec.camera.position = {0, -500, 250};
    spec.camera.target   = {0, 0, 0};
    spec.camera.up       = {0, 500, -250.01f};
    Rejects(spec, "parallel");
    spec.camera.position = {300, -300, 200};
    spec.camera.up       = {0, 0, 1};
    CHECK(ValidateObservationSpec(spec).empty(), "a rotated camera validates");
    CHECK(host::ParseObservationSpec(defaultJson.dump(), parsed).empty() && !parsed.hasCamera,
          "parsing a spec without a camera clears the previous camera");

    spec                 = defaults;
    spec.output.maxBytes = 0;
    bool threw           = false;
    try
    {
        host::SerializeObservationSpec(spec);
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw, "serializer rejects invalid specs");
    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
