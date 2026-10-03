// Regression test for the GROUND_BOUNCE catch-up loop (src/simulation/BounceCatchUp.h).
//
// The loop in EmitterInstance::UpdateParticle advanced bounceTime by one arc per
// bounce and only stopped when a bounce left v_z exactly 0. bounciness = -1 sent
// it into an infinite loop on the first landing, bounciness = 1 spun in place
// once an arc fell under half an ulp of bounceTime, and NaN / infinite values
// poisoned the particle. Ordinary values fail too: a decaying bounce eventually
// produces sub-ulp arcs, and from there the original loop walks bounceTime
// backwards and spins (section 1 prints how far it gets). No render golden uses
// GROUND_BOUNCE, so this test is what pins the behaviour:
//
//   1. EXACT: for the ordinary range b in {0, 0.2, 0.25, 0.5, 0.9}, frame by
//      frame and across one large catch-up jump, every bounce the new loop
//      takes leaves the particle equal (==) to the original loop after the same
//      number of bounces; the original is reproduced verbatim as the oracle.
//      When the new loop stops bouncing, the stop must be justified -- the
//      original's next arc was degenerate (not forward, NaN, or too short to move
//      bounceTime) or the per-update cap was reached -- and the particle must be
//      the original's state with v_z = 0 and bounceTime = FLT_MAX.
//   2. TERMINATION: for b in {-1, 1, 1 +/- eps, NaN, +/-inf} every call returns
//      (watchdog), within the per-update cap, catches up to `t`, and leaves the
//      particle finite.
//   3. PRODUCTION BINDING: EmitterInstance.cpp calls the header instead of
//      carrying its own copy of the loop.
//
// See the test_bounce_catch_up entry in tests/native-tests.json.

#include "simulation/BounceCatchUp.h"

#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <string>
#include <thread>

static int g_failed = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// Minimal stand-in for the D3DXVECTOR3 float operators the original loop used
// (component-wise, FLOAT scalar), so the oracle below is the shipped code.
struct Vec3
{
    float x, y, z;
};
static Vec3 operator+(const Vec3& a, const Vec3& b) { Vec3 r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static Vec3 operator*(const Vec3& v, float f)       { Vec3 r = { v.x * f, v.y * f, v.z * f }; return r; }
static Vec3 operator*(float f, const Vec3& v)       { Vec3 r = { f * v.x, f * v.y, f * v.z }; return r; }

struct Particle
{
    Vec3  m_initialPosition;
    Vec3  m_initialSpeed;
    Vec3  m_acceleration;
    float m_positionTime;
    float m_bounceTime;
};

// The pre-fix loop from EmitterInstance::UpdateParticle, verbatim apart from
// `m_emitter.bounciness` becoming a parameter, `0.5` being spelled `0.5f`
// (D3DX's operator*(FLOAT, const D3DXVECTOR3&) made the same conversion) and a
// `maxSteps` bound so the oracle can be stopped where the new loop stopped.
static int OldBounceLoop(Particle& particle, float t, float bounciness, int maxSteps)
{
        int steps = 0;
        while (t > particle.m_bounceTime && steps < maxSteps)
        {
            // The particle has bounced
            float bt = particle.m_bounceTime - particle.m_positionTime;
            particle.m_initialPosition =  particle.m_initialPosition + (particle.m_initialSpeed + 0.5f * particle.m_acceleration * bt) * bt;
            particle.m_initialSpeed    =  particle.m_initialSpeed + particle.m_acceleration * bt;
            particle.m_initialSpeed.z  = -particle.m_initialSpeed.z * bounciness;
            particle.m_positionTime    =  particle.m_bounceTime;

            // Calculate new bounce time
            if (particle.m_acceleration.z == 0 || particle.m_initialSpeed.z == 0)
            {
                // No more bounces
                particle.m_bounceTime = FLT_MAX;
            }
            else
            {
                // Calculate the new parabola
                // We know x(0) is 0, so the problem becomes a lot simpler
                particle.m_bounceTime += 2 * -particle.m_initialSpeed.z / particle.m_acceleration.z;
            }
            steps++;
        }
        return steps;
}

// The same original loop with no stop, bounded by an iteration budget so the
// test can show what it did without hanging. Returns -1 when the budget ran out.
static long long UnboundedOldBounceLoop(Particle& particle, float t, float bounciness, long long budget)
{
    long long n = 0;
    while (t > particle.m_bounceTime)
    {
        if (++n > budget) return -1;
        float bt = particle.m_bounceTime - particle.m_positionTime;
        particle.m_initialPosition =  particle.m_initialPosition + (particle.m_initialSpeed + 0.5f * particle.m_acceleration * bt) * bt;
        particle.m_initialSpeed    =  particle.m_initialSpeed + particle.m_acceleration * bt;
        particle.m_initialSpeed.z  = -particle.m_initialSpeed.z * bounciness;
        particle.m_positionTime    =  particle.m_bounceTime;
        if (particle.m_acceleration.z == 0 || particle.m_initialSpeed.z == 0)
            particle.m_bounceTime = FLT_MAX;
        else
            particle.m_bounceTime += 2 * -particle.m_initialSpeed.z / particle.m_acceleration.z;
    }
    return n;
}

static int NewBounceLoop(Particle& p, float t, float bounciness)
{
    return BounceCatchUp(&p.m_initialPosition.x, &p.m_initialSpeed.x, &p.m_acceleration.x,
                         p.m_positionTime, p.m_bounceTime, t, bounciness);
}

// Watchdog: a candidate call that never returns can't be joined, so a monitor
// thread reports it and exits instead of letting the lane hang.
static std::atomic<long long> g_deadlineMs(0);
static char g_watchLabel[256];

static long long NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void WatchdogMain()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const long long deadline = g_deadlineMs.load();
        if (deadline != 0 && NowMs() > deadline)
        {
            std::printf("  FAIL: %s did not return within 10 s (infinite bounce loop)\n", g_watchLabel);
            std::printf("=== FAILED ===\n");
            std::fflush(stdout);
            std::_Exit(1);
        }
    }
}

