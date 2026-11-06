// Live engine view command policy: newest-wins camera, debounced refresh and re-park, resume
// after a still, camera latency, and the clamped live orbit. No pipe, host or engine.
#include "host/EngineLiveSync.h"
#include <cstdio>

using namespace host::engine_live;
using host::engine_preview::Orbit;
namespace
{
constexpr std::uint64_t kStartMs         = 1000;
constexpr int           kWidthPx         = 1280;
constexpr int           kHeightPx        = 720;
constexpr int           kWiderPx         = 1400;
constexpr double        kPitchDegrees    = 34.0;
constexpr double        kFarUnits        = 900.0;
constexpr double        kNearUnits       = 600.0;
constexpr std::uint64_t kFrameLagMs      = 40;
constexpr double        kSteepDegrees    = 89.0;
constexpr double        kToleranceUnit   = 0.000001;
constexpr std::uint64_t kFramesPerWindow = 30;
constexpr double        kAnchorX         = 2048.0;
constexpr double        kAnchorY         = 1536.0;
constexpr double        kAnchorZ         = 12.0;
int                     g_failed         = 0;

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
    sync.NoteAnchor({0.0, 0.0, 0.0});
    return sync;
}

void TestCameraIsRelativeToTheOpeningTarget()
{
    EngineLiveSync sync;
    sync.NoteCamera(MakeOrbit(kFarUnits));
    const std::optional<LiveCommand> anchor = sync.TakeNextCommand(kStartMs);
    Check(anchor && sync.IsAnchorRequest(*anchor), "the opening camera is read before any camera is sent");
    Check(!sync.TakeNextCommand(kStartMs), "the camera waits for the opening target");
    sync.NoteAnchor({kAnchorX, kAnchorY, kAnchorZ});
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
    sync.NoteCamera(MakeOrbit(kFarUnits));
    sync.NoteCamera(MakeOrbit(kNearUnits));
    const std::optional<LiveCommand> first = sync.TakeNextCommand(kStartMs);
    Check(first && first->command == "camera" && first->args["distance"] == kNearUnits,
          "only the newest camera is sent");
    Check(first && first->args["tag"] == 1, "the first camera carries tag 1");
    Check(!sync.TakeNextCommand(kStartMs), "nothing else waits");
    sync.NoteCamera(MakeOrbit(kNearUnits));
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
    sync.NoteCamera(MakeOrbit(kFarUnits));
    sync.TakeNextCommand(kStartMs);
    sync.NoteStillFinished();
    sync.NoteCamera(MakeOrbit(kFarUnits));
    const std::optional<LiveCommand> resume = sync.TakeNextCommand(kStartMs);
    Check(resume && resume->command == "clock" && resume->args["action"] == "resume", "a still resumes the clock");
    const std::optional<LiveCommand> camera = sync.TakeNextCommand(kStartMs);
    Check(camera && camera->command == "camera", "and resends the camera the still moved");
}

void TestCameraLatencyWaitsForTheTaggedFrame()
{
    EngineLiveSync sync = MakeAnchoredSync();
    sync.NoteCamera(MakeOrbit(kFarUnits));
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

void TestLiveOrbitClampsInsteadOfRefusing()
{
    using host::engine_preview::kMaxPitchDegrees;
    using host::engine_preview::LiveOrbitFromLookAt;
    const Orbit below = LiveOrbitFromLookAt({0.0, -kFarUnits, -kFarUnits}, {0.0, 0.0, 0.0});
    Check(below.pitch == 0.0, "a camera below its target is raised to level");
    const double steepZ = kFarUnits * std::tan(kSteepDegrees * 3.14159265358979323846 / 180.0);
    const Orbit  steep  = LiveOrbitFromLookAt({0.0, -kFarUnits, steepZ}, {0.0, 0.0, 0.0});
    Check(std::abs(steep.pitch - kMaxPitchDegrees) < kToleranceUnit, "a steep camera is held at the pitch limit");
    const Orbit onTarget = LiveOrbitFromLookAt({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
    Check(onTarget.distance > 0, "a camera on its target is moved out");
}
} // namespace

int main()
{
    TestCameraIsRelativeToTheOpeningTarget();
    TestFailedAnchorIsAskedAgain();
    TestCameraKeepsOnlyTheNewest();
    TestEditsAndResizesWaitForAPause();
    TestStillResumesAndResendsTheCamera();
    TestCameraLatencyWaitsForTheTaggedFrame();
    TestFrameRateCountsWholeWindows();
    TestLiveOrbitClampsInsteadOfRefusing();
    if (g_failed) return 1;
    std::printf("test_engine_live_sync: all passed\n");
    return 0;
}
