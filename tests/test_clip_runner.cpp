// Unit tests for src/host/ClipRunner.{h,cpp} — the per-frame state machine,
// driven with stub hooks (no host / WebView2 / D3D9).
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include "ClipRunner.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using host::ClipRunner;

int main() {
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

    if (g_fail) { std::printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    std::printf("all clip-runner tests passed\n");
    return 0;
}