static int GuardedNewLoop(Particle& p, float t, float b, const std::string& label)
{
    std::snprintf(g_watchLabel, sizeof(g_watchLabel), "%s", label.c_str());
    g_deadlineMs.store(NowMs() + 10000);
    const int steps = NewBounceLoop(p, t, b);
    g_deadlineMs.store(0);
    return steps;
}

// Spawn as EmitterInstance::SpawnParticle does for GROUND_BOUNCE: first ground
// contact from the parabola's later root (or the linear case).
static Particle Spawn(Vec3 pos, Vec3 speed, Vec3 acc)
{
    Particle p;
    p.m_initialPosition = pos;
    p.m_initialSpeed    = speed;
    p.m_acceleration    = acc;
    p.m_positionTime    = 0;
    if (acc.z != 0)
    {
        float D  = sqrtf(speed.z * speed.z - 2 * acc.z * pos.z);
        float t0 = (-speed.z - D) / acc.z;
        float t1 = (-speed.z + D) / acc.z;
        p.m_bounceTime = (t0 > t1) ? t0 : t1;
    }
    else if (speed.z != 0)
    {
        p.m_bounceTime = -pos.z / speed.z;
        if (p.m_bounceTime < 0) p.m_bounceTime = FLT_MAX;
    }
    else
    {
        p.m_bounceTime = FLT_MAX;
    }
    return p;
}

static bool Same(const Particle& a, const Particle& b)
{
    return a.m_initialPosition.x == b.m_initialPosition.x &&
           a.m_initialPosition.y == b.m_initialPosition.y &&
           a.m_initialPosition.z == b.m_initialPosition.z &&
           a.m_initialSpeed.x    == b.m_initialSpeed.x    &&
           a.m_initialSpeed.y    == b.m_initialSpeed.y    &&
           a.m_initialSpeed.z    == b.m_initialSpeed.z    &&
           a.m_positionTime      == b.m_positionTime      &&
           a.m_bounceTime        == b.m_bounceTime;
}

static bool Finite(const Particle& p)
{
    return std::isfinite(p.m_initialPosition.x) && std::isfinite(p.m_initialPosition.y) &&
           std::isfinite(p.m_initialPosition.z) && std::isfinite(p.m_initialSpeed.x) &&
           std::isfinite(p.m_initialSpeed.y)    && std::isfinite(p.m_initialSpeed.z) &&
           std::isfinite(p.m_positionTime)      && std::isfinite(p.m_bounceTime);
}

