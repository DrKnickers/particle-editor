// Unit test for engine/set/camera validation (src/host/CameraParams.h,
// 2026-10-01 audit HX2).
//
// The handler used to map a missing field to (0,0,0) and hand any camera to
// Engine::SetCamera; a coincident eye/target or a zero/parallel up vector made
// D3DXMatrixLookAtRH produce a NaN view matrix. Valid cameras -- the Reset
// Camera default, the drive/clip orbit poses -- must still pass unchanged.

#include "host/CameraParams.h"

#include <cmath>
#include <cstdio>
#include <limits>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using nlohmann::json;

static json Cam(json position, json target, json up)
{
    return json{{"position", position}, {"target", target}, {"up", up}};
}

static bool Accepts(const json& params)
{
    host::CameraParams p;
    return host::ReadCameraParams(params, p) == nullptr;
}

int main()
{
    std::printf("test_camera_params\n");

    // --- valid cameras pass, values copied exactly ---
    {
        // web/apps/editor/src/lib/reset-camera.ts RESET_CAMERA.
        host::CameraParams p;
        const char* why = host::ReadCameraParams(
            Cam({0, -250, 125}, {0, 0, 0}, {0, 0, 1}), p);
        CHECK(why == nullptr, "Reset Camera default is accepted");
        CHECK(p.position[0] == 0.0f && p.position[1] == -250.0f && p.position[2] == 125.0f
              && p.target[2] == 0.0f && p.up[2] == 1.0f,
              "values are copied unchanged");
    }
    CHECK(Accepts(Cam({10, 0, 0}, {0, 0, 0}, {0, 0, 1})), "viewport-camera.spec pose accepted");
    CHECK(Accepts(Cam({1.5, 2.5, 3.5}, {0.5, 0, 0}, {0, 0, 2})), "fractional values, non-unit up accepted");
    CHECK(Accepts(Cam({10, 0, 0, 99}, {0, 0, 0, 99}, {0, 0, 1, 99})),
          "extra array elements ignored (JsonToVec3 tolerance kept)");
    {
        // A near-pole orbit (pitch 89.9 deg, ComputeOrbitCamera's Z-up) stays legal.
        const double pitch = 89.9 * 3.14159265358979323846 / 180.0;
        const double dist = 300.0;
        CHECK(Accepts(Cam({0, std::cos(pitch) * dist, std::sin(pitch) * dist},
                          {0, 0, 0}, {0, 0, 1})),
              "near-pole orbit pose accepted");
    }

    // --- malformed fields are refused (used to become (0,0,0)) ---
    CHECK(!Accepts(json::object()), "empty params refused");
    CHECK(!Accepts(json::array()), "non-object params refused");
    CHECK(!Accepts(json{{"target", {0, 0, 0}}, {"up", {0, 0, 1}}}), "missing position refused");
    CHECK(!Accepts(json{{"position", {10, 0, 0}}, {"up", {0, 0, 1}}}), "missing target refused");
    CHECK(!Accepts(json{{"position", {10, 0, 0}}, {"target", {0, 0, 0}}}), "missing up refused");
    CHECK(!Accepts(Cam({10, 0}, {0, 0, 0}, {0, 0, 1})), "two-element vector refused");
    CHECK(!Accepts(Cam("10,0,0", {0, 0, 0}, {0, 0, 1})), "non-array vector refused");
    CHECK(!Accepts(Cam({10, "0", 0}, {0, 0, 0}, {0, 0, 1})), "string component refused");
    CHECK(!Accepts(Cam({10, nullptr, 0}, {0, 0, 0}, {0, 0, 1})), "null component refused");

    // --- non-finite / out-of-float-range components ---
    CHECK(!Accepts(Cam({std::numeric_limits<double>::quiet_NaN(), 0, 0}, {0, 0, 0}, {0, 0, 1})),
          "NaN component refused");
    CHECK(!Accepts(Cam({std::numeric_limits<double>::infinity(), 0, 0}, {0, 0, 0}, {0, 0, 1})),
          "infinite component refused");
    CHECK(!Accepts(Cam({1e300, 0, 0}, {0, 0, 0}, {0, 0, 1})), "component beyond float range refused");

    // --- degenerate geometry ---
    CHECK(!Accepts(Cam({5, 5, 5}, {5, 5, 5}, {0, 0, 1})), "coincident position/target refused");
    CHECK(!Accepts(Cam({10, 0, 0}, {0, 0, 0}, {0, 0, 0})), "zero up refused");
    CHECK(!Accepts(Cam({0, 0, 10}, {0, 0, 0}, {0, 0, 1})), "up parallel to the view direction refused");
    CHECK(!Accepts(Cam({0, 0, 10}, {0, 0, 0}, {0, 0, -3})), "up anti-parallel to the view direction refused");
    {
        host::CameraParams p;
        const char* why = host::ReadCameraParams(Cam({5, 5, 5}, {5, 5, 5}, {0, 0, 1}), p);
        CHECK(why != nullptr && why[0] != '\0', "a refusal carries a reason for the ok:false envelope");
    }

    std::printf(g_failed ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
