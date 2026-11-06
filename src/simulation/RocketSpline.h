#pragma once

// The game's rocket spline: a natural cubic per axis over the control-point index,
// each segment's arc length by recursive chord bisection, and the point a given
// distance along the curve.
// Owns the curve math only; ProjectileFlight.h builds a rocket's control points and
// flies it. Floats only and no engine includes, so the unit test can link it bare.

#include <cmath>
#include <vector>

// Cubic: a + b*u + c*u^2 + d*u^3 over one segment, u in [0, 1].
struct SplineCubic
{
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    float Eval(float u) const { return ((d * u + c) * u + b) * u + a; }
};

class RocketSpline
{
public:
    // Spline evaluation needs at least this many control points; with fewer it
    // returns false and keeps whatever it computed before.
    static constexpr int kMinEvaluatedControlPoints = 3;
    // Spline sample count per segment: ceil(arc length / spacing), clamped here.
    static constexpr int kMaxEvaluatedPointsPerSegment = 100;
    // Arc-length bisection stops once chord and half-chords agree this closely.
    static constexpr float kArcTolerance = 1e-6f;
    // Not in the game: float rounding ends arc-length recursion long before this
    // on real curves, but a NaN control point would recurse forever.
    static constexpr int kMaxArcBisections = 32;

    void AddControlPoint(const float p[3]) { m_controlPoints.push_back({p[0], p[1], p[2]}); }

    int ControlPointCount() const { return (int)m_controlPoints.size(); }

    const float* ControlPoint(int index) const { return m_controlPoints[index].v; }

    // Remove control points from the end down to `firstIndex`, inclusive.
    void RemoveControlPointsFrom(int firstIndex)
    {
        if (firstIndex < (int)m_controlPoints.size()) m_controlPoints.resize(firstIndex < 0 ? 0 : firstIndex);
    }

    // Spline evaluation fits the cubics, measures each segment and samples it
    // about every `spacing` units of arc. It rebuilds the cached curve that
    // Length, PointFromLength and LastEvaluatedStep read, so call it again after
    // changing control points.
    bool Evaluate(float spacing)
    {
        if ((int)m_controlPoints.size() < kMinEvaluatedControlPoints) return false;
        const int segmentCount = (int)m_controlPoints.size() - 1;
        for (int axis = 0; axis < 3; axis++) FitNaturalCubic(axis);

        m_arcLength.clear();
        m_length = 0.0f;
        for (int s = 0; s < segmentCount; s++)
        {
            m_arcLength.push_back(SegmentArcLength(s, 0.0f, 1.0f));
            m_length += m_arcLength[s];
        }

        m_evaluatedPoints.clear();
        Point start;
        EvalSegment(0, 0.0f, start.v);
        m_evaluatedPoints.push_back(start);
        for (int s = 0; s < segmentCount; s++)
        {
            int samples = (int)std::ceil(m_arcLength[s] / spacing);
            if (samples < 1) samples = 1;
            if (samples > kMaxEvaluatedPointsPerSegment) samples = kMaxEvaluatedPointsPerSegment;
            for (int k = 1; k <= samples; k++)
            {
                Point sample;
                EvalSegment(s, (float)k / (float)samples, sample.v);
                m_evaluatedPoints.push_back(sample);
            }
        }
        return true;
    }

    float Length() const { return m_length; }

    // The last evaluated point minus the one before it (indices n - 1 and n - 2).
    // False with fewer than two evaluated points.
    bool LastEvaluatedStep(float step[3]) const
    {
        const size_t n = m_evaluatedPoints.size();
        if (n < 2) return false;
        for (int i = 0; i < 3; i++) step[i] = m_evaluatedPoints[n - 1].v[i] - m_evaluatedPoints[n - 2].v[i];
        return true;
    }