// One update of the new loop checked against the oracle. `oracle` carries the
// original's state between updates (it adopts the settled state once the new
// loop settles, as the particle in the engine would). Returns false on any
// mismatch; counts bounces and settles for the report line.
static bool CompareUpdate(Particle& candidate, Particle& oracle, float t, float b,
                          int& bounces, int& settles, int& cappedSettles)
{
    const bool wasBouncing = candidate.m_bounceTime != FLT_MAX;
    const int newSteps = NewBounceLoop(candidate, t, b);
    const int oldSteps = OldBounceLoop(oracle, t, b, newSteps);
    bounces += newSteps;
    if (oldSteps != newSteps) return false;
    if (Same(candidate, oracle)) return true;

    // The only allowed difference: the new loop settled after its last bounce.
    if (!wasBouncing || candidate.m_bounceTime != FLT_MAX || oracle.m_bounceTime == FLT_MAX)
        return false;
    const float delta  = 2 * -oracle.m_initialSpeed.z / oracle.m_acceleration.z;
    const bool  degenerate = !(delta > 0) || oracle.m_positionTime + delta == oracle.m_positionTime;
    const bool  capped     = newSteps >= kMaxBounceCatchUpSteps && t > oracle.m_bounceTime;
    Particle settled = oracle;
    settled.m_initialSpeed.z = 0.0f;
    settled.m_bounceTime     = FLT_MAX;
    if (!(degenerate || capped) || !Same(candidate, settled))
        return false;
    settles++;
    if (!degenerate) cappedSettles++;
    oracle = settled;
    return true;
}

struct Scenario
{
    const char* name;
    Vec3 pos, speed, acc;
};

static const Scenario kScenarios[] = {
    { "dropped from 10",          { 0, 0, 10 },    { 0, 0, 0 },     { 0, 0, -9.8f } },
    { "thrown up and sideways",   { 1, -2, 3 },    { 4, 1.5f, 12 }, { 0.5f, 0, -25 } },
    { "fast and low",             { 0, 0, 0.25f }, { 30, 0, -40 },  { 0, -3, -300 } },
    { "linear fall (no gravity)", { 0, 0, 5 },     { 1, 0, -2 },    { 0, 0, 0 } },
};

static std::string Label(const char* what, const Scenario& s, float b)
{
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s: %s, b=%.9g", what, s.name, b);
    return buf;
}

static std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

