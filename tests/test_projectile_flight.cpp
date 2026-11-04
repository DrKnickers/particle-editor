// Test for the curved projectile paths (src/simulation/ProjectileFlight.h, #967).
//
// Each path is checked against its own literal re-run of the decompiled per-frame
// service (ProjectileBehaviorClass::Service_As_*_Path_Projectile), written here
// independently of the header, plus the facing math, the lob solver and the ends.

#include "simulation/ProjectileFlight.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) \
    do { if (cond) std::printf("  ok: %s\n", msg); else { std::printf("  FAIL: %s\n", msg); ++g_fail; } } while (0)

static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static double Dist(const float a[3], const float b[3])
{
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Reference missile loop: yaw/pitch in degrees, each axis stepped toward the target
// by at most `rate` (Turn_To_Face), then a move of `speed` along the new facing.
static std::vector<FlightSample> ReferenceMissile(const float launch[3], float yaw0, float pitch0,
                                                  const float target[3], float speed, float rate, float range)
{
    std::vector<FlightSample> path;
    float pos[3] = { launch[0], launch[1], launch[2] };
    float yaw = yaw0, pitch = pitch0;
    auto facingDir = [&](float d[3]) {
        d[0] = std::cos(pitch * 0.017453292f) * std::cos(yaw * 0.017453292f);
        d[1] = std::cos(pitch * 0.017453292f) * std::sin(yaw * 0.017453292f);
        d[2] = -std::sin(pitch * 0.017453292f);
    };
    auto stepToward = [rate](float cur, float want) {
        float d = std::fmod(want - cur, 360.0f);
        if (d > 180.0f) d -= 360.0f;
        if (d < -180.0f) d += 360.0f;
        return cur + (d > rate ? rate : (d < -rate ? -rate : d));
    };
    FlightSample s;
    float d[3];
    facingDir(d);
    for (int i = 0; i < 3; i++) { s.pos[i] = pos[i]; s.facing[i] = d[i]; }
    path.push_back(s);
    float travelled = 0.0f;
    const float maxTravel = range + std::fabs(target[2] - launch[2]);
    for (int frame = 1; frame < kMaxFlightFrames; frame++)
    {
        const float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
        float wantYaw = std::atan2(dy, dx) * 57.295776f;
        if (wantYaw < 0) wantYaw += 360.0f;
        const float wantPitch = -std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * 57.295776f;
        pitch = stepToward(pitch, wantPitch);
        yaw   = stepToward(yaw, wantYaw);
        facingDir(d);
        for (int i = 0; i < 3; i++) pos[i] += d[i] * speed;
        for (int i = 0; i < 3; i++) { s.pos[i] = pos[i]; s.facing[i] = d[i]; }
        path.push_back(s);
        travelled += speed;
        if (travelled >= maxTravel || Dist(pos, target) <= speed) break;
    }
    return path;
}

static bool SamePath(const std::vector<FlightSample>& a, const std::vector<FlightSample>& b, double tol)
{
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); k++)
        for (int i = 0; i < 3; i++)
            if (!Near(a[k].pos[i], b[k].pos[i], tol) || !Near(a[k].facing[i], b[k].facing[i], 1e-3)) return false;
    return true;
}

