// Unit tests for src/host/ClipRunner.{h,cpp} — the per-frame state machine,
// driven with stub hooks (no host / WebView2 / D3D9).
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <crtdbg.h>
#include "ClipRunner.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

using host::ClipRunner;

int main() {
#ifdef _DEBUG
    // Route CRT assertions (e.g. a bad STL access in a failing test) to stderr
    // instead of a modal dialog — an unattended/subagent test run otherwise hangs
    // on the Abort/Retry/Ignore box. Fail fast (nonzero exit), never block.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif

    // Init rejects a bad timeline with exitCode 2.
    {
        ClipRunner r; std::string err;
        CHECK(!r.Init("not json", err));
        CHECK(r.ExitCode() == 2 && !err.empty());
    }
    // Init accepts a minimal valid timeline; FrameCount matches.
    {
        ClipRunner r; std::string err;
        const char* js = R"({"fps":60,"width":1920,"height":1080,"durationMs":100,"out":"o",
                            "tracks":[{"cursor":[{"t":0,"x":0,"y":0,"vis":true}]}]})";
        CHECK(r.Init(js, err));
        CHECK(r.FrameCount() == 6);  // round(100*60/1000)=6
    }
    // Full run: a 3-frame clip drives step/cursor/at/ack/capture in order.
    {
        ClipRunner r; std::string err;
        const char* js = R"({"fps":60,"width":1920,"height":1080,"durationMs":50,"out":"o",
          "tracks":[
            {"tween":"camera-orbit","from":{"yaw":0,"dist":100},"to":{"yaw":1},"t0":0,"t1":50,"ease":"linear"},
            {"cursor":[{"t":0,"x":0,"y":0,"vis":true},{"t":50,"x":50,"y":50,"vis":true}]},
            {"at":20,"kind":"emitters/select","params":{"stableId":"x"}}
          ]})";
        CHECK(r.Init(js, err));
        const int N = r.FrameCount();  // round(50*60/1000)=3
        CHECK(N == 3);

        std::vector<int> steps, captures; std::vector<std::string> dispatched; int acks = 0;
        r.SetHooks(
            [&](const std::string& req){ auto j=nlohmann::json::parse(req); dispatched.push_back(j["kind"]);
                                         return std::string(R"({"type":"res","ok":true,"data":{}})"); },
            [&](int f){ steps.push_back(f); },
            [&](double,double,bool,bool){},
            [&](int,double){ ++acks; return true; },
            [&](int idx){ captures.push_back(idx); return true; },
            [&](const std::string&){});

        int guard = 0;
        while (r.Tick() != ClipRunner::Status::Done) { if (++guard > 100) { CHECK(!"runaway"); break; } }
        CHECK(r.ExitCode() == 0);
        CHECK((int)steps.size() == N);                 // one clock step per frame
        CHECK(steps[0] == 1);                          // 60/fps = 1 for fps=60
        CHECK((int)captures.size() == N);              // one capture per frame
        CHECK(captures[0] == 0 && captures[2] == 2);   // sequential indices
        CHECK(acks == N);                              // one ack per frame
        int camCount = 0, selCount = 0;
        for (auto& k : dispatched) { if (k == "engine/set/camera") ++camCount; if (k == "emitters/select") ++selCount; }
        CHECK(camCount == N);
        CHECK(selCount == 1);
    }
    // A failed dispatch aborts with exit 3.
    {
        ClipRunner r; std::string err;
        const char* js = R"({"fps":60,"width":1920,"height":1080,"durationMs":50,"out":"o",
          "tracks":[{"at":0,"kind":"emitters/select","params":{"stableId":"x"}}]})";
        CHECK(r.Init(js, err));
        r.SetHooks(
            [&](const std::string&){ return std::string(R"({"type":"res","ok":false,"error":"boom"})"); },
            [&](int){}, [&](double,double,bool,bool){}, [&](int,double){return true;},
            [&](int){ return true; }, [&](const std::string&){});
        while (r.Tick() != ClipRunner::Status::Done) {}
        CHECK(r.ExitCode() == 3);
    }
    // A capture failure aborts with exit 4.
    {
        ClipRunner r; std::string err;
        const char* js = R"({"fps":60,"width":1920,"height":1080,"durationMs":50,"out":"o",
          "tracks":[{"cursor":[{"t":0,"x":0,"y":0,"vis":true}]}]})";
        CHECK(r.Init(js, err));
        r.SetHooks([&](const std::string&){ return std::string(R"({"type":"res","ok":true,"data":{}})"); },
            [&](int){}, [&](double,double,bool,bool){}, [&](int,double){return true;},
            [&](int){ return false; }, [&](const std::string&){});
        while (r.Tick() != ClipRunner::Status::Done) {}
        CHECK(r.ExitCode() == 4);
    }
    // An ack timeout is non-fatal (best-effort): still captures, exit 0.
    {
        ClipRunner r; std::string err;
        const char* js = R"({"fps":60,"width":1920,"height":1080,"durationMs":50,"out":"o",
          "tracks":[{"cursor":[{"t":0,"x":0,"y":0,"vis":true}]}]})";
        CHECK(r.Init(js, err));
        int caps = 0;
        r.SetHooks([&](const std::string&){ return std::string(R"({"type":"res","ok":true,"data":{}})"); },
            [&](int){}, [&](double,double,bool,bool){}, [&](int,double){return false;}, // ack ALWAYS times out
            [&](int){ ++caps; return true; }, [&](const std::string&){});
        while (r.Tick() != ClipRunner::Status::Done) {}
        CHECK(r.ExitCode() == 0 && caps == r.FrameCount());
    }

    // --- track-key tween emits a RAMPING set-track-key each frame ---------
    {
        ClipRunner r; std::string err;
        // 3 frames @ 60fps over 50ms; red border key 0 scrubs 1.0 -> 0.0 linearly.
        const char* js = R"({"fps":60,"width":1280,"height":720,"durationMs":50,"out":"o",
          "tracks":[{"tween":"track-key","id":7,"track":"red","keyTime":0,
                     "from":{"value":1.0},"to":{"value":0.0},"t0":0,"t1":50,"ease":"linear"}]})";
        CHECK(r.Init(js, err));
        std::vector<double> values; bool shapeOk = true;
        // get-tracks mock with a red key at time 0 — keeps the Task-3 preflight happy
        // once it lands (this test is forward-compatible with that task).
        const std::string tracksResp = R"({"type":"res","ok":true,"data":{"tracks":[
            {"name":"red","keys":[{"time":0.0,"value":1.0},{"time":100.0,"value":0.0}],"interpolation":"linear","lockedTo":null}]}})";
        r.SetHooks(
            [&](const std::string& req){
                auto j = nlohmann::json::parse(req);
                if (j["kind"] == "emitters/get-tracks") return tracksResp;
                if (j["kind"] == "emitters/set-track-key") {
                    const auto& p = j["params"];
                    if (p["id"].get<int>() != 7 || p["track"].get<std::string>() != "red"
                        || p["oldTime"].get<double>() != 0.0 || p["newTime"].get<double>() != 0.0)
                        shapeOk = false;                      // id/track set, time pinned at the border
                    values.push_back(p["newValue"].get<double>());
                }
                return std::string(R"({"type":"res","ok":true,"data":{}})"); },
            [&](int){}, [&](double,double,bool,bool){},
            [&](int,double){ return true; }, [&](int){ return true; }, [&](const std::string&){});
        int guard = 0;
        while (r.Tick() != ClipRunner::Status::Done) { if (++guard > 100) { CHECK(!"runaway"); break; } }
        CHECK(r.ExitCode() == 0);
        CHECK((int)values.size() == r.FrameCount());          // one emit per frame
        CHECK(shapeOk);                                       // params shaped right, time pinned
        CHECK(!values.empty());                               // guard the front()/back() below
        if (!values.empty()) {
            CHECK(Near(values.front(), 1.0));                 // starts at `from`
            CHECK(values.back() < values.front());            // and RAMPS down (a constant fails here)
        }
    }

    // --- preflight: missing key at keyTime -> exit 3, no frames ----------
    {
        // get-tracks mock returns red keys ONLY at 0 and 100; tween targets keyTime 50.
        auto tracksResp = std::string(R"({"type":"res","ok":true,"data":{"tracks":[
            {"name":"red","keys":[{"time":0.0,"value":1.0},{"time":100.0,"value":0.0}],"interpolation":"linear","lockedTo":null}]}})");
        ClipRunner r; std::string err;
        const char* js = R"({"fps":60,"width":1280,"height":720,"durationMs":50,"out":"o",
          "tracks":[{"tween":"track-key","id":2,"track":"red","keyTime":50,
                     "from":{"value":1.0},"to":{"value":0.0},"t0":0,"t1":50}]})";
        CHECK(r.Init(js, err));
        int caps = 0;
        r.SetHooks(
            [&](const std::string& req){
                auto j = nlohmann::json::parse(req);
                if (j["kind"] == "emitters/get-tracks") return tracksResp;
                return std::string(R"({"type":"res","ok":true,"data":{}})"); },
            [&](int){}, [&](double,double,bool,bool){},
            [&](int,double){ return true; }, [&](int){ ++caps; return true; }, [&](const std::string&){});
        while (r.Tick() != ClipRunner::Status::Done) {}
        CHECK(r.ExitCode() == 3);                              // preflight rejected
        CHECK(caps == 0);                                     // never captured a frame
    }
    // --- preflight: border key present -> proceeds normally --------------
    {
        auto tracksResp = std::string(R"({"type":"res","ok":true,"data":{"tracks":[
            {"name":"red","keys":[{"time":0.0,"value":1.0},{"time":100.0,"value":0.0}],"interpolation":"linear","lockedTo":null}]}})");
        ClipRunner r; std::string err;
        const char* js = R"({"fps":60,"width":1280,"height":720,"durationMs":50,"out":"o",
          "tracks":[{"tween":"track-key","id":2,"track":"red","keyTime":0,
                     "from":{"value":1.0},"to":{"value":0.0},"t0":0,"t1":50}]})";
        CHECK(r.Init(js, err));
        int caps = 0;
        r.SetHooks(
            [&](const std::string& req){
                auto j = nlohmann::json::parse(req);
                if (j["kind"] == "emitters/get-tracks") return tracksResp;
                return std::string(R"({"type":"res","ok":true,"data":{}})"); },
            [&](int){}, [&](double,double,bool,bool){},
            [&](int,double){ return true; }, [&](int){ ++caps; return true; }, [&](const std::string&){});
        while (r.Tick() != ClipRunner::Status::Done) {}
        CHECK(r.ExitCode() == 0 && caps == r.FrameCount());
    }

    if (g_fail) { std::printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    std::printf("all clip-runner tests passed\n");
    return 0;
}
