// Unit tests for src/host/ClipTimeline.h — the pure timeline model: parse,
// validation, tween + cursor evaluation, frame math. No host / WebView2 / D3D9.
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include "ClipTimeline.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

int main()
{
    using namespace clip;

    // Frame math: 4000ms @ 60fps -> 240 frames; frame N at N*1000/60 ms.
    {
        Timeline tl; tl.fps = 60; tl.durationMs = 4000;
        CHECK(FrameCount(tl) == 240);
        CHECK(Near(FrameTimeMs(tl, 0), 0.0));
        CHECK(Near(FrameTimeMs(tl, 239), 239 * 1000.0 / 60.0));
    }
    // Easing endpoints + monotonic midpoint.
    {
        CHECK(Near(ApplyEase(Ease::Linear, 0.0), 0.0));
        CHECK(Near(ApplyEase(Ease::Linear, 1.0), 1.0));
        CHECK(Near(ApplyEase(Ease::Linear, 0.5), 0.5));
        CHECK(Near(ApplyEase(Ease::InOutSine, 0.0), 0.0));
        CHECK(Near(ApplyEase(Ease::InOutSine, 1.0), 1.0));
        CHECK(Near(ApplyEase(Ease::InOutSine, 0.5), 0.5));
        CHECK(ApplyEase(Ease::InOutSine, 0.25) < 0.25);  // ease-in below the diagonal
    }
    // camera-orbit tween: yaw animates 0 -> pi, pitch/dist hold from `from`.
    {
        Tween tw; tw.name = "camera-orbit"; tw.t0 = 0; tw.t1 = 1000; tw.ease = Ease::Linear;
        tw.from = nlohmann::json{{"yaw", 0.0}, {"pitch", 0.0}, {"dist", 100.0}};
        tw.to   = nlohmann::json{{"yaw", 3.14159265358979323846}};
        auto a = EvalCameraOrbit(tw, 0.0);     // yaw=0 -> eye on +Y at dist
        CHECK(Near(a.position.x, 0.0) && Near(a.position.y, 100.0) && Near(a.position.z, 0.0));
        auto m = EvalCameraOrbit(tw, 500.0);   // yaw=pi/2 -> eye on +X
        CHECK(Near(m.position.x, 100.0, 1e-4) && Near(m.position.y, 0.0, 1e-4));
        auto b = EvalCameraOrbit(tw, 1000.0);  // yaw=pi -> eye on -Y
        CHECK(Near(b.position.y, -100.0, 1e-4));
    }
    // cursor interpolation: glide (0,0)->(100,200) over [0,1000]; press latches at a key.
    {
        std::vector<CursorKey> keys = {
            { 0,   0,   0,   false, false },
            { 1000,100, 200, true,  false },
            { 1000,100, 200, true,  true  },  // same t, press on
        };
        auto c0 = EvalCursor(keys, 0.0);    CHECK(Near(c0.x, 0) && Near(c0.y, 0) && !c0.vis);
        auto c5 = EvalCursor(keys, 500.0);  CHECK(Near(c5.x, 50) && Near(c5.y, 100) && c5.vis);
        auto c1 = EvalCursor(keys, 1000.0); CHECK(Near(c1.x, 100) && c1.press);
        auto cAfter = EvalCursor(keys, 2000.0); CHECK(Near(cAfter.x, 100));  // clamp past last
    }
    // Allowlist: engine/set/* ok, but engine/set/paused subtracted; step-frames rejected.
    {
        CHECK(IsAllowedRecordKind("engine/set/model-shadows"));
        CHECK(IsAllowedRecordKind("emitters/select"));
        CHECK(!IsAllowedRecordKind("engine/set/paused"));
        CHECK(!IsAllowedRecordKind("engine/action/step-frames"));
        CHECK(!IsAllowedRecordKind("file/save"));
    }
    // Happy-path parse with each track shape + open sugar.
    {
        const char* js = R"({
          "open":"C:\\s.alo","fps":60,"width":1920,"height":1080,"durationMs":4000,
          "out":"clip-out","openSettleMs":500,"loop":true,
          "tracks":[
            {"tween":"camera-orbit","from":{"yaw":0},"to":{"yaw":6.2832},"t0":0,"t1":4000,"ease":"inOutSine"},
            {"cursor":[{"t":0,"x":1,"y":2,"vis":false},{"t":500,"x":3,"y":4,"vis":true,"press":true}]},
            {"at":950,"kind":"emitters/select","params":{"stableId":"core"}}
          ]})";
        Timeline tl; std::string err;
        CHECK(ParseTimeline(js, tl, err));
        CHECK(err.empty());
        CHECK(tl.fps == 60 && tl.width == 1920 && tl.height == 1080);
        CHECK(tl.openPath == "C:\\s.alo");
        CHECK(tl.loop);
        CHECK(tl.tweens.size() == 1 && tl.tweens[0].name == "camera-orbit" && tl.tweens[0].ease == Ease::InOutSine);
        CHECK(tl.cursor.size() == 2 && tl.cursor[1].press);
        CHECK(tl.ats.size() == 1 && tl.ats[0].kind == "emitters/select" && Near(tl.ats[0].t, 950));
    }
    // at-events are sorted ascending after parse (forward-index firing depends on it).
    {
        const char* js = R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o",
          "tracks":[{"at":900,"kind":"emitters/list"},{"at":100,"kind":"emitters/list"}]})";
        Timeline tl; std::string err;
        CHECK(ParseTimeline(js, tl, err));
        CHECK(tl.ats.size() == 2 && Near(tl.ats[0].t, 100) && Near(tl.ats[1].t, 900));
    }
    // Validation rejections — each must return false with a non-empty err.
    {
        auto rejects = [](const char* js) { Timeline t; std::string e; return !ParseTimeline(js, t, e) && !e.empty(); };
        CHECK(rejects("not json"));
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080})"));                 // missing durationMs/out/tracks
        CHECK(rejects(R"({"fps":24,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[]})")); // fps !| 60
        CHECK(rejects(R"({"fps":60,"width":400,"height":1080,"durationMs":1000,"out":"o","tracks":[]})"));  // width < floor
        CHECK(rejects(R"({"fps":60,"width":1280,"height":200,"durationMs":1000,"out":"o","tracks":[]})"));  // height < floor
        { Timeline t; std::string e; CHECK(ParseTimeline(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o","tracks":[]})", t, e)); }  // 720p accepted
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":0,"out":"o","tracks":[]})"));    // durationMs <= 0
        CHECK(rejects(R"({"fps":30,"width":1920,"height":1080,"durationMs":10,"out":"o","tracks":[]})"));   // rounds to 0 frames
        CHECK(rejects(R"({"fps":30,"width":1920,"height":1080,"durationMs":1000,"out":"C:\\x","tracks":[]})")); // out absolute (drive)
        CHECK(rejects(R"({"fps":30,"width":1920,"height":1080,"durationMs":1000,"out":"/x","tracks":[]})"));    // out leading slash
        CHECK(rejects(R"({"fps":30,"width":1920,"height":1080,"durationMs":1000,"out":"a/../b","tracks":[]})")); // out parent-traversal
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"at":0,"kind":"file/save"}]})"));
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"at":0,"kind":"engine/set/paused","params":{"paused":true}}]})"));
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"at":0,"kind":"ui/cursor"}]})"));
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"at":0,"kind":"file/open","params":{}}]})"));
        CHECK(rejects(R"({"open":"a.alo","fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"at":0,"kind":"file/open","params":{"path":"b.alo"}}]})"));
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"cursor":[{"t":100},{"t":50}]}]})"));
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"at":0,"kind":"emitters/list","cursor":[]}]})"));
        CHECK(rejects(R"({"fps":60,"width":1920,"height":1080,"durationMs":1000,"out":"o","tracks":[{"tween":"zoom-blur","t0":0,"t1":100}]})"));
    }

    if (g_fail) { std::printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    std::printf("all clip-timeline tests passed\n");
    return 0;
}
