#include "DriveRunner.h"

#include <cassert>

// The Do*/Tick asserts below encode the invariant that ParseScript is the SOLE
// Step factory (it validates every step's required fields). They can only fire
// on a Step hand-built outside ParseScript, which no current path does.
namespace host {

bool DriveRunner::Init(const std::string& scriptJson, std::string& err)
{
    if (!drive::ParseScript(scriptJson, m_script, err)) { m_exitCode = 2; return false; }
    return true;
}

void DriveRunner::SetHooks(DispatchFn d, CaptureFn c, NowMsFn n, LogFn l)
{ m_dispatch = std::move(d); m_capture = std::move(c); m_now = std::move(n); m_log = std::move(l); }

double DriveRunner::TotalSettleMs() const
{
    double t = 0;
    for (const auto& s : m_script.steps) if (s.kind == drive::StepKind::Settle) t += s.settleMs;
    return t;
}

bool DriveRunner::DispatchKind(const std::string& kind, const nlohmann::json& params)
{
    const std::string env = drive::BuildRequestEnvelope(++m_reqId, kind, params);
    const std::string res = m_dispatch(env);
    if (drive::ClassifyResponse(res) != drive::Outcome::Ok) {
        if (m_log) m_log("drive: step '" + kind + "' FAILED: " + res);
        m_exitCode = 3;
        return false;
    }
    m_lastVisualChangeMs = m_now();
    // file/open reloads textures + swaps the whole system; give it a larger
    // floor so the scene actually paints before a capture (engine ReloadTextures
    // comment, BridgeDispatcher.cpp ~2804). Other setters use the default.
    m_preCaptureFloorMs = (kind == "file/open") ? 500.0 : m_script.defaultSettleMs;
    return true;
}

bool DriveRunner::DoBridge(const drive::Step& s)
{
    assert(!s.bridgeKind.empty());
    return DispatchKind(s.bridgeKind, s.params);
}

bool DriveRunner::DoCamera(const drive::Step& s)
{
    drive::CameraVecs cv = drive::ComputeOrbitCamera(s.yaw, s.pitch, s.dist, s.target);
    nlohmann::json params = {
        {"position", {cv.position.x, cv.position.y, cv.position.z}},
        {"target",   {cv.target.x,   cv.target.y,   cv.target.z}},
        {"up",       {cv.up.x,       cv.up.y,       cv.up.z}},
    };
    return DispatchKind("engine/set/camera", params);
}

bool DriveRunner::DoSelectEmitter(const drive::Step& s)
{
    assert(s.selName.empty() != s.selStableId.empty());   // exactly one (ParseScript-enforced)
    const std::string listEnv = m_dispatch(
        drive::BuildRequestEnvelope(++m_reqId, "emitters/list", nlohmann::json::object()));
    drive::EmitterResolve r = drive::ResolveEmitterId(listEnv, s.selName, s.selStableId);
    if (!r.id.has_value()) {
        if (m_log) m_log("drive: select-emitter no match (name='" + s.selName +
                         "' stableId='" + s.selStableId + "')");
        m_exitCode = 3; return false;
    }
    if (r.duplicateName && m_log)
        m_log("drive: select-emitter name not unique; taking first pre-order match");
    return DispatchKind("emitters/select", nlohmann::json{{"id", *r.id}});
}

bool DriveRunner::DoCapture(const drive::Step& s)
{
    // NOTE: this only fails on a capture-machinery error (null/blank window,
    // GDI exhaustion, PrintWindow FALSE, encode fail). A PrintWindow that
    // SUCCEEDS over a black-but-valid composite passes -> exit 0 does NOT
    // certify a non-black viewport. The non-black pixel gate is intentionally
    // the smoke's job (tasks/drive-smoke.ps1); a legitimately dark scene (space
    // backdrop) must not fail the runner.
    assert(!s.capturePath.empty());
    if (!m_capture(s.capturePath)) {
        if (m_log) m_log("drive: capture FAILED for " + s.capturePath);
        m_exitCode = 4; return false;
    }
    return true;
}

DriveRunner::Status DriveRunner::Tick()
{
    assert(m_dispatch && m_capture && m_now);   // SetHooks must precede Tick
    if (m_index >= m_script.steps.size() || m_exitCode != 0) return Status::Done;

    const drive::Step& s = m_script.steps[m_index];

    switch (s.kind) {
        case drive::StepKind::Settle: {
            if (!m_settleActive) { m_settleActive = true; m_settleDeadline = m_now() + s.settleMs; }
            if (m_now() < m_settleDeadline) return Status::Running;  // keep rendering
            m_settleActive = false;
            // A completed settle SATISFIES the pre-capture wait (no double-wait):
            // backdate so the next capture's floor is already met.
            m_lastVisualChangeMs = m_now() - m_preCaptureFloorMs;
            ++m_index;
            return Status::Running;
        }
        case drive::StepKind::Capture: {
            // pre-capture settle: wait the floor set by the last visual step
            // (defaultSettleMs, or 500 after a file/open) since it landed.
            if (m_now() - m_lastVisualChangeMs < m_preCaptureFloorMs) return Status::Running;
            DoCapture(s);  // sets exitCode on failure -> next Tick returns Done
            if (m_exitCode == 0) ++m_index;
            return m_exitCode == 0 ? Status::Running : Status::Done;
        }
        case drive::StepKind::Bridge:        { if (DoBridge(s))        ++m_index; return m_exitCode ? Status::Done : Status::Running; }
        case drive::StepKind::Camera:        { if (DoCamera(s))        ++m_index; return m_exitCode ? Status::Done : Status::Running; }
        case drive::StepKind::SelectEmitter: { if (DoSelectEmitter(s)) ++m_index; return m_exitCode ? Status::Done : Status::Running; }
    }
    return Status::Done;
}

}  // namespace host
