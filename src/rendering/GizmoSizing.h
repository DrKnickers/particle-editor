#pragma once
#include <cmath>
// World-space length of the reference-object gizmo's axis handle, sized so the gizmo holds a
// constant on-screen PIXEL size at its origin. viewSpaceDepth = dot(refPos-eye, viewForward).
// Screen-uniform: worldPerPixel = 2*depth*tan(fovY/2)/viewportH; len = kTargetGizmoPx*worldPerPixel.
// Falls back to the legacy eye-distance*0.12 when the scene viewport isn't active / degenerate.
inline float GizmoHandleLengthWorld(float viewSpaceDepth, float fovY, int viewportH,
                                    bool sceneActive, float eyeDist)
{
    constexpr float kTargetGizmoPx = 111.25f;   // reproduces legacy dist*0.12 at H=768, fovY=45deg
    if (!sceneActive || viewportH <= 0 || viewSpaceDepth <= 0.0f) {
        float l = eyeDist * 0.12f;  return l > 1.0f ? l : 1.0f;       // legacy fallback
    }
    float worldPerPixel = (2.0f * viewSpaceDepth * std::tan(fovY * 0.5f)) / (float)viewportH;
    float l = kTargetGizmoPx * worldPerPixel;   return l > 1.0f ? l : 1.0f;
}
