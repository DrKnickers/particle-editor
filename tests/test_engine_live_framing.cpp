// Live engine view framing: 4:3 park sizes, the cover crop of the host's frame, and the field of
// view (plus distance past the host's limits) that matches the editor's own picture.
#include "host/EngineLiveFraming.h"
#include <cmath>
#include <cstdio>

using namespace host::engine_live;
namespace
{
constexpr int    kWideWidthPx       = 1600;
constexpr int    kWideHeightPx      = 800;
constexpr int    kTallWidthPx       = 600;
constexpr int    kTallHeightPx      = 900;
constexpr int    kTinyEdgePx        = 100;
constexpr int    kHugeWidthPx       = 5000;
constexpr int    kHugeHeightPx      = 3000;
constexpr double kEditorFovYDegrees = 30.0;
constexpr double kWideOpenDegrees   = 1.0;
constexpr double kWideLimitDegrees  = 179.0;
constexpr double kTolerance         = 0.0001;
int              g_failed           = 0;

void Check(bool passed, const char* message)
{
    if (passed) return;
    ++g_failed;
    std::printf("FAIL: %s\n", message);
}

bool IsNear(double actual, double expected)
{
    return std::abs(actual - expected) < kTolerance;
}

void TestParkSizeCoversAtFourThree()
{
    const ParkSize wide = ParkSizeForViewport(kWideWidthPx, kWideHeightPx);
    Check(wide.widthPx == kWideWidthPx && wide.heightPx == std::lround(kWideWidthPx / kHostFrameAspect),
          "a wide viewport parks full width");
    const ParkSize tall = ParkSizeForViewport(kTallWidthPx, kTallHeightPx);
    Check(tall.heightPx == kTallHeightPx && tall.widthPx == std::lround(kTallHeightPx * kHostFrameAspect),
          "a tall viewport parks full height");
    const ParkSize tiny = ParkSizeForViewport(kTinyEdgePx, kTinyEdgePx);
    Check(tiny.heightPx == kMinParkEdgePx, "a tiny viewport parks at the host's minimum edge");
    const ParkSize huge = ParkSizeForViewport(kHugeWidthPx, kHugeHeightPx);
    Check(huge.heightPx == static_cast<int>(kMaxHeightPx) &&
              huge.widthPx == std::lround(huge.heightPx * kHostFrameAspect),
          "a huge viewport parks at the largest 4:3 the section holds");
}

void TestCropShowsACentredBand()
{
    const FrameCrop wide = CoverCropForViewport(kWideWidthPx, kWideHeightPx);
    // A 2:1 viewport shows two thirds of a 4:3 frame's height.
    Check(wide.u0 == 0.0f && wide.u1 == 1.0f, "a wide viewport keeps the full width");
    Check(IsNear(wide.v0, 1.0 / 6) && IsNear(wide.v1, 5.0 / 6), "and the middle two thirds of the height");
    const FrameCrop tall = CoverCropForViewport(kTallWidthPx, kTallHeightPx);
    Check(tall.v0 == 0.0f && tall.v1 == 1.0f && tall.u0 > 0.0f && IsNear(tall.u0, 1.0 - tall.u1),
          "a tall viewport keeps the full height and a centred band of the width");
}

void TestLensMatchesTheEditorsVerticalAngle()
{
    const double   editorFovY = kEditorFovYDegrees / kRadiansToDegrees;
    const HostLens free =
        MatchHostLens(editorFovY, kWideWidthPx, kWideHeightPx, FieldLimits{kWideOpenDegrees, kWideLimitDegrees});
    Check(IsNear(free.distanceScale, 1.0), "a reachable field needs no distance change");
    const double hostHalfTangent  = std::tan(free.fieldDegrees / kHostFrameAspect / 2 / kRadiansToDegrees);
    const double shownHalfTangent = hostHalfTangent * VisibleHeightShare(kWideWidthPx, kWideHeightPx);
    Check(IsNear(shownHalfTangent, std::tan(editorFovY / 2)), "the shown band spans the editor's vertical angle");

    const HostLens fixed = MatchHostLens(editorFovY, kWideWidthPx, kWideHeightPx,
                                         FieldLimits{kHostDefaultFieldDegrees, kHostDefaultFieldDegrees});
    Check(fixed.fieldDegrees == kHostDefaultFieldDegrees, "a host that cannot set its field keeps the default");
    const double fixedHalfTangent = std::tan(kHostDefaultFieldDegrees / kHostFrameAspect / 2 / kRadiansToDegrees);
    Check(IsNear(fixed.distanceScale, hostHalfTangent / fixedHalfTangent),
          "and the eye moves so the picture keeps the editor's scale");
}
} // namespace

int main()
{
    TestParkSizeCoversAtFourThree();
    TestCropShowsACentredBand();
    TestLensMatchesTheEditorsVerticalAngle();
    if (g_failed) return 1;
    std::printf("test_engine_live_framing: all passed\n");
    return 0;
}