    // Distance lookup: the segment holding `distance`, entered at a
    // parameter LINEAR in the distance, (distance - segment start) / segment length.
    // So equal steps along the path are only roughly equal in space. False past the end.
    bool PointFromLength(float distance, float out[3], int& segment) const
    {
        const int segmentCount = (int)m_controlPoints.size() - 1;
        float     segmentStart = 0.0f;
        for (int s = 0; s < segmentCount && s < (int)m_arcLength.size(); s++)
        {
            if (distance < segmentStart + m_arcLength[s])
            {
                EvalSegment(s, (distance - segmentStart) / m_arcLength[s], out);
                segment = s;
                return true;
            }
            segmentStart += m_arcLength[s];
        }
        return false;
    }

    void EvalSegment(int segment, float u, float out[3]) const
    {
        for (int axis = 0; axis < 3; axis++) out[axis] = m_cubics[axis][segment].Eval(u);
    }

    const SplineCubic& Cubic(int axis, int segment) const { return m_cubics[axis][segment]; }

private:
    struct Point
    {
        float v[3];
    };

    // Cubic fitting: the tridiagonal solve for a natural cubic through
    // the control points, parameterised by index (every segment spans u = 0..1).
    void FitNaturalCubic(int axis)
    {
        const int count = (int)m_controlPoints.size();
        const int last  = count - 1;
        auto      x     = [&](int i)
        {
            return m_controlPoints[i].v[axis];
        };

        std::vector<float> gamma(count), delta(count), slope(count);
        gamma[0] = 0.5f;
        for (int i = 1; i < last; i++) gamma[i] = 1.0f / (4.0f - gamma[i - 1]);
        gamma[last] = 1.0f / (2.0f - gamma[last - 1]);

        delta[0] = (x(1) - x(0)) * 3.0f * gamma[0];
        for (int i = 1; i < last; i++) delta[i] = ((x(i + 1) - x(i - 1)) * 3.0f - delta[i - 1]) * gamma[i];
        delta[last] = ((x(last) - x(last - 1)) * 3.0f - delta[last - 1]) * gamma[last];

        slope[last] = delta[last];
        for (int i = last - 1; i >= 0; i--) slope[i] = delta[i] - gamma[i] * slope[i + 1];

        m_cubics[axis].clear();
        for (int i = 0; i < last; i++)
        {
            SplineCubic cubic;
            cubic.a = x(i);
            cubic.b = slope[i];
            cubic.c = ((x(i + 1) - x(i)) * 3.0f - slope[i] * 2.0f) - slope[i + 1];
            cubic.d = (x(i) - x(i + 1)) * 2.0f + slope[i] + slope[i + 1];
            m_cubics[axis].push_back(cubic);
        }
    }

    float SegmentArcLength(int segment, float u0, float u1) const { return SplineArc(segment, u0, u1, 0); }

    // Arc length: the chord, or the two half-chords once they agree with it.
    float SplineArc(int segment, float u0, float u1, int depth) const
    {
        float p0[3], p1[3], mid[3];
        EvalSegment(segment, u0, p0);
        EvalSegment(segment, u1, p1);
        const float chord = Distance(p0, p1);
        if (chord < kArcTolerance) return chord;

        const float uMid = (u0 + u1) * 0.5f;
        EvalSegment(segment, uMid, mid);
        const float halves = Distance(p0, mid) + Distance(p1, mid);
        const bool  agree =
            std::fabs(chord / halves - 1.0f) <= kArcTolerance || std::fabs(chord - halves) <= kArcTolerance;
        if (agree || depth >= kMaxArcBisections) return halves;
        return SplineArc(segment, u0, uMid, depth + 1) + SplineArc(segment, uMid, u1, depth + 1);
    }

    static float Distance(const float a[3], const float b[3])
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    std::vector<Point>       m_controlPoints;
    std::vector<SplineCubic> m_cubics[3]; // per axis, one per segment
    std::vector<float>       m_arcLength; // per segment
    std::vector<Point>       m_evaluatedPoints;
    float                    m_length = 0.0f;
};
