#ifndef SCENE_OVERSCAN_H
#define SCENE_OVERSCAN_H
// Pure helper for the scene-rect overscan guard. Header-only, no engine / Win32
// deps (mirrors ModLayers.h / GizmoSizing.h) so it unit-tests standalone.
//
// The engine renders the scene viewport slightly LARGER than the true client
// rect (an "overscan" guard band) so particles near the edge don't pop/clip as
// the rect animates or reattaches; DirectComposition then clips back to the true
// rect. Both LayoutBroker's normal apply AND its reemit path must feed the engine
// the SAME overscanned rect (release-audit #10 — reemit previously replayed the
// raw true rect, reintroducing edge artifacts).

struct SceneViewport
{
    int x, y, w, h;
};

// Map a true client rect (front of pipeline) to the engine's overscanned
// viewport. Guard band: GBx is w/64 with a 12px floor; GBy preserves aspect
// (GBx * h / w, rounded) and degrades to GBx when w == 0.
inline SceneViewport ComputeOverscanViewport(int x, int y, int w, int h)
{
    const int GBx = (w / 64 > 12) ? (w / 64) : 12;
    const int GBy = (w > 0) ? (GBx * h + w / 2) / w : GBx;
    return SceneViewport{ x - GBx, y - GBy, w + 2 * GBx, h + 2 * GBy };
}

#endif // SCENE_OVERSCAN_H
