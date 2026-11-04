// Test for the rocket's spline (src/simulation/RocketSpline.h, the game's SplineClass).
//
// Checks the natural cubic against its defining conditions (through every control
// point, matching slopes at the joins, no curvature at the ends), the arc lengths
// against a dense polyline, and the game's linear distance-to-parameter lookup.

#include "simulation/RocketSpline.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            std::printf("  ok: %s\n", msg);                                                                            \
        else                                                                                                           \
        {                                                                                                              \
            std::printf("  FAIL: %s\n", msg);                                                                          \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool Near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

// Length of segment `s` measured as a polyline of `steps` chords.
static double PolylineLength(const RocketSpline& spline, int s, int steps)
{
    double total = 0.0;
    float  prev[3], cur[3];
    spline.EvalSegment(s, 0.0f, prev);
    for (int k = 1; k <= steps; k++)
    {
        spline.EvalSegment(s, (float)k / steps, cur);
        const double dx = cur[0] - prev[0], dy = cur[1] - prev[1], dz = cur[2] - prev[2];
        total += std::sqrt(dx * dx + dy * dy + dz * dz);
        for (int i = 0; i < 3; i++) prev[i] = cur[i];
    }
    return total;
}

int main()
{
    std::printf("[straight line]\n");
    {
        RocketSpline line;
        for (int k = 0; k < 4; k++)
        {
            const float p[3] = {10.0f * k, 0, 0};
            line.AddControlPoint(p);
        }
        CHECK(line.Evaluate(10.0f), "four points evaluate");
        CHECK(Near(line.Length(), 30.0, 1e-3), "evenly spaced collinear points: length is the distance");
        float at[3];
        int   seg = -1;
        CHECK(line.PointFromLength(15.0f, at, seg) && Near(at[0], 15.0, 1e-3) && seg == 1,
              "15 along lands mid-way, in the second segment");
        CHECK(!line.PointFromLength(30.5f, at, seg), "past the end: no point");
        float step[3];
        CHECK(line.LastEvaluatedStep(step) && Near(step[0], 10.0, 1e-3),
              "10-unit segments at spacing 10: one sample each");
        line.Evaluate(3.0f);
        CHECK(line.LastEvaluatedStep(step) && Near(step[0], 2.5, 1e-3),
              "at spacing 3: ceil(10/3) = 4 samples per segment");
    }

    std::printf("[natural cubic]\n");
    {
        const float  pts[5][3] = {{0, 0, 0}, {40, 25, 5}, {70, -10, 12}, {130, 30, 0}, {150, 0, -8}};
        RocketSpline curve;
        for (const auto& p : pts) curve.AddControlPoint(p);
        CHECK(curve.Evaluate(10.0f), "five points evaluate");

        bool through = true, smooth = true;
        for (int s = 0; s < 4; s++)
        {
            float a[3], b[3];
            curve.EvalSegment(s, 0.0f, a);
            curve.EvalSegment(s, 1.0f, b);
            for (int i = 0; i < 3; i++)
                if (!Near(a[i], pts[s][i], 1e-3) || !Near(b[i], pts[s + 1][i], 1e-3)) through = false;
        }
        for (int axis = 0; axis < 3; axis++)
            for (int s = 0; s < 3; s++)
            {
                const SplineCubic& c0       = curve.Cubic(axis, s);
                const SplineCubic& c1       = curve.Cubic(axis, s + 1);
                const double       slopeOut = c0.b + 2 * c0.c + 3 * c0.d, slopeIn = c1.b;
                const double       curvOut = 2 * c0.c + 6 * c0.d, curvIn = 2 * c1.c;
                if (!Near(slopeOut, slopeIn, 1e-3) || !Near(curvOut, curvIn, 1e-3)) smooth = false;
            }
        CHECK(through, "passes through every control point");
        CHECK(smooth, "slope and curvature match at every join");
        bool naturalEnds = true;
        for (int axis = 0; axis < 3; axis++)
        {
            const SplineCubic& first = curve.Cubic(axis, 0);
            const SplineCubic& last  = curve.Cubic(axis, 3);
            if (!Near(2 * first.c, 0.0, 1e-3) || !Near(2 * last.c + 6 * last.d, 0.0, 1e-3)) naturalEnds = false;
        }
        CHECK(naturalEnds, "no curvature at either end (natural)");

        double polyline = 0.0;
        for (int s = 0; s < 4; s++) polyline += PolylineLength(curve, s, 20000);
        CHECK(Near(curve.Length(), polyline, polyline * 1e-4), "arc length matches a dense polyline");

        // The game's lookup is linear in the segment parameter, not in distance.
        const double firstSegment = PolylineLength(curve, 0, 20000);
        float        at[3], expect[3];
        int          seg = -1;
        curve.PointFromLength((float)(firstSegment * 0.25), at, seg);
        curve.EvalSegment(0, 0.25f, expect);
        CHECK(seg == 0 && Near(at[0], expect[0], 1e-2) && Near(at[1], expect[1], 1e-2),
              "a quarter of the first segment's length lands at u = 0.25");
    }

    std::printf("[editing]\n");
    {
        RocketSpline two;
        const float  a[3] = {0, 0, 0}, b[3] = {5, 0, 0};
        two.AddControlPoint(a);
        two.AddControlPoint(b);
        float at[3];
        int   seg = -1;
        CHECK(!two.Evaluate(10.0f), "two points don't evaluate (the game needs three)");
        CHECK(!two.PointFromLength(1.0f, at, seg), "an unevaluated spline has no points");

        RocketSpline five;
        for (int k = 0; k < 5; k++)
        {
            const float p[3] = {(float)k, 0, 0};
            five.AddControlPoint(p);
        }
        five.RemoveControlPointsFrom(3);
        CHECK(five.ControlPointCount() == 3 && five.ControlPoint(2)[0] == 2.0f, "removing from 3 keeps points 0..2");
    }

    std::printf(g_fail ? "\n=== RocketSpline: %d FAILED ===\n" : "\n=== RocketSpline: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
