// Unit tests for src/host/DriveScript.h pure helpers.
#include <cstdio>
#include <cmath>
#include <string>
#include "DriveScript.h"

static int g_fail = 0;
static int g_check = 0;
#define CHECK(cond) do { ++g_check; if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main()
{
    using namespace drive;

    // --- minimal valid script: one bridge step parses ---
    {
        Script s; std::string err;
        const bool ok = ParseScript(
            R"({"steps":[{"kind":"engine/set/background","params":{"rgb":1234}}]})",
            s, err);
        CHECK(ok);
        CHECK(err.empty());
        CHECK(s.steps.size() == 1);
        CHECK(s.steps[0].kind == StepKind::Bridge);
        CHECK(s.steps[0].bridgeKind == "engine/set/background");
        CHECK(s.defaultSettleMs == 150.0);  // default
    }

    // --- allowlist: explicit render-safe commands, file/open(+path), emitters/list/select, texture preview OK ---
    {
        Script s; std::string err;
        CHECK(ParseScript(R"({"steps":[{"kind":"engine/set/camera","params":{}}]})", s, err));
        CHECK(ParseScript(R"({"steps":[{"kind":"engine/set/background","params":{"rgb":1234}}]})", s, err));
        CHECK(ParseScript(R"({"steps":[{"kind":"emitters/list"}]})", s, err));
        CHECK(ParseScript(R"({"steps":[{"kind":"emitters/select","params":{"id":0}}]})", s, err));
        CHECK(ParseScript(R"({"steps":[{"kind":"layout/scene-rect","params":{"x":0,"y":0,"w":800,"h":600}}]})", s, err));
        CHECK(ParseScript(R"({"steps":[{"kind":"textures/get-preview","params":{"filename":"p_fire.dds"}}]})", s, err));
        // read-only state query — the assert-state source for the ground-view-only
        // drive check (#617). Snapshot data carries /dirty, /ground, etc.
        CHECK(ParseScript(R"({"steps":[{"kind":"engine/state/snapshot"}]})", s, err));
        CHECK(ParseScript(R"({"steps":[{"assert-state":{"kind":"engine/state/snapshot","path":"/dirty","equals":false}}]})", s, err));
    }
    // --- allowlist: dangerous/unknown kinds REJECTED ---
    {
        Script s; std::string err;
        CHECK(!ParseScript(R"({"steps":[{"kind":"file/save"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"file/save-as"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"textures/browse"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"app/quit"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"mods/set-layers"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"debug/capture-window"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"emitters/delete"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"engine/set/leave-particles","params":{"enabled":true}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"engine/set/ground-slot-custom-path","params":{"path":"C:\\x.dds"}}]})", s, err));
    }
    // --- file/open requires an explicit path ---
    {
        Script s; std::string err;
        CHECK(!ParseScript(R"({"steps":[{"kind":"file/open","params":{}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"kind":"file/open"}]})", s, err));
        CHECK(ParseScript(R"({"steps":[{"kind":"file/open","params":{"path":"C:\\x.alo"}}]})", s, err));
    }
    // --- ...and the assertion steps can't smuggle an empty-path file/open past
    //     that guard (the modal native dialog would deadlock an unattended run) ---
    {
        Script s; std::string err;
        CHECK(!ParseScript(
            R"({"steps":[{"assert-state":{"kind":"file/open","path":"/ok","equals":true}}]})", s, err));
        CHECK(ParseScript(
            R"({"steps":[{"assert-state":{"kind":"file/open","params":{"path":"C:\\x.alo"},"path":"/ok","equals":true}}]})",
            s, err));
        CHECK(!ParseScript(R"({"steps":[{"bridge-selftest":{"kind":"file/open"}}]})", s, err));
    }
    // --- `open` sugar expands to a leading file/open; collision with explicit file/open rejected ---
    {
        Script s; std::string err;
        CHECK(ParseScript(R"({"open":"C:\\x.alo","steps":[{"kind":"emitters/list"}]})", s, err));
        CHECK(s.steps.size() == 2);
        CHECK(s.steps[0].kind == StepKind::Bridge);
        CHECK(s.steps[0].bridgeKind == "file/open");
        CHECK(s.steps[0].params["path"] == "C:\\x.alo");
        CHECK(!ParseScript(
            R"({"open":"C:\\x.alo","steps":[{"kind":"file/open","params":{"path":"C:\\y.alo"}}]})",
            s, err));  // both present -> reject
    }

    // --- capture / settle / camera / select-emitter step shapes ---
    {
        Script s; std::string err;
        CHECK(ParseScript(R"({"steps":[{"capture":"shot.png"}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::Capture);
        CHECK(s.steps[0].capturePath == "shot.png");

        CHECK(ParseScript(R"({"steps":[{"settle":200}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::Settle);
        CHECK(s.steps[0].settleMs == 200.0);

        CHECK(ParseScript(R"({"steps":[{"camera":{"yaw":0.5,"pitch":-0.3,"dist":240}}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::Camera);
        CHECK(s.steps[0].dist == 240.0);

        CHECK(ParseScript(R"({"steps":[{"select-emitter":{"name":"smoke"}}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::SelectEmitter);
        CHECK(s.steps[0].selName == "smoke");

        CHECK(ParseScript(R"({"steps":[{"select-emitter":{"stableId":"abc"}}]})", s, err));
        CHECK(s.steps[0].selStableId == "abc");

        // capture requires a string path
        CHECK(!ParseScript(R"({"steps":[{"capture":123}]})", s, err));
        // capture paths are artifact-relative only
        CHECK(!ParseScript(R"({"steps":[{"capture":"C:\\shot.png"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"capture":"..\\shot.png"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"capture":"nested/../shot.png"}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"capture":"\\\\server\\share\\shot.png"}]})", s, err));
        // select-emitter requires name OR stableId
        CHECK(!ParseScript(R"({"steps":[{"select-emitter":{}}]})", s, err));
        // select-emitter rejects BOTH name AND stableId (exactly-one invariant)
        CHECK(!ParseScript(R"({"steps":[{"select-emitter":{"name":"a","stableId":"b"}}]})", s, err));
        // fit is NOT supported in v1 (descoped) -> ignored, orbit still parses
        CHECK(ParseScript(R"({"steps":[{"camera":{"yaw":0,"pitch":0,"dist":10,"fit":true}}]})", s, err));
        // camera.target parsing (3-element array)
        CHECK(ParseScript(R"({"steps":[{"camera":{"dist":10,"target":[1,2,3]}}]})", s, err));
        CHECK(s.steps[0].target.x == 1.0 && s.steps[0].target.y == 2.0 && s.steps[0].target.z == 3.0);
    }

    // --- assertion/selftest step shapes: defaults and explicit fields ---
    {
        Script s; std::string err;
        CHECK(ParseScript(R"({"steps":[{"assert-state":{"kind":"emitters/list","path":"/root","equals":{"children":[]}}}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::AssertState);
        CHECK(s.steps[0].bridgeKind == "emitters/list");
        CHECK(s.steps[0].params.is_object() && s.steps[0].params.empty());
        CHECK(s.steps[0].assertPath == "/root");
        CHECK(s.steps[0].assertEquals == nlohmann::json({{"children", nlohmann::json::array()}}));

        CHECK(ParseScript(R"({"steps":[{"assert-state":{"kind":"emitters/select","params":{"id":7},"path":"/selected","equals":true}}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::AssertState);
        CHECK(s.steps[0].bridgeKind == "emitters/select");
        CHECK(s.steps[0].params["id"] == 7);
        CHECK(s.steps[0].assertPath == "/selected");
        CHECK(s.steps[0].assertEquals == true);

        CHECK(ParseScript(R"({"steps":[{"assert-viewport-nonblack":{}}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::AssertViewportNonblack);
        CHECK(s.steps[0].viewportThreshold == 96);
        CHECK(s.steps[0].regionX0 == 0.30 && s.steps[0].regionY0 == 0.16);
        CHECK(s.steps[0].regionX1 == 0.75 && s.steps[0].regionY1 == 0.64);

        CHECK(ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"threshold":200,"region":[0.1,0.2,0.3,0.4]}}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::AssertViewportNonblack);
        CHECK(s.steps[0].viewportThreshold == 200);
        CHECK(s.steps[0].regionX0 == 0.1 && s.steps[0].regionY0 == 0.2);
        CHECK(s.steps[0].regionX1 == 0.3 && s.steps[0].regionY1 == 0.4);

        CHECK(ParseScript(R"({"steps":[{"bridge-selftest":{"kind":"emitters/list"}}]})", s, err));
        CHECK(s.steps[0].kind == StepKind::BridgeSelftest);
        CHECK(s.steps[0].bridgeKind == "emitters/list");
    }

    // --- assertion/selftest validation failures ---
    {
        Script s; std::string err;
        CHECK(!ParseScript(R"({"steps":[{"assert-state":{"kind":"file/save","path":"/x","equals":1}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"bridge-selftest":{"kind":"file/save"}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-state":{"kind":"emitters/list","path":"root","equals":1}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-state":{"kind":"emitters/list","path":"/bad~2escape","equals":1}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-state":{"kind":"emitters/list","path":"/x"}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-state":{"kind":"emitters/list","params":[1],"path":"/x","equals":1}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"threshold":0}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"threshold":766}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"threshold":96.5}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"region":[0.1,0.2,0.3]}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"region":[0.5,0.2,0.5,0.4]}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"region":[-0.1,0.2,0.3,0.4]}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"assert-viewport-nonblack":{"region":[0.1,0.2,1.1,0.4]}}]})", s, err));
    }

    // --- parse edge cases ---
    {
        Script s; std::string err;
        // settleMs default override
        CHECK(ParseScript(R"({"settleMs":300,"steps":[{"settle":1}]})", s, err) && s.defaultSettleMs == 300.0);
        CHECK(!ParseScript(R"({"settleMs":-1,"steps":[]})", s, err));
        CHECK(!ParseScript(R"({"settleMs":60001,"steps":[]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"settle":-1}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"settle":60001}]})", s, err));
        // non-object root rejected
        CHECK(!ParseScript("[1,2,3]", s, err));
        CHECK(!ParseScript("42", s, err));
        // missing steps array rejected
        CHECK(!ParseScript(R"({"open":"C:\\x.alo"})", s, err));
        // empty steps array is accepted (pins the current no-op contract)
        CHECK(ParseScript(R"({"steps":[]})", s, err) && s.steps.empty());
    }

    // --- malformed typed fields reject without throwing ---
    {
        Script s; std::string err;
        CHECK(!ParseScript(R"({"steps":[{"kind":123}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"camera":{"target":[1,"bad",3]}}]})", s, err));
        CHECK(!ParseScript(R"({"steps":[{"select-emitter":{"name":42}}]})", s, err));
    }

    // --- ClassifyResponse failure-contract edge cases ---
    {
        // missing/empty/non-boolean ok -> Malformed (must NOT classify as Ok)
        CHECK(ClassifyResponse("{}") == Outcome::Malformed);
        CHECK(ClassifyResponse(R"({"data":{}})") == Outcome::Malformed);
        CHECK(ClassifyResponse(R"({"ok":"yes"})") == Outcome::Malformed);
    }
    // --- BuildRequestEnvelope null params -> {} ---
    {
        auto j = nlohmann::json::parse(BuildRequestEnvelope(1, "emitters/list", nlohmann::json()));
        CHECK(j["params"].is_object() && j["params"].empty());
    }
    // --- ResolveEmitterId malformed inputs -> not found (no crash) ---
    {
        CHECK(!ResolveEmitterId(R"({"ok":true,"data":{}})", "x", "").id.has_value());      // no root
        CHECK(!ResolveEmitterId("garbage", "x", "").id.has_value());                        // parse fail
        CHECK(!ResolveEmitterId(R"({"data":{"root":{"children":[]}}})", "x", "").id.has_value());  // empty tree
    }

    // --- request envelope wrapping ---
    {
        const std::string env = BuildRequestEnvelope(7, "engine/set/background",
                                                      nlohmann::json{{"rgb", 42}});
        auto j = nlohmann::json::parse(env);
        CHECK(j["type"] == "req");
        CHECK(j["id"] == "7");
        CHECK(j["kind"] == "engine/set/background");
        CHECK(j["params"]["rgb"] == 42);
    }
    // --- response classification: success / top-level fail / NESTED fail / malformed ---
    {
        CHECK(ClassifyResponse(R"({"type":"res","ok":true,"data":{"x":1}})") == Outcome::Ok);
        CHECK(ClassifyResponse(R"({"type":"res","ok":false,"error":"boom"})") == Outcome::Failed);
        // file/open failure shape: top-level ok:true, NESTED data.ok:false
        CHECK(ClassifyResponse(R"({"type":"res","ok":true,"data":{"ok":false,"error":"no file"}})") == Outcome::Failed);
        CHECK(ClassifyResponse(R"({"type":"res","ok":true,"data":{"ok":true}})") == Outcome::Ok);
        // engine/set/* success returns sendOk(json::object()) -> data:{} (no inner ok)
        CHECK(ClassifyResponse(R"({"type":"res","ok":true,"data":{}})") == Outcome::Ok);
        CHECK(ClassifyResponse("not json") == Outcome::Malformed);
    }

    // --- orbit camera math (ENGINE IS Z-UP) ---
    // Convention: pitch = elevation above the XY (horizon) plane, yaw = azimuth
    // about Z. yaw=pitch=0 -> eye on +Y at the horizon (z=0); pitch raises +Z.
    {
        CameraVecs cv = ComputeOrbitCamera(0.0, 0.0, 100.0, Vec3d{0,0,0});
        CHECK(std::abs(cv.position.y - 100.0) < 1e-6);     // eye on +Y at the horizon
        CHECK(std::abs(cv.position.x) < 1e-6);
        CHECK(std::abs(cv.position.z) < 1e-6);
        CHECK(std::abs(cv.target.x) < 1e-6 && std::abs(cv.target.y) < 1e-6 && std::abs(cv.target.z) < 1e-6);
        CHECK(std::abs(cv.up.z - 1.0) < 1e-6);             // Z-up
        // pitch>0 raises the eye toward +Z (the pole)
        CameraVecs cp = ComputeOrbitCamera(0.0, 0.5, 100.0, Vec3d{0,0,0});
        CHECK(cp.position.z > 0.0);
    }
    // --- emitter resolve: REAL shape data.root.children; NUMERIC stableId; pre-order; warn on dup ---
    {
        const char* list = R"({"type":"res","ok":true,"data":{"root":{
            "id":-1,"stableId":0,"name":"","children":[
                {"id":0,"stableId":10,"name":"smoke","children":[
                    {"id":1,"stableId":11,"name":"spark","children":[]}]},
                {"id":2,"stableId":12,"name":"smoke","children":[]}
            ]}}})";
        EmitterResolve r1 = ResolveEmitterId(list, "", "11");     // by stableId (numeric on wire, string in)
        CHECK(r1.id.has_value() && *r1.id == 1);
        EmitterResolve r2 = ResolveEmitterId(list, "spark", "");  // by name, unique
        CHECK(r2.id.has_value() && *r2.id == 1 && !r2.duplicateName);
        EmitterResolve r3 = ResolveEmitterId(list, "smoke", "");  // by name, duplicate
        CHECK(r3.id.has_value() && *r3.id == 0 && r3.duplicateName);  // first pre-order
        EmitterResolve r4 = ResolveEmitterId(list, "missing", "");
        CHECK(!r4.id.has_value());
    }

    if (g_fail == 0) std::printf("=== DriveScript: ALL PASS (%d checks) ===\n", g_check);
    else             std::printf("=== DriveScript: %d FAIL (%d checks) ===\n", g_fail, g_check);
    return g_fail ? 1 : 0;
}
