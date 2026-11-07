// Live engine view framing: the window-free math that lines the engine host's
// picture up with the editor's own. The host always renders a 4:3 projection stretched to its
// window (measured live 2026-11-05), so it is parked at 4:3, shown cropped to cover the
// viewport, and given a field of view (and, past its limits, a distance) that matches ours.
// No pipe, window or device calls; the dispatcher and the engine apply the results.
#ifndef HOST_ENGINE_LIVE_FRAMING_H
#define HOST_ENGINE_LIVE_FRAMING_H

#include "EngineLiveFrame.h"
#include "EnginePreviewPipe.h"
#include <algorithm>
#include <cmath>

namespace host::engine_live
{
constexpr double kHostFrameAspect = 4.0 / 3.0;
// The host's field of view spans its 4:3 frame horizontally; its camera update divides the
// angle (not the tangent) by the frame aspect to get the vertical angle. Maps open at 55.
constexpr double kHostDefaultFieldDegrees = 55.0;
// The host streams only a main view at least this big on each edge.
constexpr int kMinParkEdgePx = 400;
using engine_preview::kRadiansToDegrees;

// The field-of-view range the host's camera accepts, in its own degrees.
struct FieldLimits
{
    double minDegrees;
    double maxDegrees;
};

struct ParkSize
{
    int widthPx;
    int heightPx;
};

// The part of the frame, in texture coordinates, that fills the viewport.
struct FrameCrop
{
    float u0, v0, u1, v1;
};

// What to ask of the host's camera: its field of view, and how much farther the eye must sit
// to make up for a field outside the host's limits (1 when the field alone matches).
struct HostLens
{
    double fieldDegrees;
    double distanceScale;
};

// The smallest 4:3 view that covers the viewport, raised to the host's minimum edge and
// lowered to the largest 4:3 view the frame section holds.
inline ParkSize ParkSizeForViewport(int viewportWidthPx, int viewportHeightPx)
{
    const double maxWidthPx = (std::min)(static_cast<double>(kMaxWidthPx), kMaxHeightPx * kHostFrameAspect);
    double       widthPx    = (std::max)(static_cast<double>(viewportWidthPx), viewportHeightPx * kHostFrameAspect);
    widthPx                 = std::clamp(widthPx, kMinParkEdgePx * kHostFrameAspect, maxWidthPx);
    return ParkSize{static_cast<int>(std::lround(widthPx)), static_cast<int>(std::lround(widthPx / kHostFrameAspect))};
}

// A viewport wider than 4:3 shows the frame's full width and a centred band of its height;
// a narrower one shows its full height and a centred band of its width.
inline double VisibleHeightShare(double viewportWidthPx, double viewportHeightPx)
{
    return (std::min)(1.0, kHostFrameAspect / (viewportWidthPx / viewportHeightPx));
}

inline FrameCrop CoverCropForViewport(double viewportWidthPx, double viewportHeightPx)
{
    const double heightShare = VisibleHeightShare(viewportWidthPx, viewportHeightPx);
    const double widthShare  = (std::min)(1.0, (viewportWidthPx / viewportHeightPx) / kHostFrameAspect);
    const float  uMargin     = static_cast<float>((1.0 - widthShare) / 2);
    const float  vMargin     = static_cast<float>((1.0 - heightShare) / 2);
    return FrameCrop{uMargin, vMargin, 1.0f - uMargin, 1.0f - vMargin};
}

// Matches the host's visible vertical angle to the editor's. The field is clamped to the
// host's limits (both the default when it cannot set one); the rest becomes eye distance,
// since a picture's scale at the target goes as 1 / (distance * tan(half angle)).
inline HostLens MatchHostLens(double             editorFovYRadians,
                              double             viewportWidthPx,
                              double             viewportHeightPx,
                              const FieldLimits& hostField)
{
    const double neededTangent =
        std::tan(editorFovYRadians / 2) / VisibleHeightShare(viewportWidthPx, viewportHeightPx);
    const double neededField  = 2 * std::atan(neededTangent) * kRadiansToDegrees * kHostFrameAspect;
    const double field        = std::clamp(neededField, hostField.minDegrees, hostField.maxDegrees);
    const double fieldTangent = std::tan(field / kHostFrameAspect / 2 / kRadiansToDegrees);
    return HostLens{field, neededTangent / fieldTangent};
}
} // namespace host::engine_live
#endif
