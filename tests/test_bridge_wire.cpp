// Regression test for the host->UI bridge serializer and dispatch guard
// (src/host/BridgeWire.h, 2026-09-30 audit H1).
//
// Emitter names are the raw bytes of an .alo's 0x16 chunk, not UTF-8. One
// emitter named "\xE9..." made nlohmann's default dump() throw type_error 316
// from the emitters/list response and from every emitters/tree/changed event,
// and the async door's dump sat outside every try -- straight through the
// WebView2 COM callback. This pins the chokepoint every response and event now
// goes through (SerializeBridgeEnvelope) and the guard every request handler
// runs under (RunGuardedDispatch). The end-to-end leg (a real .alo loaded by
// the real exe) is web/apps/editor/tests/bridge-utf8-safety.spec.ts.

#include "host/BridgeWire.h"

#include <cstdio>
#include <stdexcept>
#include <string>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using nlohmann::json;

// "default" with its first byte replaced by Latin-1 e-acute: exactly what
// ChunkReader::readString hands back for a CP-1252 emitter name.
static const std::string kRawName = "\xE9" "efault";
// The same name after replacement: U+FFFD (EF BF BD) + "efault".
static const std::string kReplacedName = "\xEF\xBF\xBD" "efault";

static json EmitterNode(const std::string& name)
{
    return json{
        {"id", 0}, {"name", name}, {"role", "root"}, {"linkGroup", 0},
        {"visible", true}, {"children", json::array()},
    };
}

static bool DefaultDumpThrows316(const json& env)
{
    try
    {
        (void)env.dump();
    }
    catch (const json::type_error& e)
    {
        return e.id == 316;
    }
    return false;
}

static void TestResponseEnvelope()
{
    std::printf("emitters/list response with a non-UTF-8 emitter name\n");
    const json res = {
        {"type", "res"}, {"id", "r1"}, {"ok", true},
        {"data", json{{"root", json{{"id", -1}, {"name", ""}, {"role", "root"},
                                    {"children", json::array({EmitterNode(kRawName)})}}}}},
    };
    CHECK(DefaultDumpThrows316(res), "precondition: the default dump() throws type_error 316");

    std::string wire;
    bool threw = false;
    try { wire = host::SerializeBridgeEnvelope(res); } catch (...) { threw = true; }
    CHECK(!threw, "SerializeBridgeEnvelope does not throw");

    const json back = json::parse(wire, nullptr, false);
    CHECK(!back.is_discarded(), "the wire text is valid JSON");
    CHECK(back.value("ok", false) == true && back.value("id", std::string()) == "r1",
          "envelope keeps ok:true and its correlation id");
    const json& child = back["data"]["root"]["children"][0];
    CHECK(child.value("name", std::string()) == kReplacedName,
          "the bad byte became U+FFFD, the rest of the name is intact");
    CHECK(child.value("role", std::string()) == "root", "sibling fields survive");
}

static void TestEventEnvelope()
{
    std::printf("emitters/tree/changed event with a non-UTF-8 emitter name\n");
    const json evt = {
        {"type", "evt"}, {"kind", "emitters/tree/changed"},
        {"payload", json{{"root", json{{"children", json::array({EmitterNode(kRawName)})}}}}},
    };
    CHECK(DefaultDumpThrows316(evt), "precondition: the default dump() throws type_error 316");

    std::string wire;
    bool threw = false;
    try { wire = host::SerializeBridgeEnvelope(evt); } catch (...) { threw = true; }
    CHECK(!threw, "SerializeBridgeEnvelope does not throw");
    const json back = json::parse(wire, nullptr, false);
    CHECK(!back.is_discarded()
          && back["payload"]["root"]["children"][0].value("name", std::string()) == kReplacedName,
          "event payload parses and carries the replaced name");
}

static void TestValidUtf8Unchanged()
{
    std::printf("valid UTF-8 serializes exactly like the default dump()\n");
    const json env = {
        {"type", "evt"}, {"kind", "recent/changed"},
        {"payload", json{{"paths", json::array({"C:/fx/caf\xC3\xA9.alo", "plain.alo",
                                                "quote \" back\\slash \x01 ctl"})},
                         {"n", 3}, {"f", 0.25}, {"b", false}, {"z", nullptr}}},
    };
    CHECK(host::SerializeBridgeEnvelope(env) == env.dump(),
          "byte-identical to dump() for valid input (behaviour unchanged)");
}

static void TestGuardedDispatch()
{
    std::printf("RunGuardedDispatch\n");
    const json req = {{"type", "req"}, {"id", "q7"}, {"kind", "x/y"}, {"params", json::object()}};

    {
        int errors = 0;
        const json res = host::RunGuardedDispatch(req,
            [] { return json{{"type", "res"}, {"id", "q7"}, {"ok", true}, {"data", 1}}; },
            [&](const char*, const char*) { ++errors; });
        CHECK(errors == 0 && res.value("ok", false) && res["data"] == 1,
              "a normal handler's envelope passes through untouched");
    }
    {
        std::string category;
        const json res = host::RunGuardedDispatch(req,
            []() -> json { return json{{"n", json("not a number").get<int>()}}; },
            [&](const char* c, const char*) { category = c; });
        CHECK(category == "type/conversion", "json::exception is reported as type/conversion");
        CHECK(res.value("ok", true) == false && res.value("id", std::string()) == "q7",
              "json::exception -> ok:false with the request id");
        CHECK(res.value("error", std::string()).rfind("dispatch exception: ", 0) == 0,
              "error text keeps the existing 'dispatch exception: ' prefix");
    }
    {
        std::string category;
        const json res = host::RunGuardedDispatch(req,
            []() -> json { throw std::runtime_error("cannot open C:/fx/\xE9t\xE9.alo"); },
            [&](const char* c, const char*) { category = c; });
        CHECK(category == "std", "std::exception is caught (was uncaught before H1)");
        CHECK(res.value("ok", true) == false && res.value("id", std::string()) == "q7",
              "std::exception -> ok:false with the request id");
        bool threw = false;
        try { (void)host::SerializeBridgeEnvelope(res); } catch (...) { threw = true; }
        CHECK(!threw, "a non-UTF-8 exception message still serializes");
    }
    {
        std::string category;
        const json res = host::RunGuardedDispatch(req,
            []() -> json { throw 42; },
            [&](const char* c, const char*) { category = c; });
        CHECK(category == "unknown" && res.value("ok", true) == false,
              "a non-std exception is caught too");
    }
    {
        const json noId = {{"type", "req"}, {"kind", "x/y"}};
        const json res = host::RunGuardedDispatch(noId,
            []() -> json { throw std::runtime_error("boom"); },
            [](const char*, const char*) {});
        CHECK(res.contains("id") && res["id"].is_null(), "no request id -> id:null");
    }
    {
        const json numericId = {{"type", "req"}, {"id", 5}, {"kind", "x/y"}};
        const json res = host::BuildDispatchExceptionEnvelope(numericId, nullptr);
        CHECK(res["id"].is_null() && res.value("error", std::string()) == "dispatch exception: (no message)",
              "non-string id -> id:null; null what -> placeholder text");
    }
}

int main()
{
    std::printf("test_bridge_wire\n");
    TestResponseEnvelope();
    TestEventEnvelope();
    TestValidUtf8Unchanged();
    TestGuardedDispatch();
    std::printf(g_failed ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
