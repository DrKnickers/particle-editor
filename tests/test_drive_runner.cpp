// Unit tests for src/host/DriveRunner.{h,cpp} — the step machine, driven with
// stub hooks + a fake clock (no host / WebView2 / D3D9). Covers the failure
// surface (exit codes, abort-on-failure, timing floors) the live smoke can't.
#include <cstdio>
#include <string>
#include <vector>
#include "DriveRunner.h"

static int g_fail = 0;
static int g_check = 0;
#define CHECK(cond) do { ++g_check; if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using host::DriveRunner;

// A scripted stub bridge: returns a canned response per kind and records the
// ordered list of dispatched kinds (+ the last emitters/select id).
struct Bridge {
    std::vector<std::string> kinds;
    std::string okResp   = R"({"type":"res","ok":true,"data":{}})";
    std::string failResp = R"({"type":"res","ok":false,"error":"boom"})";
    std::string listResp =
        R"({"type":"res","ok":true,"data":{"root":{"id":-1,"stableId":0,"name":"",)"
        R"("children":[{"id":0,"stableId":10,"name":"smoke","children":[]},)"
        R"({"id":1,"stableId":11,"name":"spark","children":[]}]}}})";
    bool failNextBridge = false;
    bool malformedList  = false;
    int  lastSelectId   = -999;

    std::string operator()(const std::string& req) {
        auto j = nlohmann::json::parse(req);
        const std::string kind = j["kind"];
        kinds.push_back(kind);
        if (kind == "emitters/list") return malformedList ? std::string("garbage") : listResp;
        if (kind == "emitters/select") { lastSelectId = j["params"].value("id", -999); return okResp; }
        if (failNextBridge) return failResp;
        return okResp;
    }
};

// Run a runner to completion with a fake 16ms/frame clock. Returns ExitCode.
// `now` is shared so capture hooks can record the time they fired.
static int RunToDone(DriveRunner& r, double& now, int maxIters = 100000) {
    int i = 0;
    while (r.Tick() != DriveRunner::Status::Done) {
        now += 16.0;
        if (++i > maxIters) { CHECK(!"RunToDone exceeded iteration cap"); break; }
    }
    return r.ExitCode();
}

static bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines)
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