int main()
{
    std::printf("[kinds]\n");
    {
        CHECK(ProjectileFlightKindFor("Missile")      == ProjectileFlightKind::Missile, "MISSILE homes");
        CHECK(ProjectileFlightKindFor("BOMB")         == ProjectileFlightKind::Bomb,    "BOMB falls");
        CHECK(ProjectileFlightKindFor("Cluster_Bomb") == ProjectileFlightKind::Bomb,    "CLUSTER_BOMB falls");
        CHECK(ProjectileFlightKindFor("GRENADE")      == ProjectileFlightKind::Grenade, "GRENADE lobs");
        CHECK(ProjectileFlightKindFor("ROCKET")       == ProjectileFlightKind::Rocket,  "ROCKET follows a spline");
        CHECK(ProjectileFlightKindFor("MPTL_Rocket")  == ProjectileFlightKind::Rocket,  "MPTL_ROCKET follows a spline");
        CHECK(ProjectileFlightKindFor("Light_Saber")  == ProjectileFlightKind::Direct,  "LIGHT_SABER not emulated");
        CHECK(ProjectileFlightKindFor("LASER")        == ProjectileFlightKind::Direct,  "LASER straight");
        CHECK(ProjectileFlightKindFor("")             == ProjectileFlightKind::Direct,  "no category straight");
        CHECK(IsProjectilePathEmulated("Laser") && IsProjectilePathEmulated("") && IsProjectilePathEmulated("MISSILE") &&
              IsProjectilePathEmulated("cluster_bomb") && IsProjectilePathEmulated("GRENADE") &&
              IsProjectilePathEmulated("ROCKET") && IsProjectilePathEmulated("MPTL_Rocket"),
              "straight, missile, bomb, grenade and rocket paths are emulated");
        CHECK(!IsProjectilePathEmulated("Light_Saber"), "the lightsaber throw is not");
        CHECK(IsClusterBombCategory("Cluster_Bomb") && !IsClusterBombCategory("BOMB"), "cluster bomb told apart");
        CHECK(ProjectileUsesTarget("MISSILE") && ProjectileUsesTarget("Grenade") && ProjectileUsesTarget("CLUSTER_BOMB") &&
              ProjectileUsesTarget("ROCKET") && !ProjectileUsesTarget("BOMB") && !ProjectileUsesTarget("Laser") &&
              !ProjectileUsesTarget("Light_Saber"),
              "missiles, grenades, rockets and cluster bombs aim at the target");
    }

    std::printf("[facing]\n");
    {
        const float o[3] = { 0, 0, 0 };
        const float east[3] = { 10, 0, 0 }, north[3] = { 0, 10, 0 }, west[3] = { -10, 0, 0 }, south[3] = { 0, -10, 0 };
        CHECK(Near(GameFacingFromTo(o, east).yawDeg, 0, 1e-4),    "+X -> yaw 0");
        CHECK(Near(GameFacingFromTo(o, north).yawDeg, 90, 1e-3),  "+Y -> yaw 90");
        CHECK(Near(GameFacingFromTo(o, west).yawDeg, 180, 1e-3),  "-X -> yaw 180");
        CHECK(Near(GameFacingFromTo(o, south).yawDeg, 270, 1e-3), "-Y -> yaw 270 (negatives wrap)");
        const float up45[3] = { 10, 0, 10 };
        const GameFacing f = GameFacingFromTo(o, up45);
        CHECK(Near(f.pitchDeg, -45, 1e-3), "45 degrees up -> pitch -45 (nose up is negative)");
        float d[3];
        GameFacingDirection(f, d);
        CHECK(Near(d[0], 0.7071, 1e-3) && Near(d[1], 0, 1e-5) && Near(d[2], 0.7071, 1e-3), "pitch -45 points up");
        GameFacing w; w.yawDeg = 350.0f;
        GameFacing t; t.yawDeg = 10.0f;
        CHECK(Near(GameTurnToFace(w, t, 3.0f).yawDeg, 353.0f, 1e-4), "turning wraps through 0 (Clamp_180)");
        CHECK(Near(GameTurnToFace(w, t, 30.0f).yawDeg, 370.0f, 1e-4), "a big enough rate lands on the target heading");
    }

    std::printf("[missile]\n");
    {
        ProjectileFlightParams p;
        p.speedPerFrame = 7.0f; p.range = 2200.0f; p.turnRateDegPerFrame = 3.0f;   // Proj_Ship_Concussion_Missile
        const float launch[3] = { 0, 0, 0 }, aim[3] = { 0, 1, 0 }, target[3] = { 300, 0, 0 };
        std::vector<FlightSample> path;
        CHECK(ComputeProjectileFlight(ProjectileFlightKind::Missile, p, launch, aim, target, path), "missile flies");
        const std::vector<FlightSample> ref = ReferenceMissile(launch, 90.0f, 0.0f, target, 7.0f, 3.0f, 2200.0f);
        CHECK(SamePath(path, ref, 1e-2), "path = the decompiled turn-then-move loop, frame by frame");
        CHECK(path.size() > 2 && Near(path[0].facing[1], 1.0, 1e-5), "launches along the aim (+Y)");
        CHECK(path.size() > 2 && Near(path[1].facing[1], std::sin(87.0 * 3.14159265 / 180.0), 1e-4),
              "first frame turns 3 degrees toward the target");
        CHECK(!path.empty() && Dist(path.back().pos, target) <= 7.0, "ends within one step of the target");
        CHECK(path.size() < 120, "reaches it well inside its range");

        p.turnRateDegPerFrame = 0.0f;   // mass drivers: a missile that never turns
        CHECK(ComputeProjectileFlight(ProjectileFlightKind::Missile, p, launch, aim, target, path), "rate 0 flies");
        bool straight = true;
        for (const FlightSample& s : path) straight = straight && Near(s.pos[0], 0, 1e-3);
        CHECK(straight, "turn rate 0 never turns");
        CHECK(path.size() == (size_t)std::ceil(2200.0 / 7.0) + 1, "and dies at its range (ceil(range / speed) frames)");
    }

    std::printf("[bomb]\n");
    {
        ProjectileFlightParams p;
        p.speedPerFrame = 4.0f; p.range = 10000.0f; p.bombFallAccel = 0.2f; p.groundZ = 0.0f;   // Y_Wing_Bombing_Run_Bomb
        const float launch[3] = { 0, 0, 100 }, aim[3] = { 1, 0, 0 }, target[3] = { 500, 0, 0 };
        std::vector<FlightSample> path;
        CHECK(ComputeProjectileFlight(ProjectileFlightKind::Bomb, p, launch, aim, target, path), "bomb flies");
        CHECK(path.size() > 5 && Near(path[1].pos[2], 100.0, 1e-4) && Near(path[2].pos[2], 99.8, 1e-4) &&
              Near(path[3].pos[2], 99.4, 1e-4) && Near(path[4].pos[2], 98.8, 1e-4),
              "drops 0, 0.2, 0.4, 0.6 ... per frame (fall rate grows by the accel)");
        CHECK(path.size() > 5 && Near(path[4].pos[0], 16.0, 1e-4), "moves Speed along its launch facing each frame");
        bool fixedFacing = true;
        for (const FlightSample& s : path) fixedFacing = fixedFacing && Near(s.facing[0], 1.0, 1e-6);
        CHECK(fixedFacing, "its facing never changes while it falls");
        CHECK(!path.empty() && Near(path.back().pos[2], 0.0, 1e-3), "ends on the ground");

        p.clusterBomb = true;
        const float high[3] = { 0, 0, 0 }, below[3] = { 0, 200, 0 };
        const float launch2[3] = { 0, 0, 50 };
        CHECK(ComputeProjectileFlight(ProjectileFlightKind::Bomb, p, launch2, high, below, path), "cluster bomb flies");
        CHECK(path.size() > 1 && path[1].pos[1] > 0.0f && Near(path[1].pos[0], 0, 1e-3),
              "a cluster bomb launches toward the target");
    }

    std::printf("[grenade]\n");
    {
        float angle = 0.0f;
        CHECK(GameLobAngle(100.0f, 0.0f, 10.0f, 0.56f, angle), "level lob in reach solves");
        const double highLob = 3.14159265 / 2.0 - 0.5 * std::asin(0.56 * 100.0 / 100.0);
        CHECK(Near(angle, highLob, 1e-4), "level lob = the high solution of sin(2a) = g d / v^2");
        CHECK(!GameLobAngle(200.0f, 0.0f, 10.0f, 0.56f, angle), "beyond v^2 / g can't be reached");

        ProjectileFlightParams p;   // Proj_Merc_Concussion_Grenade
        p.speedPerFrame = 10.0f; p.grenadeGravity = 0.7f; p.grenadeLobMod = 0.8f; p.grenadeCanLobSlower = true;
        p.maxLifetimeSec = 900.0f; p.range = 50.0f;
        float lobSpeed = 0.0f;
        const float launch[3] = { 0, 0, 0 }, target[3] = { 100, 0, 0 }, aim[3] = { 0, 1, 0 };
        CHECK(GameGrenadeLob(p, launch, target, angle, lobSpeed) && Near(lobSpeed, 8.0, 1e-4),
              "can lob slower: 10 -> 8 reaches, 6.4 doesn't, so it throws at 8");

        std::vector<FlightSample> path;
        CHECK(ComputeProjectileFlight(ProjectileFlightKind::Grenade, p, launch, aim, target, path), "grenade flies");
        bool closedForm = path.size() > 3;
        float dir[3];
        GameFacing f; f.yawDeg = 0.0f; f.pitchDeg = -(angle * 57.295776f);
        GameFacingDirection(f, dir);
        for (size_t k = 1; closedForm && k + 1 < path.size(); k++)
        {
            const float t = (float)k;
            closedForm = Near(path[k].pos[0], t * dir[0] * 8.0f, 1e-3) &&
                         Near(path[k].pos[2], t * dir[2] * 8.0f + t * (0.5f * t * -0.56f), 1e-3);
        }
        CHECK(closedForm, "path = P0 + t (dir v + t/2 (0,0,-g)) each frame, facing the target, not the aim");
        CHECK(!path.empty() && Near(path.back().pos[0], 100.0, 1.0) && Near(path.back().pos[2], 0.0, 1e-3),
              "lands on the target");
        CHECK(path.size() > 3 && path[1].facing[2] > 0.0f && path[path.size() - 2].facing[2] < 0.0f,
              "faces up on the way up and down on the way down");

        p.grenadeCanLobSlower = false;
        const float far[3] = { 200, 0, 0 };
        CHECK(!ComputeProjectileFlight(ProjectileFlightKind::Grenade, p, launch, aim, far, path) && path.empty(),
              "out of lob range: the game doesn't fire");

        p.maxLifetimeSec = 0.1f;
        CHECK(ComputeProjectileFlight(ProjectileFlightKind::Grenade, p, launch, aim, target, path) && path.size() == 5,
              "a short <Projectile_Max_Lifetime> ends it once age > lifetime (frame 4)");
    }

    std::printf("[sampling]\n");
    {
        std::vector<FlightSample> path(3);
        for (int k = 0; k < 3; k++)
            for (int i = 0; i < 3; i++) { path[k].pos[i] = 0.0f; path[k].facing[i] = 0.0f; }
        path[1].pos[0] = 10.0f; path[1].facing[0] = 1.0f;
        path[2].pos[0] = 10.0f; path[2].pos[1] = 5.0f; path[2].facing[1] = 1.0f;
        float pos[3], vel[3], face[3];
        CHECK(Near(FlightPathSeconds(path), 2.0 / 30.0, 1e-6), "a 3-sample path lives 2 logic frames");
        CHECK(SampleFlightPath(path, 0.5f / 30.0f, pos, vel, face) && Near(pos[0], 5.0, 1e-4) &&
              Near(vel[0], 300.0, 1e-3) && face[0] == 1.0f, "half a frame in: halfway, 30x the step, that step's facing");
        CHECK(SampleFlightPath(path, 1.5f / 30.0f, pos, vel, face) && Near(pos[1], 2.5, 1e-4) && face[1] == 1.0f,
              "second frame: the turn's facing");
        CHECK(SampleFlightPath(path, 1.0f, pos, vel, face) && pos[1] == 5.0f && vel[0] == 0.0f,
              "past the end it rests on the last sample");
        std::vector<FlightSample> none;
        CHECK(!SampleFlightPath(none, 0.0f, pos, vel, face), "an empty path samples nothing");
    }

    std::printf("[rocket]\n");
    {
        int draws = 0;
        auto noOffset  = [&draws](float lo, float) { ++draws; return lo; };
        auto maxOffset = [](float lo, float hi) { return (lo == 0.0f && hi > 6.0f && hi < 6.3f) ? 1.0f : hi; };

        ProjectileFlightParams p;
        p.speedPerFrame          = 8.0f;
        p.range                  = 2000.0f;
        p.rocketStraightDistance = 100.0f;
        p.rocketCurveDistance    = 50.0f;
        p.rocketCurveOffset      = 15.0f;
        p.rocketExplodesAtTarget = true;
        p.inSpace                = true;
        p.turnRateDegPerFrame    = 30.0f;
        p.randomInRange          = noOffset;

        const float launch[3] = { 0, 0, 0 }, target[3] = { 1000, 0, 0 }, aimX[3] = { 1, 0, 0 };
        float dir[3] = { 1, 0, 0 };

        // Space, no offsets: points every 55 (50 x 1.1); the straight 100 takes ceil(100/55) = 2,
        // the 900 to the target ceil(900/55) = 17. Each point but a segment's last draws twice.
        RocketSpline spline;
        RocketPathDetail::BuildSpaceRocketPath(spline, p, launch, dir, target);
        CHECK(spline.ControlPointCount() == 1 + 2 + 17, "space: launch + 2 straight + 17 curve points");
        CHECK(draws == 2 * (1 + 16), "an angle and a length per offset point; segment ends take none");
        bool collinear = true;
        for (int k = 0; k < spline.ControlPointCount(); k++)
            if (spline.ControlPoint(k)[1] != 0.0f || spline.ControlPoint(k)[2] != 0.0f) collinear = false;
        CHECK(collinear, "zero offsets keep every point on the launch-target line");

        std::vector<FlightSample> path;
        CHECK(ComputeProjectileFlight(ProjectileFlightKind::Rocket, p, launch, aimX, target, path), "space rocket flies");
        CHECK(Dist(path.back().pos, target) <= 8.0 + 1e-3, "it ends within one step of the target");
        CHECK(path.size() > 120 && path.size() < 130, "about 1000 / 8 frames");

        // Offsets: the largest draw pushes each point Curve_Offset off the line, at right angles.
        p.randomInRange = maxOffset;
        RocketSpline bent;
        RocketPathDetail::BuildSpaceRocketPath(bent, p, launch, dir, target);
        bool onCircle = true;
        for (int k = 3; k < bent.ControlPointCount() - 1; k++)
        {
            const float* q = bent.ControlPoint(k);
            if (!Near(std::sqrt(q[1] * q[1] + q[2] * q[2]), 15.0, 1e-2)) onCircle = false;
        }
        CHECK(onCircle, "target-segment points sit Curve_Offset off the line");
        const float* straightEnd = bent.ControlPoint(2);
        CHECK(Near(straightEnd[0], 100.0, 1e-3) && straightEnd[1] == 0.0f, "the straight start ends exactly 100 along the aim");
        const float* lastPoint = bent.ControlPoint(bent.ControlPointCount() - 1);
        CHECK(lastPoint[0] == 1000.0f && lastPoint[1] == 0.0f, "and the path ends exactly on the target");

        // No explode: an overshoot past the target, cut at the flight distance.
        p.randomInRange          = noOffset;
        p.rocketExplodesAtTarget = false;
        p.range                  = 1400.0f;
        RocketSpline overshoot;
        RocketPathDetail::BuildSpaceRocketPath(overshoot, p, launch, dir, target);
        CHECK(overshoot.ControlPoint(overshoot.ControlPointCount() - 1)[0] > 1000.0f &&
              overshoot.Length() <= 1400.0f + 1.0f, "space overshoot runs past the target, cut near the flight distance");

        // Turn rate 0: the model keeps its launch facing all the way.
        p.turnRateDegPerFrame = 0.0f;
        const float aimUp[3] = { 1, 0, 1 };
        ComputeProjectileFlight(ProjectileFlightKind::Rocket, p, launch, aimUp, target, path);
        bool fixedFacing = true;
        for (const FlightSample& s : path)
            if (!Near(s.facing[0], path[0].facing[0], 1e-5) || !Near(s.facing[2], path[0].facing[2], 1e-5))
                fixedFacing = false;
        CHECK(fixedFacing && path[0].facing[2] > 0.5f, "turn rate 0: facing never leaves the launch aim");

        // Land: half the distance straight whatever the tag says, points held at the
        // launch's clearance, and a target on the terrain pulls the last point down.
        p.inSpace                = false;
        p.rocketStraightDistance = 0.0f;
        p.rocketTargetOnTerrain  = true;
        p.rocketExplodesAtTarget = true;
        p.groundZ                = 0.0f;
        const float high[3] = { 0, 0, 40 };
        RocketSpline land;
        const float landExplode = RocketPathDetail::BuildLandRocketPath(land, p, high, dir, target);
        CHECK(land.ControlPointCount() == 1 + 10 + 10, "land: ceil(500/55) points to half way, then ceil(~500/55) to the target");
        bool heldUp = true;
        for (int k = 1; k < land.ControlPointCount() - 1; k++)
            if (land.ControlPoint(k)[2] < 40.0f - 1e-3f) heldUp = false;
        CHECK(heldUp, "every point before the target stays at the launch's 40 above the ground");
        CHECK(land.ControlPoint(land.ControlPointCount() - 1)[2] == 0.0f, "target on terrain: the last point is on the ground");
        CHECK(Near(landExplode, land.Length(), 1e-3), "explodes at target radius: stops at the path's length");

        float previous[3] = { 0, 0, 100 }, point[3] = { 5, 0, 10 };
        RocketPathDetail::AdjustHeight(point, previous, 40.0f, 0.0f);
        CHECK(point[2] == 70.0f, "Adjust_Height: no lower than 0.7 x the previous point's height");

        p.rocketCurveDistance = 0.0f;
        CHECK(!ComputeProjectileFlight(ProjectileFlightKind::Rocket, p, high, aimX, target, path),
              "no curve distance: no points, so the rocket detonates on its first frame");
    }

    std::printf("[guards]\n");
    {
        ProjectileFlightParams p;
        const float o[3] = { 0, 0, 0 }, t[3] = { 10, 0, 0 };
        std::vector<FlightSample> path;
        CHECK(!ComputeProjectileFlight(ProjectileFlightKind::Missile, p, o, o, t, path), "no speed -> no shot");
    }

    std::printf(g_fail ? "\n=== ProjectileFlight: %d FAILED ===\n" : "\n=== ProjectileFlight: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
