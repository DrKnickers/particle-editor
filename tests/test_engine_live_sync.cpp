// Live engine view command policy: newest-wins camera, debounced refresh and re-park, resume
// after a still, camera latency, and the clamped live orbit. No pipe, host or engine.
#include "host/EngineLiveSync.h"
#include <cstdio>

using namespace host::engine_live;
using host::engine_preview::Orbit;
namespace
{
constexpr std::uint64_t kStartMs            = 1000;
constexpr int           kWidthPx            = 1280;
constexpr int           kHeightPx           = 720;
constexpr int           kWiderPx            = 1400;
constexpr double        kPitchDegrees       = 34.0;
constexpr double        kFarUnits           = 900.0;
constexpr double        kNearUnits          = 600.0;
constexpr std::uint64_t kFrameLagMs         = 40;
constexpr double        kSteepDegrees       = 89.0;
constexpr double        kToleranceUnit      = 0.000001;
constexpr std::uint64_t kFramesPerWindow    = 30;
constexpr double        kNarrowFieldDegrees = 40.0;
constexpr double        kFieldMinDegrees    = 20.0;
constexpr double        kEyeHeightUnits     = 600.0;
constexpr double        kRaisedTargetUnits  = 200.0;
constexpr double        kGroundedTargetY    = 450.0;
constexpr double        kUnscaled           = 1.0;
constexpr double        kDoubleDistance     = 2.0;
// Drops 0.001 units over 900 across: the ray meets the ground about 540,000 units out.
constexpr double kSlightDropUnits = 0.001;
constexpr double kAnchorX         = 2048.0;
constexpr double kAnchorY         = 1536.0;
constexpr double kAnchorZ         = 12.0;
int              g_failed         = 0;

void Check(bool passed, const char* message)
{
    if (passed) return;
    ++g_failed;
    std::printf("FAIL: %s\n", message);
}

Orbit MakeOrbit(double distance)
{
    return Orbit{kPitchDegrees, 0.0, distance, {0.0, 0.0, 0.0}};
}

// The first camera waits for the host's opening target; these tests start past that step.
EngineLiveSync MakeAnchoredSync()
{
    EngineLiveSync sync;
    sync.TakeNextCommand(kStartMs);
    sync.NoteAnchor({0.0, 0.0, 0.0}, std::nullopt);
    return sync;
}

void TestGroundWaitsForOtherWorkAndCoalesces()
{
    EngineLiveSync sync;
    sync.NoteGround("dirt", "old.dds", "old_B.dds");
    sync.NoteGround("grass", "new.dds", "new_B.dds");
    sync.NoteViewportSize(kWidthPx, kHeightPx, kStartMs);
    sync.NoteDocumentEdited(kStartMs);
    sync.NoteStillFinished();
    sync.NoteCamera(MakeOrbit(kFarUnits), kHostDefaultFieldDegrees);
    const std::uint64_t dueMs = kStartMs + kRefreshDebounceMs;
    Check(sync.TakeNextCommand(dueMs)->command == "window", "park precedes ground");
    Check(sync.TakeNextCommand(dueMs)->command == "live-refresh", "refresh precedes ground");
    Check(sync.TakeNextCommand(dueMs)->command == "clock", "resume precedes ground");
    const std::optional<LiveCommand> anchor = sync.TakeNextCommand(dueMs);
    Check(anchor && sync.IsAnchorRequest(*anchor), "anchor precedes ground");
    Check(!sync.TakeNextCommand(dueMs), "ground waits for the anchor reply");
    sync.NoteAnchor({0.0, 0.0, 0.0}, std::nullopt);
    Check(sync.TakeNextCommand(dueMs)->command == "camera", "camera precedes ground");
    const std::optional<LiveCommand> ground = sync.TakeNextCommand(dueMs);
    Check(ground && ground->command == "scene" && ground->args["action"] == "ground", "ground uses scene");
    Check(ground && ground->args["color"] == "new.dds" && ground->args["normal"] == "new_B.dds",
          "only the newest ground pair is sent");
    sync.NoteGroundFinished(true);
    Check(sync.IsGroundCurrent("grass") && !sync.IsGroundCurrent("snow"), "the applied ground is current");
    sync.NoteGround("grass", "grass-again.dds", "grass-again_B.dds");
    Check(!sync.TakeNextCommand(dueMs), "an applied ground is not resent under a new export path");
}

void TestGroundWaitsForDebounceAndRetriesOnlyOnChange()
{
    EngineLiveSync sync = MakeAnchoredSync();
    sync.NoteGround("sand", "color.dds", "");
    sync.NoteDocumentEdited(kStartMs);
    Check(!sync.TakeNextCommand(kStartMs), "ground does not delay an edit waiting for debounce");
    sync.TakeNextCommand(kStartMs + kRefreshDebounceMs);
    const std::optional<LiveCommand> ground = sync.TakeNextCommand(kStartMs + kRefreshDebounceMs);
    Check(ground && !ground->args.contains("normal"), "missing companion uses the host's flat normal");
    sync.NoteGroundFinished(false);
    Check(!sync.TakeNextCommand(kStartMs), "a failed ground has no retry loop");
    Check(!sync.IsGroundCurrent("sand"), "a failed ground is not current");
    sync.NoteGround("sand", "color.dds", "");
    Check(sync.TakeNextCommand(kStartMs).has_value(), "a later change retries a failed ground");
    sync.NoteGround("snow", "later.dds", "");
    sync.NoteGroundUnsupported();
    sync.NoteGround("dirt", "ignored.dds", "");
    Check(!sync.IsGroundAvailable() && !sync.TakeNextCommand(kStartMs), "unsupported disables this session");
    sync.NoteCamera(MakeOrbit(kNearUnits), kHostDefaultFieldDegrees);
    Check(sync.TakeNextCommand(kStartMs)->command == "camera", "unsupported ground keeps the live camera");
}

void TestGroundChangedDuringACommandKeepsTheLatest()
{
    EngineLiveSync sync = MakeAnchoredSync();
    sync.NoteGround("dirt", "first.dds", "");
    sync.TakeNextCommand(kStartMs);
    sync.NoteGround("grass", "second.dds", "");
    sync.NoteGround("snow", "third.dds", "");
    Check(sync.IsGroundCurrent("snow"), "the queued ground is current");
    sync.NoteGroundFinished(true);
    const std::optional<LiveCommand> latest = sync.TakeNextCommand(kStartMs);
    Check(latest && latest->args["color"] == "third.dds", "edits during ground apply keep only the latest");
    sync.NoteGroundFinished(true);
    sync.NoteGround("snow", "third-again.dds", "");
    Check(!sync.TakeNextCommand(kStartMs), "the completed latest ground is not resent");
}

void TestCameraIsRelativeToTheOpeningTarget()
{
    EngineLiveSync sync;
    sync.NoteCamera(MakeOrbit(kFarUnits), kHostDefaultFieldDegrees);
    const std::optional<LiveCommand> anchor = sync.TakeNextCommand(kStartMs);
    Check(anchor && sync.IsAnchorRequest(*anchor), "the opening camera is read before any camera is sent");
    Check(!sync.TakeNextCommand(kStartMs), "the camera waits for the opening target");
    sync.NoteAnchor({kAnchorX, kAnchorY, kAnchorZ}, std::nullopt);
    const std::optional<LiveCommand> camera = sync.TakeNextCommand(kStartMs);
    Check(camera && camera->args["target"] == nlohmann::json::array({kAnchorX, kAnchorY, kAnchorZ}),
          "an effect-centred camera aims at the effect's place on the map");
}

void TestFailedAnchorIsAskedAgain()
{
    EngineLiveSync sync;
    sync.TakeNextCommand(kStartMs);
    sync.NoteAnchorFailed();
    const std::optional<LiveCommand> retry = sync.TakeNextCommand(kStartMs);
    Check(retry && sync.IsAnchorRequest(*retry), "a failed opening-camera read is retried");
}

void TestCameraKeepsOnlyTheNewest()
{
    EngineLiveSync sync = MakeAnchoredSync();
    sync.NoteCamera(MakeOrbit(kFarUnits), kHostDefaultFieldDegrees);
    sync.NoteCamera(MakeOrbit(kNearUnits), kHostDefaultFieldDegrees);
    const std::optional<LiveCommand> first = sync.TakeNextCommand(kStartMs);
    Check(first && first->command == "camera" && first->args["distance"] == kNearUnits,
          "only the newest camera is sent");
    Check(first && first->args["tag"] == 1, "the first camera carries tag 1");
    Check(!sync.TakeNextCommand(kStartMs), "nothing else waits");
    sync.NoteCamera(MakeOrbit(kNearUnits), kHostDefaultFieldDegrees);
    Check(!sync.TakeNextCommand(kStartMs), "an unchanged camera is not resent");
}

void TestEditsAndResizesWaitForAPause()
{
    EngineLiveSync sync = MakeAnchoredSync();
    sync.NoteParked(kWidthPx, kHeightPx);
    sync.NoteViewportSize(kWidthPx, kHeightPx, kStartMs);
    Check(!sync.TakeNextCommand(kStartMs + kParkDebounceMs), "the parked size needs no re-park");
    sync.NoteDocumentEdited(kStartMs);
    sync.NoteViewportSize(kWiderPx, kHeightPx, kStartMs);
    Check(!sync.TakeNextCommand(kStartMs + 1), "commands wait for their debounce");
    const std::optional<LiveCommand> park = sync.TakeNextCommand(kStartMs + kRefreshDebounceMs);
    Check(park && park->command == "window" && park->args["width"] == kWiderPx, "a resize re-parks first");
    const std::optional<LiveCommand> refresh = sync.TakeNextCommand(kStartMs + kRefreshDebounceMs);
    Check(refresh && refresh->command == "live-refresh", "then the edit refreshes the effect");
}

void TestStillResumesAndResendsTheCamera()
{
    EngineLiveSync sync = MakeAnchoredSync();
    sync.NoteCamera(MakeOrbit(kFarUnits), kHostDefaultFieldDegrees);
    sync.TakeNextCommand(kStartMs);
    sync.NoteStillFinished();
    sync.NoteCamera(MakeOrbit(kFarUnits), kHostDefaultFieldDegrees);
    const std::optional<LiveCommand> resume = sync.TakeNextCommand(kStartMs);
    Check(resume && resume->command == "clock" && resume->args["action"] == "resume", "a still resumes the clock");
    const std::optional<LiveCommand> camera = sync.TakeNextCommand(kStartMs);
    Check(camera && camera->command == "camera", "and resends the camera the still moved");
}

void TestCameraLatencyWaitsForTheTaggedFrame()
{
    EngineLiveSync sync = MakeAnchoredSync();
    sync.NoteCamera(MakeOrbit(kFarUnits), kHostDefaultFieldDegrees);
    sync.TakeNextCommand(kStartMs);
    sync.NoteFrameCameraTag(0, kStartMs + 1);
    Check(!sync.CameraLatencyMs(), "a frame from before the camera does not count");
    sync.NoteFrameCameraTag(1, kStartMs + kFrameLagMs);
    Check(sync.CameraLatencyMs() && *sync.CameraLatencyMs() == kFrameLagMs, "latency is send to tagged frame");
}

void TestFrameRateCountsWholeWindows()
{
    EngineLiveSync sync;
    sync.NoteFrameShown(kStartMs);
    for (std::uint64_t frame = 1; frame < kFramesPerWindow; ++frame)
        sync.NoteFrameShown(kStartMs + frame * kFpsWindowMs / kFramesPerWindow);
    Check(sync.FramesPerSecond() == 0.0, "no rate before a whole window");
    sync.NoteFrameShown(kStartMs + kFpsWindowMs);
    Check(sync.FramesPerSecond() == kFramesPerWindow, "a full window reports its frame count");
}

void TestFieldIsSentOnlyToAHostThatCanSetIt()
{
    EngineLiveSync olderHost = MakeAnchoredSync();
    olderHost.NoteCamera(MakeOrbit(kFarUnits), kNarrowFieldDegrees);
    const std::optional<LiveCommand> withoutField = olderHost.TakeNextCommand(kStartMs);
    Check(withoutField && !withoutField->args.contains("fov"), "an older host gets no field it would refuse");

    EngineLiveSync fieldHost;
    fieldHost.TakeNextCommand(kStartMs);
    fieldHost.NoteAnchor({0.0, 0.0, 0.0}, FieldLimits{kFieldMinDegrees, kHostDefaultFieldDegrees});
    fieldHost.NoteCamera(MakeOrbit(kFarUnits), kNarrowFieldDegrees);
    const std::optional<LiveCommand> withField = fieldHost.TakeNextCommand(kStartMs);
    Check(withField && withField->args["fov"] == kNarrowFieldDegrees, "a host with field limits gets the field");
    fieldHost.NoteCamera(MakeOrbit(kFarUnits), kHostDefaultFieldDegrees);
    Check(fieldHost.TakeNextCommand(kStartMs).has_value(), "a field change alone resends the camera");
}

void TestRaisedTargetSlidesToTheGround()
{
    using host::engine_preview::LiveOrbitFromLookAt;
    // Eye 600 up, looking at a point 200 up: the ray meets the ground 1.5 times as far out.
    const Orbit grounded =
        LiveOrbitFromLookAt({0.0, -kFarUnits, kEyeHeightUnits}, {0.0, 0.0, kRaisedTargetUnits}, kUnscaled);
    Check(grounded.target[2] == 0.0, "the target is moved to the ground");
    Check(std::abs(grounded.target[1] - kGroundedTargetY) < kToleranceUnit, "along the same view ray");
    const double sameRayPitch =
        std::atan2(kEyeHeightUnits - kRaisedTargetUnits, kFarUnits) * host::engine_preview::kRadiansToDegrees;
    Check(std::abs(grounded.pitch - sameRayPitch) < kToleranceUnit, "looking the same way");

    // Doubling the eye's distance from the raised point keeps that point at the lens's scale:
    // the eye moves back by one pivot distance, so the grounded distance grows by the same.
    const double pivotUnits = std::hypot(kFarUnits, kEyeHeightUnits - kRaisedTargetUnits);
    const Orbit  pushedBack =
        LiveOrbitFromLookAt({0.0, -kFarUnits, kEyeHeightUnits}, {0.0, 0.0, kRaisedTargetUnits}, kDoubleDistance);
    Check(std::abs(pushedBack.distance - (grounded.distance + pivotUnits)) < kToleranceUnit,
          "a lens distance scales the eye about the effect, not the ground point");
    Check(std::abs(pushedBack.pitch - grounded.pitch) < kToleranceUnit, "and keeps the view direction");
}

void TestNearlyLevelRayIsRefused()
{
    using host::engine_preview::LiveOrbitFromLookAt;
    bool isRefused = false;
    try
    {
        LiveOrbitFromLookAt({0.0, -kFarUnits, kEyeHeightUnits}, {0.0, 0.0, kEyeHeightUnits - kSlightDropUnits},
                            kUnscaled);
    }
    catch (const host::engine_preview::Failure&)
    {
        isRefused = true;
    }
    Check(isRefused, "a ray meeting the ground past the engine's reach keeps the last camera");
}

void TestLiveOrbitClampsInsteadOfRefusing()
{
    using host::engine_preview::kMaxPitchDegrees;
    using host::engine_preview::LiveOrbitFromLookAt;
    const Orbit below = LiveOrbitFromLookAt({0.0, -kFarUnits, -kFarUnits}, {0.0, 0.0, 0.0}, kUnscaled);
    Check(below.pitch == 0.0, "a camera below its target is raised to level");
    const double steepZ = kFarUnits * std::tan(kSteepDegrees * 3.14159265358979323846 / 180.0);
    const Orbit  steep  = LiveOrbitFromLookAt({0.0, -kFarUnits, steepZ}, {0.0, 0.0, 0.0}, kUnscaled);
    Check(std::abs(steep.pitch - kMaxPitchDegrees) < kToleranceUnit, "a steep camera is held at the pitch limit");
    const Orbit onTarget = LiveOrbitFromLookAt({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, kUnscaled);
    Check(onTarget.distance > 0, "a camera on its target is moved out");
}
} // namespace

int main()
{
    TestGroundWaitsForOtherWorkAndCoalesces();
    TestGroundWaitsForDebounceAndRetriesOnlyOnChange();
    TestGroundChangedDuringACommandKeepsTheLatest();
    TestCameraIsRelativeToTheOpeningTarget();
    TestFailedAnchorIsAskedAgain();
    TestCameraKeepsOnlyTheNewest();
    TestEditsAndResizesWaitForAPause();
    TestStillResumesAndResendsTheCamera();
    TestCameraLatencyWaitsForTheTaggedFrame();
    TestFrameRateCountsWholeWindows();
    TestFieldIsSentOnlyToAHostThatCanSetIt();
    TestRaisedTargetSlidesToTheGround();
    TestNearlyLevelRayIsRefused();
    TestLiveOrbitClampsInsteadOfRefusing();
    if (g_failed) return 1;
    std::printf("test_engine_live_sync: all passed\n");
    return 0;
}
