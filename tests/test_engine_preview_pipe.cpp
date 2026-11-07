// Window-free protocol, path and orbit rules. These tests never connect to a host
// or write a preview copy; the worker and document tests own those boundaries.
#include "host/EnginePreviewPipe.h"
#include <cstdio>
#include <functional>

using namespace host::engine_preview;
using nlohmann::json;
namespace
{
constexpr double kAngleToleranceDegrees = 0.000001;
constexpr double kDiagonalUnits         = 10.0;
constexpr double kTooFarUnits           = kMaxDistanceUnits + 1;
int              g_failed               = 0;

void Check(bool passed, const char* message)
{
    if (passed) return;
    ++g_failed;
    std::printf("FAIL: %s\n", message);
}

void Refuses(const std::function<void()>& action, const char* message)
{
    try
    {
        action();
        Check(false, message);
    }
    catch (const Failure&)
    {
    }
}

void TestWire()
{
    std::int64_t      id   = 1;
    const std::string line = FrameRequest(id, "ping", json::object());
    Check(line.back() == '\n' && line.find('\n') == line.size() - 1, "one newline per request");
    Check(json::parse(line)["id"] == 1 && id == 2, "request id increments");
    Check(json::parse(FrameRequest(id, "preview-still", {{"label", "line\nbreak"}}))["id"] == 2,
          "embedded newline is escaped");
    Refuses([&]() -> void { FrameRequest(id, "ping", {{"text", std::string(kMaxLineBytes, 'x')}}); },
            "oversized request refused");
    Refuses([&]() -> void { FrameRequest(id, "ping", json::array()); }, "args must be an object");
    Check(ParseReply(R"({"id":1,"ok":true,"result":{}})", 1).is_object(), "success object accepted");
    const std::vector<std::string> invalid = {"[]",
                                              "null",
                                              "broken",
                                              R"({"id":2,"ok":true,"result":{}})",
                                              R"({"id":1.0,"ok":true,"result":{}})",
                                              R"({"id":1,"ok":1,"result":{}})",
                                              R"({"id":1,"ok":true,"result":[]})",
                                              R"({"id":null,"ok":false})",
                                              R"({"id":1,"ok":false,"error":{"code":1,"message":"bad"}})"};
    for (const std::string& reply : invalid)
        Refuses([&]() -> void { ParseReply(reply, 1); }, "malformed reply refused");
    Refuses([]() -> void { ParseReply(std::string(kMaxLineBytes + 1, ' '), 1); }, "oversized reply refused");
    for (const std::string& code : {"bad-args", "unsupported", "internal", "timeout", "not-found", "needs-reopen"})
    {
        try
        {
            ParseReply(json{{"id", 1}, {"ok", false}, {"error", {{"code", code}, {"message", "host detail"}}}}.dump(),
                       1);
            Check(false, "error throws");
        }
        catch (const Failure& error)
        {
            Check(error.code == code, "protocol error code preserved");
            Check(std::string(error.what()).find("host detail") != std::string::npos, "host reason preserved");
            Check(ErrorMessage(code, "") != ErrorMessage("unknown", ""), "every protocol error has a specific message");
        }
    }
    id = kMaxRequestId + 1;
    Refuses([&]() -> void { FrameRequest(id, "ping", json::object()); }, "ids stay in exact JSON integer range");
}

void TestPipes()
{
    Check(ParsePipePid(L"alamo-preview-42") == 42, "bare pipe name parsed");
    Check(ParsePipePid(L"alamo-preview-042") == 0, "noncanonical pid ignored");
    Check(ParsePipePid(L"\\\\.\\pipe\\alamo-preview-42") == 42, "full pipe name parsed");
    for (const std::wstring& name :
         {L"alamo-preview-", L"alamo-preview-0", L"alamo-preview-42-extra", L"other-42", L"alamo-preview-4294967296"})
        Check(ParsePipePid(name) == 0, "invalid pipe name ignored");
    Check(SelectPipe({L"other", L"alamo-preview-42"}) == L"\\\\.\\pipe\\alamo-preview-42", "sole host selected");
    Refuses([]() -> void { SelectPipe({L"other"}); }, "missing host refused");
    Refuses([]() -> void { SelectPipe({L"alamo-preview-1", L"alamo-preview-2"}); }, "multiple hosts require pid");
    Check(SelectPipe({L"alamo-preview-1", L"alamo-preview-2"}, 2) == L"\\\\.\\pipe\\alamo-preview-2",
          "explicit pid selects host");
}

void TestPaths()
{
    // Synthetic drive roots keep the policy fixture independent of any machine layout.
    const std::string              root    = std::string(1, 'Q') + ":\\";
    const std::string              install = root + "editor";
    const std::string              windows = root + "windows";
    const std::vector<std::string> games   = {root + "game", root + "other-game/"};
    const auto check = [&](const std::string& path, const std::string& name = "effect") -> std::string
    {
        return CheckPreviewPath(path, name, install, windows, games);
    };
    Check(check(root + "preview\\Effect.ALO").empty(), "absolute ASCII alo with matching name accepted");
    Check(check(root + "editor-other\\effect.alo").empty(), "protected root comparison respects separator boundary");
    for (const std::string& path : std::vector<std::string>{
             "effect.alo", root + "preview\\effect.png", root + "preview\\different.alo", root + "editor\\effect.alo",
             root + "WINDOWS\\effect.alo", root + "preview\\..\\effect.alo", root + "preview\\effect.alo:stream",
             root + "preview.\\effect.alo", root + "preview\\effect.alo ", root + "preview\\eff\xC3\xA9" + "ct.alo"})
        Check(!check(path).empty(), "unsafe preview path refused");
    Check(!CheckPreviewPath(root + "effect.alo", "effect", "", windows, games).empty(),
          "unknown install root fails closed");
    const std::string gameRefusal = "The preview copy must be outside the game folders.";
    Check(check(root + "game\\Data\\effect.alo") == gameRefusal, "game destination refused");
    Check(check(root + "GAME\\Data\\effect.alo") == gameRefusal, "game root comparison ignores case");
    Check(check(root + "game/Data/effect.alo") == gameRefusal, "forward-slash destination refused");
    Check(check(root + "other-game\\Data\\effect.alo") == gameRefusal, "forward-slash game root refused");
    Check(check(root + "game-other\\effect.alo").empty(), "game root comparison respects separator boundary");
    Check(!check(root + "preview\\con.alo", "con").empty(), "Windows device name refused");
    Check(!check(root + "preview:stream\\effect.alo").empty(), "parent alternate stream refused");
    Check(!check(root + "preview\\effect.alo\\").empty(), "a trailing separator cannot name a file");
}

void TestOrbit()
{
    struct AngleCase
    {
        std::array<double, 3> position;
        double                pitch;
        double                yaw;
    };
    // Host yaw grows clockwise seen from above (calibrated live), so a camera at +X is -90.
    const AngleCase cases[] = {{{0, -kDiagonalUnits, 0}, 0, 0},
                               {{kDiagonalUnits, 0, 0}, 0, -90},
                               {{0, kDiagonalUnits, 0}, 0, -180},
                               {{-kDiagonalUnits, 0, 0}, 0, 90},
                               {{0, -kDiagonalUnits, kDiagonalUnits}, 45, 0}};
    for (const AngleCase& expected : cases)
    {
        const Orbit orbit = OrbitFromLookAt(expected.position, {0, 0, 0});
        Check(std::abs(orbit.pitch - expected.pitch) < kAngleToleranceDegrees, "pitch angle table");
        Check(std::abs(orbit.yaw - expected.yaw) < kAngleToleranceDegrees, "yaw angle table");
    }
    const Orbit shifted = OrbitFromLookAt({1, 0, 1}, {1, 1, 1});
    Check(shifted.distance == 1 && shifted.target == std::array<double, 3>{1, 1, 1},
          "target preserved and distance derived");
    Refuses([]() -> void { OrbitFromLookAt({0, 0, 0}, {0, 0, 0}); }, "coincident camera refused");
    Refuses([]() -> void { OrbitFromLookAt({0, 0, 1}, {0, 0, 0}); }, "vertical camera outside pitch range refused");
    Refuses([]() -> void { OrbitFromLookAt({0, -1, -1}, {0, 0, 0}); }, "below-ground camera refused");
    Refuses([]() -> void { OrbitFromLookAt({kTooFarUnits, 0, 0}, {0, 0, 0}); }, "distance limit enforced");
    Refuses([]() -> void { OrbitFromLookAt({0, -1, 0}, {0, (std::numeric_limits<double>::infinity)(), 0}); },
            "nonfinite target refused");
}
} // namespace
int main()
{
    TestWire();
    TestPipes();
    TestPaths();
    TestOrbit();
    std::printf("engine preview pipe: %s\n", g_failed == 0 ? "ALL PASS" : "FAILED");
    return g_failed == 0 ? 0 : 1;
}