int main()
{
    // 1. Happy path: bridge + capture -> exit 0, capture fired once.
    {
        double now = 0; Bridge b; int captures = 0;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"kind":"engine/set/background","params":{"rgb":1}},{"capture":"x.png"}]})", err));
        r.SetHooks(std::ref(b), [&](const std::string&){ ++captures; return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 0);
        CHECK(b.kinds.size() == 1 && b.kinds[0] == "engine/set/background");
        CHECK(captures == 1);
    }

    // 2. Failing step -> exit 3, and LATER steps do NOT run (abort).
    {
        double now = 0; Bridge b; b.failNextBridge = true; int captures = 0;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"kind":"engine/set/background"},{"kind":"engine/set/bloom"},{"capture":"x.png"}]})", err));
        r.SetHooks(std::ref(b), [&](const std::string&){ ++captures; return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 3);
        CHECK(b.kinds.size() == 1);   // aborted after the first (failing) step
        CHECK(captures == 0);
    }

    // 3. Malformed response (unparseable) -> exit 3 (NOT silently Ok).
    {
        double now = 0;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"kind":"engine/set/bloom"}]})", err));
        r.SetHooks([](const std::string&){ return std::string("not json"); },
                   [](const std::string&){ return true; }, [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 3);
    }

    // 4. Capture machinery failure -> exit 4.
    {
        double now = 0; Bridge b;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"capture":"x.png"}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return false; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 4);
    }

    // 5. Bad script -> Init fails, exit code 2.
    {
        DriveRunner r; std::string err;
        CHECK(!r.Init(R"({"steps":[{"kind":"file/save"}]})", err));  // not allowlisted
        CHECK(r.ExitCode() == 2);
        DriveRunner r2; std::string err2;
        CHECK(!r2.Init("", err2));                                   // empty/unparseable
        CHECK(r2.ExitCode() == 2);
    }

    // 6. select-emitter: resolves the id from emitters/list, then selects it.
    {
        double now = 0; Bridge b;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"select-emitter":{"name":"spark"}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 0);
        CHECK(b.kinds.size() == 2 && b.kinds[0] == "emitters/list" && b.kinds[1] == "emitters/select");
        CHECK(b.lastSelectId == 1);   // "spark" -> id 1
    }

    // 7. select-emitter no match -> exit 3 (loud, not silent).
    {
        double now = 0; Bridge b;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"select-emitter":{"name":"nope"}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 3);
    }

    // 8. Pre-capture floor: default (~150) after a setter vs 500 after file/open.
    {
        double now = 0; Bridge b; double capAt = -1;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"kind":"engine/set/background"},{"capture":"x.png"}]})", err));
        r.SetHooks(std::ref(b), [&](const std::string&){ capAt = now; return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 0);
        CHECK(capAt >= 150.0 && capAt < 320.0);   // default floor honored, not 500
    }
    {
        double now = 0; Bridge b; double capAt = -1;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"kind":"file/open","params":{"path":"C:\\x.alo"}},{"capture":"x.png"}]})", err));
        r.SetHooks(std::ref(b), [&](const std::string&){ capAt = now; return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 0);
        CHECK(capAt >= 500.0);   // file/open raises the floor to 500
    }

    // 9. Settle SATISFIES the pre-capture wait (no double-wait): a settle(400)
    //    before a capture must not add another full floor on top.
    {
        double now = 0; Bridge b; double capAt = -1;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"kind":"engine/set/background"},{"settle":400},{"capture":"x.png"}]})", err));
        r.SetHooks(std::ref(b), [&](const std::string&){ capAt = now; return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 0);
        CHECK(capAt >= 400.0 && capAt < 500.0);   // ~settle end, NOT 400+150
    }

    // 10. assert-state dispatches through the bridge and compares data by JSON pointer.
    {
        double now = 0; Bridge b;
        b.okResp = R"({"type":"res","ok":true,"data":{"root":{"count":2}}})";
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-state":{"kind":"layout/scene-rect","path":"/root/count","equals":2}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 0);
        CHECK(b.kinds.size() == 1 && b.kinds[0] == "layout/scene-rect");
    }
    {
        double now = 0; Bridge b; std::vector<std::string> logs;
        b.okResp = R"({"type":"res","ok":true,"data":{"root":{"count":1}}})";
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-state":{"kind":"layout/scene-rect","path":"/root/count","equals":2}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [&](const std::string& line){ logs.push_back(line); });
        CHECK(RunToDone(r, now) == 6);
        CHECK(Contains(logs, "drive: assert-state mismatch"));
        CHECK(Contains(logs, "actual=1 expected=2"));
    }
    {
        double now = 0; Bridge b; std::vector<std::string> logs;
        b.okResp = R"({"type":"res","ok":true,"data":{"root":{}}})";
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-state":{"kind":"layout/scene-rect","path":"/root/count","equals":2}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [&](const std::string& line){ logs.push_back(line); });
        CHECK(RunToDone(r, now) == 6);
        CHECK(Contains(logs, "drive: assert-state unresolved"));
    }
    {
        double now = 0; Bridge b; b.failNextBridge = true;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-state":{"kind":"layout/scene-rect","path":"/root/count","equals":2}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 3);
    }

    // 11. assert-viewport-nonblack maps probe failures and dark regions to assertion failure.
    {
        double now = 0; Bridge b; double x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-viewport-nonblack":{"threshold":100,"region":[0.1,0.2,0.3,0.4]}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [](const std::string&){});
        r.SetProbeHook([&](double ax0, double ay0, double ax1, double ay1) {
            x0 = ax0; y0 = ay0; x1 = ax1; y1 = ay1; return 120;
        });
        CHECK(RunToDone(r, now) == 0);
        CHECK(x0 == 0.1 && y0 == 0.2 && x1 == 0.3 && y1 == 0.4);
    }
    {
        double now = 0; Bridge b; std::vector<std::string> logs;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-viewport-nonblack":{"threshold":100}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [&](const std::string& line){ logs.push_back(line); });
        r.SetProbeHook([](double, double, double, double) { return 99; });
        CHECK(RunToDone(r, now) == 6);
        CHECK(Contains(logs, "drive: viewport probe max=99 threshold=100"));
    }
    {
        double now = 0; Bridge b; std::vector<std::string> logs;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-viewport-nonblack":{}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [&](const std::string& line){ logs.push_back(line); });
        r.SetProbeHook([](double, double, double, double) { return -1; });
        CHECK(RunToDone(r, now) == 6);
        CHECK(Contains(logs, "drive: viewport probe failed"));
    }
    {
        double now = 0; Bridge b; std::vector<std::string> logs;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"assert-viewport-nonblack":{}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [&](const std::string& line){ logs.push_back(line); });
        CHECK(RunToDone(r, now) == 6);
        CHECK(Contains(logs, "drive: probe hook unavailable"));
    }

    // 12. bridge-selftest uses the selftest hook with the pinned timeout.
    {
        double now = 0; Bridge b; std::string seenKind; int seenTimeout = 0;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"bridge-selftest":{"kind":"emitters/list"}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [](const std::string&){});
        r.SetSelftestHook([&](const std::string& kind, int timeoutMs) {
            seenKind = kind; seenTimeout = timeoutMs; return true;
        });
        CHECK(RunToDone(r, now) == 0);
        CHECK(seenKind == "emitters/list" && seenTimeout == 10000);
    }
    {
        double now = 0; Bridge b; std::vector<std::string> logs;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"bridge-selftest":{"kind":"emitters/list"}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [&](const std::string& line){ logs.push_back(line); });
        r.SetSelftestHook([](const std::string&, int) { return false; });
        CHECK(RunToDone(r, now) == 6);
        CHECK(Contains(logs, "drive: bridge selftest failed"));
    }
    {
        double now = 0; Bridge b; std::vector<std::string> logs;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"bridge-selftest":{"kind":"emitters/list"}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [&](const std::string& line){ logs.push_back(line); });
        CHECK(RunToDone(r, now) == 6);
        CHECK(Contains(logs, "drive: selftest hook unavailable"));
    }

    // 13. TotalSettleMs sums settle steps (watchdog budget input).
    {
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"settle":200},{"kind":"engine/set/bloom"},{"settle":300}]})", err));
        CHECK(r.TotalSettleMs() == 500.0);
    }

    // 14. Camera step dispatches engine/set/camera (orbit -> concrete vectors).
    {
        double now = 0; Bridge b;
        DriveRunner r; std::string err;
        CHECK(r.Init(R"({"steps":[{"camera":{"yaw":0,"pitch":0,"dist":100}}]})", err));
        r.SetHooks(std::ref(b), [](const std::string&){ return true; },
                   [&]{ return now; }, [](const std::string&){});
        CHECK(RunToDone(r, now) == 0);
        CHECK(b.kinds.size() == 1 && b.kinds[0] == "engine/set/camera");
    }

    if (g_fail == 0) std::printf("=== DriveRunner: ALL PASS (%d checks) ===\n", g_check);
    else             std::printf("=== DriveRunner: %d FAIL (%d checks) ===\n", g_fail, g_check);
    return g_fail ? 1 : 0;
}
