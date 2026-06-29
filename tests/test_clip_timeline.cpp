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

    // --- track-key: parse (happy + bounds + camera-orbit regression) ------
    {
        const char* js = R"({"fps":30,"width":1280,"height":720,"durationMs":2000,"out":"o",
          "tracks":[
            {"tween":"track-key","id":3,"track":"red","keyTime":0,
             "from":{"value":1.0},"to":{"value":0.15},"t0":400,"t1":1600,"ease":"inOutSine"},
            {"tween":"camera-orbit","from":{"yaw":0},"to":{"yaw":6.2832},"t0":0,"t1":2000,"ease":"linear"}
          ]})";
        Timeline tl; std::string err;
        CHECK(ParseTimeline(js, tl, err));
        CHECK(tl.trackKeys.size() == 1 && tl.tweens.size() == 1);          // both arms parse
        CHECK(tl.trackKeys[0].id == 3 && tl.trackKeys[0].track == "red");
        CHECK(Near(tl.trackKeys[0].keyTime, 0.0) && Near(tl.trackKeys[0].fromValue, 1.0)
              && Near(tl.trackKeys[0].toValue, 0.15) && tl.trackKeys[0].ease == Ease::InOutSine);
        CHECK(tl.tweens[0].name == "camera-orbit");                        // regression: camera survives refactor
    }
    {
        auto rejects = [](const char* j){ Timeline t; std::string e; return !ParseTimeline(j,t,e) && !e.empty(); };
        // missing from.value / missing track
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o",
          "tracks":[{"tween":"track-key","id":1,"track":"red","keyTime":0,"to":{"value":0.5},"t0":0,"t1":100}]})"));
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o",
          "tracks":[{"tween":"track-key","id":1,"keyTime":0,"from":{"value":1},"to":{"value":0.5},"t0":0,"t1":100}]})"));
        // bounds: alpha out of 0..1, keyTime out of 0..100, negative id
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o",
          "tracks":[{"tween":"track-key","id":1,"track":"alpha","keyTime":0,"from":{"value":1},"to":{"value":1.5},"t0":0,"t1":100}]})"));
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o",
          "tracks":[{"tween":"track-key","id":1,"track":"red","keyTime":150,"from":{"value":1},"to":{"value":0},"t0":0,"t1":100}]})"));
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o",
          "tracks":[{"tween":"track-key","id":-1,"track":"red","keyTime":0,"from":{"value":1},"to":{"value":0},"t0":0,"t1":100}]})"));
        // scale must be >= 0; rotationSpeed may be negative (signed)
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o",
          "tracks":[{"tween":"track-key","id":1,"track":"scale","keyTime":0,"from":{"value":1},"to":{"value":-2},"t0":0,"t1":100}]})"));
        // index shares scale's >= 0 rule
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o","tracks":[{"tween":"track-key","id":1,"track":"index","keyTime":0,"from":{"value":0},"to":{"value":-1},"t0":0,"t1":100}]})"));
        // unknown track name rejected by the TrackKeyValueInRange guard
        CHECK(rejects(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o","tracks":[{"tween":"track-key","id":1,"track":"bogus","keyTime":0,"from":{"value":0.5},"to":{"value":0.5},"t0":0,"t1":100}]})"));
        Timeline t; std::string e;
        CHECK(ParseTimeline(R"({"fps":30,"width":1280,"height":720,"durationMs":1000,"out":"o",
          "tracks":[{"tween":"track-key","id":1,"track":"rotationSpeed","keyTime":0,"from":{"value":1},"to":{"value":-3},"t0":0,"t1":100}]})", t, e));
    }
    // --- track-key: eval (linear, inOutSine, signed, clamp) --------------
    {
        TrackKeyTween tk; tk.fromValue = 1.0; tk.toValue = 0.0; tk.t0 = 0; tk.t1 = 1000; tk.ease = Ease::Linear;
        CHECK(Near(EvalTrackKeyValue(tk, -100.0), 1.0));     // before t0 -> from (clamped)
        CHECK(Near(EvalTrackKeyValue(tk, 0.0), 1.0));        // at t0 -> from
        CHECK(Near(EvalTrackKeyValue(tk, 500.0), 0.5));      // linear midpoint
        CHECK(Near(EvalTrackKeyValue(tk, 1000.0), 0.0));     // at t1 -> to
        CHECK(Near(EvalTrackKeyValue(tk, 1500.0), 0.0));     // clamp past t1
        TrackKeyTween e; e.fromValue = 0.0; e.toValue = 100.0; e.t0 = 0; e.t1 = 1000; e.ease = Ease::InOutSine;
        CHECK(EvalTrackKeyValue(e, 250.0) < 25.0);           // eased: slower than linear early
        // inOutSine at u=0.25 = 100*(1-cos(pi/4))/2 = 100*(2-sqrt2)/4 ≈ 14.6447
        CHECK(Near(EvalTrackKeyValue(e, 250.0), 14.6447, 1e-3));
        CHECK(EvalTrackKeyValue(e, 750.0) > 75.0);           // faster late
        TrackKeyTween neg; neg.fromValue = 0.5; neg.toValue = -0.5; neg.t0 = 0; neg.t1 = 1000; neg.ease = Ease::Linear;
        CHECK(Near(EvalTrackKeyValue(neg, 500.0), 0.0));     // signed lerp, not clamped at 0
        CHECK(Near(EvalTrackKeyValue(neg, 1000.0), -0.5));
        TrackKeyTween z; z.fromValue = 0.2; z.toValue = 0.9; z.t0 = 500; z.t1 = 500; // zero-width
        CHECK(Near(EvalTrackKeyValue(z, 0.0), 0.9));         // t1<=t0 -> to (TweenU convention)
    }
    // --- record-only spawner allowlist (preview-instance creation) -------
    {
        // spawner commands are record-allowed (non-modal; create the preview instance)
        CHECK(IsAllowedRecordKind("spawner/start"));
        CHECK(IsAllowedRecordKind("spawner/trigger"));
        CHECK(IsAllowedRecordKind("spawner/stop"));
        // mods/set-layers is record-allowed too (clear the mod stack -> base-game asset)
        CHECK(IsAllowedRecordKind("mods/set-layers"));
        CHECK(!drive::IsAllowedBridgeKind("mods/set-layers"));
        // emitters/delete is record-allowed (drop an emitter for a render; .alo untouched)
        CHECK(IsAllowedRecordKind("emitters/delete"));
        CHECK(!drive::IsAllowedBridgeKind("emitters/delete"));
        // ...but record-only: NOT in the shared --drive allowlist
        CHECK(!drive::IsAllowedBridgeKind("spawner/start"));
        CHECK(!drive::IsAllowedBridgeKind("spawner/trigger"));
        CHECK(!drive::IsAllowedBridgeKind("spawner/stop"));
        // sanity: paused still blocked, a render-state setter still allowed
        CHECK(!IsAllowedRecordKind("engine/set/paused"));
        CHECK(IsAllowedRecordKind("engine/set/bloom-strength"));
    }

    if (g_fail) { std::printf("\n%d CHECK(s) FAILED\n", g_fail); return 1; }
    std::printf("all clip-timeline tests passed\n");
    return 0;
}