int main()
{
    std::thread(WatchdogMain).detach();

    // --- 1. EXACT equality with the original loop over the ordinary range.
    std::printf("[exact] new loop == original loop for 0 <= b < 1\n");
    const float kOrdinary[] = { 0.0f, 0.2f, 0.25f, 0.5f, 0.9f };
    for (const Scenario& s : kScenarios)
    {
        for (float b : kOrdinary)
        {
            // Frame by frame at 30 fps for 40 s: every intermediate state matches.
            Particle oracle = Spawn(s.pos, s.speed, s.acc);
            Particle p      = oracle;
            int bounces = 0, settles = 0, capped = 0;
            bool ok = true;
            for (int frame = 1; frame <= 1200 && ok; frame++)
            {
                std::snprintf(g_watchLabel, sizeof(g_watchLabel), "%s", Label("exact frame update", s, b).c_str());
                g_deadlineMs.store(NowMs() + 10000);
                ok = CompareUpdate(p, oracle, frame / 30.0f, b, bounces, settles, capped);
                g_deadlineMs.store(0);
            }
            std::printf("    (%d bounces, %d settle%s, %d at the cap)\n", bounces, settles, settles == 1 ? "" : "s", capped);
            CHECK(ok && bounces > 0, Label("per-frame state matches the original", s, b).c_str());

            // One large catch-up (a seek or a long stall).
            Particle oracleJ = Spawn(s.pos, s.speed, s.acc);
            Particle pJ      = oracleJ;
            bounces = settles = capped = 0;
            std::snprintf(g_watchLabel, sizeof(g_watchLabel), "%s", Label("exact jump update", s, b).c_str());
            g_deadlineMs.store(NowMs() + 10000);
            ok = CompareUpdate(pJ, oracleJ, 1000.0f, b, bounces, settles, capped);
            g_deadlineMs.store(0);
            CHECK(ok, Label("single-jump state matches the original", s, b).c_str());
            CHECK(pJ.m_bounceTime == FLT_MAX, Label("bouncing has stopped by t=1000", s, b).c_str());
        }
    }

    // What the original did with no stop at all (informational: why the fix
    // exists). Budgeted, so it can't hang the test.
    for (float b : kOrdinary)
    {
        Particle p = Spawn(kScenarios[2].pos, kScenarios[2].speed, kScenarios[2].acc);
        const long long n = UnboundedOldBounceLoop(p, 1000.0f, b, 20000000LL);
        if (n < 0)
            std::printf("  note: unbounded original, %s, b=%g: still looping after 20M iterations (bounceTime=%g)\n",
                        kScenarios[2].name, b, p.m_bounceTime);
        else
            std::printf("  note: unbounded original, %s, b=%g: finished after %lld iterations\n",
                        kScenarios[2].name, b, n);
    }

    // --- 2. TERMINATION for the values the original loop could not survive.
    std::printf("[termination] every update returns, caught up and finite\n");
    const float kHostile[] = {
        -1.0f,
        1.0f,
        std::nextafter(1.0f, 2.0f),
        std::nextafter(1.0f, 0.0f),
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (const Scenario& s : kScenarios)
    {
        for (float b : kHostile)
        {
            // Frame by frame for 60 s, then one huge jump.
            Particle p = Spawn(s.pos, s.speed, s.acc);
            bool ok = true;
            for (int frame = 1; frame <= 1800 && ok; frame++)
            {
                const float t = frame / 30.0f;
                const int steps = GuardedNewLoop(p, t, b, Label("frame update", s, b));
                ok = steps <= kMaxBounceCatchUpSteps && p.m_bounceTime >= t && Finite(p);
            }
            CHECK(ok, Label("per-frame updates capped, caught up, finite", s, b).c_str());

            const int steps = GuardedNewLoop(p, 1.0e6f, b, Label("jump update", s, b));
            CHECK(steps <= kMaxBounceCatchUpSteps && p.m_bounceTime >= 1.0e6f && Finite(p),
                  Label("1e6 s jump capped, caught up, finite", s, b).c_str());
        }
    }

    // A tiny bounce late in a long life at b = 1 is the in-place spin: the arc
    // is far below an ulp of bounceTime, so the original `+=` never moved it.
    {
        Particle p;
        p.m_initialPosition = { 0, 0, 0 };
        p.m_initialSpeed    = { 0, 0, -1.0e-5f };
        p.m_acceleration    = { 0, 0, -9.8f };
        p.m_positionTime    = 4096.0f;
        p.m_bounceTime      = 4096.0f;
        const int steps = GuardedNewLoop(p, 4097.0f, 1.0f, "sub-ulp arc at b=1");
        CHECK(steps == 1 && p.m_bounceTime == FLT_MAX && p.m_initialSpeed.z == 0.0f,
              "an arc shorter than an ulp of bounceTime settles instead of spinning");
    }

    // Many legitimate bounces in one update stop at the cap and settle.
    {
        Particle p = Spawn({ 0, 0, 1.0e-6f }, { 0, 0, 0 }, { 0, 0, -9.8f });
        const int steps = GuardedNewLoop(p, 4096.0f, 1.0f, "capped catch-up at b=1");
        CHECK(steps == kMaxBounceCatchUpSteps && p.m_bounceTime == FLT_MAX && p.m_initialSpeed.z == 0.0f,
              "a catch-up longer than the per-update cap settles at the cap");
    }

    // A non-finite time can't be caught up to; it must not loop either.
    {
        Particle p = Spawn({ 0, 0, 10 }, { 0, 0, 0 }, { 0, 0, -9.8f });
        const Particle before = p;
        const int steps = GuardedNewLoop(p, std::numeric_limits<float>::infinity(), 0.5f, "t=+inf");
        CHECK(steps == 0 && Same(p, before), "t = +inf is ignored");
    }

    // --- 3. PRODUCTION BINDING: UpdateParticle uses the header.
    {
        const std::filesystem::path repoRoot =
            std::filesystem::path(__FILE__).parent_path().parent_path();
        const std::string source = ReadSource(repoRoot / "src" / "simulation" / "EmitterInstance.cpp");
        CHECK(!source.empty(), "EmitterInstance.cpp is readable");
        CHECK(std::regex_search(source, std::regex(R"(#include\s+"BounceCatchUp\.h")")),
              "EmitterInstance.cpp includes BounceCatchUp.h");
        CHECK(std::regex_search(source, std::regex(
            R"(BounceCatchUp\s*\(\s*particle\.m_initialPosition\s*,\s*particle\.m_initialSpeed\s*,\s*particle\.m_acceleration\s*,\s*particle\.m_positionTime\s*,\s*particle\.m_bounceTime\s*,\s*t\s*,\s*m_emitter\.bounciness\s*\))")),
              "UpdateParticle calls BounceCatchUp with the particle's state");
        CHECK(!std::regex_search(source, std::regex(R"(while\s*\(\s*t\s*>\s*particle\.m_bounceTime\s*\))")),
              "no private copy of the bounce loop remains in EmitterInstance.cpp");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
