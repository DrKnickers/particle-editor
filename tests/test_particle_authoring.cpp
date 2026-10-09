// Unit test for the shared authoring rules (src/effect/ParticleAuthoring.cpp).
//
// Three parts:
//
//  (a) PARITY. Each track handler in src/host/BridgeDispatch_EmitterTracks.cpp
//      is copied below, minus its JSON/host plumbing, as a reference. A
//      battery of edits runs through the reference and through the
//      plan -> (undo iff willMutate) -> apply sequence the handlers will use,
//      and every edit must leave identical keys (order included) and the same
//      undo-capture / changed bits. If a handler changes, update its copy here.
//      The set-track-key and add-track-key copies include the track-key
//      bounds fix (time within [0, 100], finite values, color values within
//      [0, 1], dedup searching downward at the 100 edge).
//  (b) DIRECT SEMANTICS: border keys, not-found, dedup cap, lock matrix,
//      interpolation, scalar predicates, batch error precedence, link groups.
//  (c) NEW RULES: full-track replacement (checked against a real save and
//      reload) and the advisory atlas-index classifier.
//  (d) TOOL-ONLY RULES (Phase 4B decision 6): set-property (every accepted
//      edge value checked against a real save and reload), rename, texture
//      names, rescale preconditions, the key-count cap and the lock matrix.
//
// Standalone console exe; see the test_particle_authoring entry in
// tests/native-tests.json.

#include "effect/ParticleAuthoring.h"
#include "effect/ParticleSystem.h"
#include "simulation/ParticleSystemInstance.h"
#include "common/files.h"
#include "common/exceptions.h"
#include "common/ResourceLimits.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// Link stub -- see test_rescale.cpp. ~Emitter calls
// ParticleSystemInstance::RemoveEmitter, whose real body is D3D-coupled; no
// EmitterInstance is ever registered here.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Emitter = ParticleSystem::Emitter;
using Track   = ParticleSystem::Emitter::Track;
using Key     = ParticleSystem::Emitter::Track::Key;

static const float kInf = std::numeric_limits<float>::infinity();
static const float kNaN = std::numeric_limits<float>::quiet_NaN();

// Keys in iteration order. Exact float compare: no NaN ever reaches a track here.
static std::vector<Key> keysOf(const Track& t) { return std::vector<Key>(t.keys.begin(), t.keys.end()); }

static bool sameKeys(const std::vector<Key>& a, const std::vector<Key>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (!(a[i].time == b[i].time && a[i].value == b[i].value)) return false;
    return true;
}

static Track makeTrack(std::initializer_list<Key> keys, Track::InterpolationType it = Track::IT_LINEAR)
{
    Track t;
    t.interpolation = it;
    for (const Key& k : keys) t.keys.insert(k);
    return t;
}

// ===========================================================================
// (a) Reference copies of the handlers' logic
// ===========================================================================

// What a handler did besides mutating keys: captured undo, and whether it
// reported a change (invalidate + dirty + events), or the error it sent.
struct Effects
{
    bool        undo    = false;
    bool        changed = false;
    std::string err;
};

static bool sameEffects(const Effects& a, const Effects& b)
{
    return a.undo == b.undo && a.changed == b.changed && a.err == b.err;
}

// emitters/set-track-key (BridgeDispatch_EmitterTracks.cpp, from the empty-track check).
static Effects refSetTrackKey(Track* track, float oldTime, float newTime, float newValue, bool isColor)
{
    Effects fx;
    if (track == nullptr || track->keys.empty()) return fx;
    const float firstTime = track->keys.begin()->time;
    const float lastTime  = track->keys.rbegin()->time;
    const bool isBorder = (oldTime == firstTime || oldTime == lastTime);
    if (isBorder) newTime = oldTime;
    if (!isBorder && !(newTime >= 0.0f && newTime <= 100.0f))
    { fx.err = "set-track-key 'newTime' must be within [0, 100]"; return fx; }
    if (!std::isfinite(newValue))
    { fx.err = "set-track-key 'newValue' must be finite"; return fx; }
    if (isColor && (newValue < 0.0f || newValue > 1.0f))
    { fx.err = "set-track-key 'newValue' must be within [0, 1] on a color track"; return fx; }
    Track::Key probe(oldTime, 0.0f);
    auto it = track->keys.find(probe);
    if (it == track->keys.end()) return fx;
    fx.undo = true;
    track->keys.erase(it);
    track->keys.insert(Track::Key(newTime, newValue));
    fx.changed = true;
    return fx;
}

// emitters/delete-track-keys (from the empty-track check).
static Effects refDeleteTrackKeys(Track* track, const std::vector<float>& times)
{
    Effects fx;
    if (track == nullptr || track->keys.empty()) return fx;
    const float firstTime = track->keys.begin()->time;
    const float lastTime  = track->keys.rbegin()->time;
    fx.undo = true;
    int removed = 0;
    for (float timeVal : times)
    {
        if (timeVal == firstTime || timeVal == lastTime) continue;
        Track::Key probe(timeVal, 0.0f);
        auto it = track->keys.find(probe);
        if (it != track->keys.end())
        {
            track->keys.erase(it);
            removed++;
        }
    }
    fx.changed = removed > 0;
    return fx;
}

// emitters/add-track-key (from the dedup loop; input already validated).
static float refBump(const Track* track, float time, float step)
{
    Track::Key probe(time, 0.0f);
    int safety = 1000;
    while (track->keys.find(probe) != track->keys.end() && safety-- > 0)
    {
        time += step;
        probe = Track::Key(time, 0.0f);
    }
    return time;
}

static float refAddTrackKey(Track* track, float time, float value)
{
    float at = refBump(track, time, 0.001f);
    if (at > 100.0f && time <= 100.0f) at = refBump(track, time, -0.001f);
    track->keys.insert(Track::Key(at, value));
    return at;
}

// emitters/set-track-lock (from the channel lookup). targetIdx is the
// resolved lockTo name, -1 when unresolved.
static Effects refSetTrackLock(Emitter* target, int channelIdx, bool lockToIsNull, int targetIdx)
{
    Effects fx;
    if (channelIdx < 0) { fx.err = "unknown channel"; return fx; }
    if (channelIdx >= 4) return fx;
    Track* desired = nullptr;
    if (lockToIsNull)
        desired = &target->trackContents[channelIdx];
    else if (targetIdx >= 0 && targetIdx < 4 && targetIdx < channelIdx)
        desired = &target->trackContents[targetIdx];
    else
    {
        fx.err = "invalid lockTo channel (must be an earlier RGBA channel)";
        return fx;
    }
    if (target->tracks[channelIdx] == desired) return fx;
    fx.undo = true;
    target->tracks[channelIdx] = desired;
    fx.changed = true;
    return fx;
}

// emitters/set-track-interpolation (from the null-track check).
static Effects refSetTrackInterpolation(Track* track, const std::string& interpName)
{
    Effects fx;
    if (track == nullptr) return fx;
    Track::InterpolationType next;
    if      (interpName == "linear") next = Track::IT_LINEAR;
    else if (interpName == "smooth") next = Track::IT_SMOOTH;
    else if (interpName == "step")   next = Track::IT_STEP;
    else { fx.err = "unknown interpolation"; return fx; }
    if (track->interpolation == next) return fx;
    fx.undo = true;
    track->interpolation = next;
    fx.changed = true;
    return fx;
}

// ---- The same edits through the unit, as the refactored handlers will call it.

static Effects unitSetTrackKey(Track* track, float oldTime, float newTime, float newValue, bool isColor)
{
    Effects fx;
    if (track == nullptr) return fx;
    TrackKeyPlan plan = PlanSetTrackKey(*track, oldTime, newTime, newValue, isColor);
    if (plan.outcome == EditOutcome::Rejected) { fx.err = plan.reason; return fx; }
    fx.undo = plan.willMutate;
    ApplySetTrackKey(*track, plan);
    fx.changed = plan.willMutate;
    return fx;
}

static Effects unitDeleteTrackKeys(Track* track, const std::vector<float>& times)
{
    Effects fx;
    if (track == nullptr || track->keys.empty()) return fx;
    fx.undo = true;   // the handler captures before the loop, unconditionally
    int removed = 0;
    for (float t : times)
    {
        TrackKeyPlan plan = PlanDeleteTrackKey(*track, t);
        ApplyDeleteTrackKey(*track, plan);
        if (plan.willMutate) removed++;
    }
    fx.changed = removed > 0;
    return fx;
}

static float unitAddTrackKey(Track* track, float time, float value)
{
    const float at = DedupedInsertTime(*track, time);
    track->keys.insert(Track::Key(at, value));
    return at;
}

static Effects unitSetTrackLock(Emitter* e, int channelIdx, bool lockToIsNull, int targetIdx)
{
    Effects fx;
    TrackLockPlan plan = PlanSetTrackLock(*e, channelIdx, targetIdx, lockToIsNull);
    if (plan.outcome == EditOutcome::Rejected) { fx.err = plan.reason; return fx; }
    fx.undo = plan.willMutate;
    ApplySetTrackLock(*e, plan);
    fx.changed = plan.willMutate;
    return fx;
}

static Effects unitSetTrackInterpolation(Track* track, const std::string& name)
{
    Effects fx;
    if (track == nullptr) return fx;
    InterpolationPlan plan = PlanSetTrackInterpolation(*track, name);
    if (plan.outcome == EditOutcome::Rejected) { fx.err = plan.reason; return fx; }
    fx.undo = plan.willMutate;
    ApplySetTrackInterpolation(*track, plan);
    fx.changed = plan.willMutate;
    return fx;
}

// Fixture tracks: default endpoints, interior keys, same-time interior keys,
// same-time border keys, a single key (first == last), and empty.
static std::vector<Track> fixtureTracks()
{
    return {
        makeTrack({Key(0, 1), Key(100, 1)}),
        makeTrack({Key(0, 0.2f), Key(25, 0.4f), Key(50, 0.6f), Key(75, 0.8f), Key(100, 1)}),
        makeTrack({Key(0, 0), Key(50, 0.1f), Key(50, 0.2f), Key(50, 0.3f), Key(100, 1)}),
        makeTrack({Key(0, 0), Key(0, 0.5f), Key(60, 0.7f), Key(100, 1), Key(100, 0.9f)}),
        makeTrack({Key(50, 0.5f)}),
        makeTrack({}),
    };
}

static const float kProbeTimes[] = {0, 100, 25, 50, 60, 75, 33, -0.0f, 1e-7f, kInf, -kInf};

static void testParity()
{
    std::printf("(a) parity with the handlers\n");

    // set-track-key
    {
        const float newTimes[]  = {0, 100, 25, 10, 150, -5, 50};
        const float newValues[] = {0.5f, 3};
        int runs = 0, bad = 0;
        for (const Track& base : fixtureTracks())
        for (float oldTime : kProbeTimes)
        for (float newTime : newTimes)
        for (float newValue : newValues)
        for (bool isColor : {false, true})
        {
            Track a = base, b = base;
            Effects fa = refSetTrackKey(&a, oldTime, newTime, newValue, isColor);
            Effects fb = unitSetTrackKey(&b, oldTime, newTime, newValue, isColor);
            runs++;
            if (!sameKeys(keysOf(a), keysOf(b)) || !sameEffects(fa, fb))
            {
                if (bad++ == 0) std::printf("    first mismatch: old=%g new=%g val=%g\n", oldTime, newTime, newValue);
            }
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "set-track-key: %d edits match the handler", runs);
        CHECK(bad == 0, msg);
    }

    // delete-track-keys: single times, then multi-time requests
    {
        std::vector<std::vector<float>> requests;
        for (float t : kProbeTimes) requests.push_back({t});
        requests.push_back({25, 50, 75});
        requests.push_back({0, 100});
        requests.push_back({50, 50, 50, 50});
        requests.push_back({33, 0, 60, 100, 25});
        requests.push_back({});
        int runs = 0, bad = 0;
        for (const Track& base : fixtureTracks())
        for (const auto& times : requests)
        {
            Track a = base, b = base;
            Effects fa = refDeleteTrackKeys(&a, times);
            Effects fb = unitDeleteTrackKeys(&b, times);
            runs++;
            if (!sameKeys(keysOf(a), keysOf(b)) || !sameEffects(fa, fb)) bad++;
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "delete-track-keys: %d requests match the handler", runs);
        CHECK(bad == 0, msg);
    }

    // add-track-key and add-track-keys: the batch is the singular insert
    // repeated, so each key dedups against the keys inserted before it.
    {
        std::vector<std::vector<Key>> batches = {
            {Key(50, 0.3f)}, {Key(0, 0.3f)}, {Key(100, 0.3f)}, {Key(25, 0.3f)},
            {Key(50, 0.1f), Key(50, 0.2f), Key(50, 0.3f)},
            {Key(0, 0.1f), Key(100, 0.2f), Key(0, 0.3f)},
            {Key(60, 0.1f), Key(60.001f, 0.2f), Key(60, 0.3f)},
        };
        int runs = 0, bad = 0;
        for (const Track& base : fixtureTracks())
        for (const auto& batch : batches)
        {
            Track a = base, b = base;
            bool sameTimes = true;
            for (const Key& k : batch)
                sameTimes = sameTimes && refAddTrackKey(&a, k.time, k.value) == unitAddTrackKey(&b, k.time, k.value);
            runs++;
            if (!sameTimes || !sameKeys(keysOf(a), keysOf(b))) bad++;
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "add-track-key(s): %d batches match the handler", runs);
        CHECK(bad == 0, msg);
    }

    // set-track-lock: every channel x every target, from four lock states.
    {
        int runs = 0, bad = 0;
        for (int state = 0; state < 4; state++)
        for (int ch = -1; ch < ParticleSystem::NUM_TRACKS; ch++)
        for (int tgt = -2; tgt < ParticleSystem::NUM_TRACKS; tgt++)   // -2 = lockTo null
        {
            Emitter a, b;
            for (Emitter* e : {&a, &b})
            {
                // 0: default (G/B/A locked to R); 1: all unlocked; 2: B->G; 3: A->B
                if (state >= 1) for (int i = 0; i < 4; i++) e->tracks[i] = &e->trackContents[i];
                if (state == 2) e->tracks[2] = &e->trackContents[1];
                if (state == 3) e->tracks[3] = &e->trackContents[2];
            }
            const bool isNull = tgt == -2;
            Effects fa = refSetTrackLock(&a, ch, isNull, isNull ? -1 : tgt);
            Effects fb = unitSetTrackLock(&b, ch, isNull, isNull ? -1 : tgt);
            bool samePtrs = true;
            for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
                samePtrs = samePtrs && (a.tracks[i] - a.trackContents) == (b.tracks[i] - b.trackContents);
            runs++;
            if (!samePtrs || !sameEffects(fa, fb)) bad++;
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "set-track-lock: %d requests match the handler", runs);
        CHECK(bad == 0, msg);
    }

    // set-track-interpolation
    {
        const char* names[] = {"linear", "smooth", "step", "Linear", "", "cubic"};
        const Track::InterpolationType starts[] = {Track::IT_UNKNOWN, Track::IT_LINEAR, Track::IT_SMOOTH, Track::IT_STEP};
        int runs = 0, bad = 0;
        for (Track::InterpolationType start : starts)
        for (const char* name : names)
        {
            Track a = makeTrack({Key(0, 0), Key(100, 1)}, start), b = a;
            Effects fa = refSetTrackInterpolation(&a, name);
            Effects fb = unitSetTrackInterpolation(&b, name);
            runs++;
            if (a.interpolation != b.interpolation || !sameEffects(fa, fb)) bad++;
        }
        char msg[96];
        std::snprintf(msg, sizeof(msg), "set-track-interpolation: %d requests match the handler", runs);
        CHECK(bad == 0, msg);
    }
}

// ===========================================================================
// (b) Direct semantics
// ===========================================================================

static void testBorderAndNotFound()
{
    std::printf("(b) border keys and not-found\n");
    const Track base = makeTrack({Key(0, 0.2f), Key(50, 0.5f), Key(100, 1)});

    CHECK(IsBorderKeyTime(base, 0) && IsBorderKeyTime(base, 100) && !IsBorderKeyTime(base, 50),
          "IsBorderKeyTime: first and last only");
    CHECK(!IsBorderKeyTime(Track(), 0), "IsBorderKeyTime: empty track has no border");

    TrackKeyPlan del = PlanDeleteTrackKey(base, 0);
    CHECK(del.outcome == EditOutcome::NoOpBorderSkip && !del.willMutate, "delete on a border key: NoOpBorderSkip, no mutation");

    Track t = base;
    TrackKeyPlan set = PlanSetTrackKey(t, 100, 40, 0.7f, false);
    ApplySetTrackKey(t, set);
    CHECK(set.outcome == EditOutcome::AppliedBorderTime && set.willMutate, "set on a border key: AppliedBorderTime, mutates");
    CHECK(t.keys.rbegin()->time == 100 && t.keys.rbegin()->value == 0.7f, "set on a border key: time pinned, VALUE applied");

    t = base;
    set = PlanSetTrackKey(t, 50, 40, 0.9f, false);
    ApplySetTrackKey(t, set);
    CHECK(set.outcome == EditOutcome::Applied && t.keys.find(Key(40, 0))->value == 0.9f && t.keys.find(Key(50, 0)) == t.keys.end(),
          "set on an interior key moves time and value");

    t = base;
    del = PlanDeleteTrackKey(t, 50);
    ApplyDeleteTrackKey(t, del);
    CHECK(del.outcome == EditOutcome::Applied && t.keys.size() == 2, "delete on an interior key removes it");

    TrackKeyPlan missSet = PlanSetTrackKey(base, 33, 40, 1, false);
    TrackKeyPlan missDel = PlanDeleteTrackKey(base, 33);
    CHECK(missSet.outcome == EditOutcome::NoOpNotFound && !missSet.willMutate, "set at an absent time: NoOpNotFound, no undo");
    CHECK(missDel.outcome == EditOutcome::NoOpNotFound && !missDel.willMutate, "delete at an absent time: NoOpNotFound, no undo");

    t = base;
    ApplySetTrackKey(t, missSet);
    ApplyDeleteTrackKey(t, missDel);
    CHECK(sameKeys(keysOf(t), keysOf(base)), "Apply* of a non-mutating plan leaves the track alone");

    // NaN would match the first key in the multiset; refuse it outright.
    CHECK(PlanDeleteTrackKey(base, kNaN).outcome == EditOutcome::Rejected, "delete at NaN: Rejected (would erase a border)");
    CHECK(PlanSetTrackKey(base, kNaN, 10, 1, false).outcome == EditOutcome::Rejected
          && PlanSetTrackKey(base, 50, kNaN, 1, false).outcome == EditOutcome::Rejected
          && PlanSetTrackKey(base, 50, 10, kNaN, false).outcome == EditOutcome::Rejected,
          "set with any NaN argument: Rejected");
}

static void testDedup()
{
    std::printf("(b) dedup by epsilon\n");
    Track t = makeTrack({Key(0, 0), Key(50, 0.5f), Key(100, 1)});
    CHECK(DedupedInsertTime(t, 25) == 25, "free time is kept");
    CHECK(DedupedInsertTime(t, 50) == 50 + 0.001f, "occupied time bumps once by 0.001");

    // A run of occupied times at the exact accumulated bump values.
    std::vector<float> run;
    float x = 40;
    for (int i = 0; i <= 1000; i++) { run.push_back(x); x += 0.001f; }   // run[i] = 40 + i bumps

    Track three = makeTrack({Key(0, 0), Key(100, 1)});
    for (int i = 0; i < 3; i++) three.keys.insert(Key(run[i], 0));
    CHECK(DedupedInsertTime(three, 40) == run[3], "a run of 3 collisions bumps 3 times");

    // 1000 occupied: the 1000th bump lands on a free time.
    Track under = makeTrack({Key(0, 0), Key(100, 1)});
    for (int i = 0; i < 1000; i++) under.keys.insert(Key(run[i], 0));
    const float u = DedupedInsertTime(under, 40);
    CHECK(u == run[1000] && under.keys.find(Key(u, 0)) == under.keys.end(), "1000 collisions: the 1000th bump is free");

    // 1001 occupied: the cap stops on an occupied time; the insert is a true duplicate.
    Track over = under;
    over.keys.insert(Key(run[1000], 0));
    const float o = DedupedInsertTime(over, 40);
    CHECK(o == run[1000] && over.keys.find(Key(o, 0)) != over.keys.end(), "1001 collisions: cap returns an occupied time (duplicate)");
}

static void testLock()
{
    std::printf("(b) track lock\n");
    // 4x4 matrix from the all-unlocked state: accepted iff target < channel.
    bool matrixOk = true;
    for (int ch = 0; ch < 4; ch++)
    for (int tgt = 0; tgt < 4; tgt++)
    {
        Emitter e;
        for (int i = 0; i < 4; i++) e.tracks[i] = &e.trackContents[i];
        TrackLockPlan p = PlanSetTrackLock(e, ch, tgt, false);
        const bool accepted = p.outcome == EditOutcome::Applied;
        matrixOk = matrixOk && accepted == (tgt < ch);
        if (!accepted) matrixOk = matrixOk && p.outcome == EditOutcome::Rejected
                                          && p.reason == "invalid lockTo channel (must be an earlier RGBA channel)";
    }
    CHECK(matrixOk, "4x4 matrix: only an earlier channel is accepted; later/self rejected with the wire reason");

    Emitter e;   // default: G/B/A locked to R
    CHECK(PlanSetTrackLock(e, 2, 0, false).outcome == EditOutcome::NoOpUnchanged, "lock already in place: NoOpUnchanged");
    TrackLockPlan unlock = PlanSetTrackLock(e, 2, -1, true);
    ApplySetTrackLock(e, unlock);
    CHECK(unlock.outcome == EditOutcome::Applied && e.tracks[2] == &e.trackContents[2], "unlock restores the channel's own keys");
    CHECK(PlanSetTrackLock(e, 0, -1, true).outcome == EditOutcome::NoOpUnchanged, "unlocking an unlocked channel: NoOpUnchanged");
    CHECK(PlanSetTrackLock(e, 3, -1, false).outcome == EditOutcome::Rejected, "unresolved lockTo with unlock=false: Rejected");
    CHECK(PlanSetTrackLock(e, 3, ParticleSystem::TRACK_SCALE, false).outcome == EditOutcome::Rejected, "non-RGBA lock target: Rejected");
    const TrackLockPlan scale = PlanSetTrackLock(e, ParticleSystem::TRACK_SCALE, 0, false);
    CHECK(scale.outcome == EditOutcome::NoOpOutOfDomain && !scale.willMutate, "lock on a non-RGBA track: NoOpOutOfDomain (distinct from unchanged)");
    CHECK(PlanSetTrackLock(e, -1, 0, false).reason == "unknown channel", "unknown channel: Rejected");
}

static void testInterpolationAndScalars()
{
    std::printf("(b) interpolation and scalar fields\n");
    CHECK(IsValidInterpolation("linear") && IsValidInterpolation("smooth") && IsValidInterpolation("step"),
          "linear/smooth/step accepted");
    CHECK(!IsValidInterpolation("Linear") && !IsValidInterpolation("") && !IsValidInterpolation("cubic"),
          "anything else rejected (case-sensitive)");

    CHECK(IsFiniteFloatField(0) && IsFiniteFloatField(FLT_MAX) && IsFiniteFloatField(-FLT_MAX), "finite float fields accepted");
    CHECK(!IsFiniteFloatField(std::nan("")) && !IsFiniteFloatField(HUGE_VAL) && !IsFiniteFloatField(-HUGE_VAL)
          && !IsFiniteFloatField(1e39) && !IsFiniteFloatField(-1e39),
          "NaN, +-inf and |x| > FLT_MAX rejected");
    CHECK(InIntFieldRange(INT_MAX) && InIntFieldRange(INT_MIN) && InIntFieldRange(0), "int range accepted at both ends");
    CHECK(!InIntFieldRange((double)INT_MAX + 1) && !InIntFieldRange((double)INT_MIN - 1) && !InIntFieldRange(std::nan("")),
          "out-of-range and NaN int rejected");

    CHECK(ValidateAddTrackKey({true, 10, 0.5f}, false).outcome == EditOutcome::Applied, "add-track-key: finite numeric key accepted");
    CHECK(ValidateAddTrackKey({false, 0, 0}, false).reason == "add-track-key requires numeric 'time' and 'value'",
          "add-track-key: non-numeric -> its wire reason");
    CHECK(ValidateAddTrackKey({true, kInf, 0}, false).reason == "add-track-key 'time'/'value' must be finite"
          && ValidateAddTrackKey({true, 0, -kInf}, false).outcome == EditOutcome::Rejected,
          "add-track-key: non-finite -> its wire reason");

    // Batch: the FIRST bad key decides, whichever kind it is.
    CHECK(ValidateBatchKeys({}, false).outcome == EditOutcome::Applied, "add-track-keys: empty batch is legal");
    CHECK(ValidateBatchKeys({{true, kInf, 0}, {false, 0, 0}}, false).reason == "add-track-keys 'time'/'value' must be finite",
          "add-track-keys: non-finite before non-numeric -> finite reason");
    CHECK(ValidateBatchKeys({{true, 1, 0}, {false, 0, 0}, {true, kInf, 0}}, false).reason
          == "add-track-keys requires numeric 'time' and 'value' on every key",
          "add-track-keys: non-numeric before non-finite -> numeric reason");

    Emitter e;
    CHECK(!HasLinkGroupMembership(e), "linkGroup 0: not linked");
    e.linkGroup = 7;
    CHECK(HasLinkGroupMembership(e), "linkGroup nonzero: linked");
}

// ===========================================================================
// (c) New rules
// ===========================================================================

typedef std::vector<unsigned char> Bytes;

static Bytes serialize(ParticleSystem& ps)
{
    MemoryFile* f = new MemoryFile();
    ps.write(f);
    f->seek(0);
    Bytes out(f->size());
    if (!out.empty()) f->read(out.data(), (unsigned long)out.size());
    f->Release();
    return out;
}

// Reloaded system, or nullptr if the load threw.
static ParticleSystem* reload(const Bytes& image)
{
    MemoryFile* f = new MemoryFile();
    if (!image.empty()) f->write(image.data(), (unsigned long)image.size());
    f->seek(0);
    ParticleSystem* ps = nullptr;
    try { ps = new ParticleSystem(f); }
    catch (...) { ps = nullptr; }
    f->Release();
    return ps;
}

static void newSystem(ParticleSystem& ps)
{
    ps.setName("AuthoringTest");
    Emitter* e = ps.addRootEmitter();
    e->name = "Em0";
    for (int t = 0; t < ParticleSystem::NUM_TRACKS; ++t) e->tracks[t] = &e->trackContents[t];
}

static bool rejected(const Emitter& e, int track, std::vector<Key> keys)
{
    return ValidateFullTrackReplacement(e, track, keys).outcome == EditOutcome::Rejected;
}

static void testFullTrackReplacement()
{
    std::printf("(c) full-track replacement\n");
    ParticleSystem ps;
    newSystem(ps);
    Emitter& e = ps.getEmitter(0);
    const int SCALE = ParticleSystem::TRACK_SCALE, RED = ParticleSystem::TRACK_RED_CHANNEL;

    // Exactly representable times (t/100*100 round-trips) and values.
    const std::vector<Key> scaleKeys = {Key(0, 2), Key(12.5f, 8), Key(50, 8), Key(50, 4), Key(100, -1)};
    CHECK(ValidateFullTrackReplacement(e, SCALE, scaleKeys).outcome == EditOutcome::Applied, "valid scale replacement accepted");
    ApplyFullTrackReplacement(e, SCALE, scaleKeys);
    CHECK(sameKeys(keysOf(*e.tracks[SCALE]), scaleKeys), "apply keeps the proposed order, same-time keys included");

    const std::vector<Key> redKeys = {Key(0, 0), Key(25, 1), Key(100, 1)};
    CHECK(ValidateFullTrackReplacement(e, RED, redKeys).outcome == EditOutcome::Applied, "valid color replacement accepted");
    ApplyFullTrackReplacement(e, RED, redKeys);

    ParticleSystem* rp = reload(serialize(ps));
    CHECK(rp != nullptr, "replaced tracks save and reload");
    if (rp)
    {
        const Emitter& r = rp->getEmitter(0);
        CHECK(sameKeys(keysOf(*r.tracks[SCALE]), scaleKeys), "scale keys survive save + reload exactly");
        CHECK(sameKeys(keysOf(*r.tracks[RED]), redKeys), "color keys survive save + reload exactly");
        delete rp;
    }

    CHECK(rejected(e, SCALE, {Key(0, 1)}), "fewer than 2 keys rejected");
    CHECK(rejected(e, SCALE, {Key(5, 1), Key(100, 1)}), "first key not at 0 rejected");
    CHECK(rejected(e, SCALE, {Key(0, 1), Key(99, 1)}), "last key not at 100 rejected");
    CHECK(rejected(e, SCALE, {Key(0, 1), Key(60, 1), Key(40, 1), Key(100, 1)}), "decreasing times rejected");
    CHECK(rejected(e, SCALE, {Key(0, 1), Key(150, 1), Key(100, 1)}), "a time past 100 rejected");
    CHECK(rejected(e, SCALE, {Key(0, 1), Key(50, kNaN), Key(100, 1)})
          && rejected(e, SCALE, {Key(0, 1), Key(kInf, 1), Key(100, 1)}), "non-finite time or value rejected");
    CHECK(rejected(e, RED, {Key(0, 0), Key(50, 1.5f), Key(100, 1)})
          && rejected(e, RED, {Key(0, -0.1f), Key(100, 1)}), "color value outside [0, 1] rejected");
    CHECK(!rejected(e, SCALE, {Key(0, 0), Key(50, 1.5f), Key(100, -3)}), "non-color tracks take any finite value");
    CHECK(rejected(e, -1, {Key(0, 0), Key(100, 1)}) && rejected(e, ParticleSystem::NUM_TRACKS, {Key(0, 0), Key(100, 1)}),
          "unknown track rejected");

    std::vector<Key> huge(kMaxAloTrackKeys + 2, Key(50, 1));
    huge.front() = Key(0, 1);
    huge.back()  = Key(100, 1);
    CHECK(rejected(e, SCALE, huge), "kMaxAloTrackKeys intermediates rejected");
    huge.pop_back(); huge.back() = Key(100, 1);
    CHECK(!rejected(e, SCALE, huge), "kMaxAloTrackKeys - 1 intermediates accepted");

    Emitter locked;   // default: G/B/A locked to R
    CHECK(rejected(locked, ParticleSystem::TRACK_GREEN_CHANNEL, {Key(0, 0), Key(100, 1)}),
          "locked color channel rejected (would rewrite the channel it is locked to)");
    locked.randomRotation = true;
    CHECK(rejected(locked, ParticleSystem::TRACK_ROTATION_SPEED, {Key(0, 0), Key(100, 1)}),
          "rotation track rejected while randomRotation is on");

    // Why the rules exist: write the illegal shapes directly and reload.
    {
        ParticleSystem bad;
        newSystem(bad);
        bad.getEmitter(0).tracks[RED]->keys = makeTrack({Key(0, 0), Key(50, 1.5f), Key(100, 1)}).keys;
        ParticleSystem* r = reload(serialize(bad));
        CHECK(r == nullptr, "grounding: a color value > 1 makes the saved file unloadable");
        delete r;
    }
    {
        ParticleSystem bad;
        newSystem(bad);
        bad.getEmitter(0).tracks[SCALE]->keys = makeTrack({Key(5, 1), Key(100, 1)}).keys;
        ParticleSystem* r = reload(serialize(bad));
        CHECK(r != nullptr && r->getEmitter(0).tracks[SCALE]->keys.begin()->time == 0,
              "grounding: a first key off 0 silently reloads at 0");
        delete r;
    }
}

static void testKeyBounds()
{
    std::printf("(c) track-key bounds\n");
    const std::string addTime = "add-track-key 'time' must be within [0, 100]";
    const std::string addColor = "add-track-key 'value' must be within [0, 1] on a color track";
    CHECK(ValidateAddTrackKey({true, 0, 0.5f}, true).outcome == EditOutcome::Applied
          && ValidateAddTrackKey({true, 100, 1}, true).outcome == EditOutcome::Applied
          && ValidateAddTrackKey({true, -0.0f, 0}, true).outcome == EditOutcome::Applied,
          "add: times 0, 100 and -0.0 accepted; color values 0 and 1 accepted");
    CHECK(ValidateAddTrackKey({true, -0.1f, 0.5f}, false).reason == addTime
          && ValidateAddTrackKey({true, 100.001f, 0.5f}, false).reason == addTime
          && ValidateAddTrackKey({true, 150, 0.5f}, false).reason == addTime,
          "add: time below 0 or above 100 rejected with its reason");
    CHECK(ValidateAddTrackKey({true, 50, 1.5f}, true).reason == addColor
          && ValidateAddTrackKey({true, 50, -0.1f}, true).reason == addColor,
          "add: color value outside [0, 1] rejected on a color track");
    CHECK(ValidateAddTrackKey({true, 50, 20}, false).outcome == EditOutcome::Applied
          && ValidateAddTrackKey({true, 50, -5}, false).outcome == EditOutcome::Applied,
          "add: non-color tracks take any finite value");
    CHECK(ValidateBatchKeys({{true, 10, 0.5f}, {true, 150, 0.5f}, {true, 20, 2}}, true).reason
          == "add-track-keys 'time' must be within [0, 100] on every key",
          "batch: first bad key decides (time before a later bad color value)");
    CHECK(ValidateBatchKeys({{true, 10, 0.5f}, {true, 20, 2}, {true, 150, 0.5f}}, true).reason
          == "add-track-keys 'value' must be within [0, 1] on a color track",
          "batch: first bad key decides (color value before a later bad time)");
    CHECK(ValidateBatchKeys({{true, 10, 20}, {true, 90, 15}}, false).outcome == EditOutcome::Applied,
          "batch: a scale paste is legal on a non-color track");

    const Track base = makeTrack({Key(0, 0.2f), Key(50, 0.5f), Key(100, 1)});
    CHECK(PlanSetTrackKey(base, 50, 150, 0.5f, false).reason == "set-track-key 'newTime' must be within [0, 100]"
          && PlanSetTrackKey(base, 50, -1, 0.5f, false).outcome == EditOutcome::Rejected
          && PlanSetTrackKey(base, 50, kInf, 0.5f, false).outcome == EditOutcome::Rejected,
          "set: a non-border key's newTime outside [0, 100] (or infinite) rejected");
    const TrackKeyPlan border = PlanSetTrackKey(base, 100, 150, 0.7f, false);
    CHECK(border.outcome == EditOutcome::AppliedBorderTime && border.replacement.time == 100,
          "set: a border key's newTime is ignored, so an out-of-range one is not refused");
    CHECK(PlanSetTrackKey(base, 50, 40, kInf, false).reason == "set-track-key 'newValue' must be finite",
          "set: non-finite newValue rejected on any track");
    CHECK(PlanSetTrackKey(base, 50, 40, 2, true).reason == "set-track-key 'newValue' must be within [0, 1] on a color track"
          && PlanSetTrackKey(base, 50, 40, 2, false).outcome == EditOutcome::Applied,
          "set: color value outside [0, 1] rejected on a color track only");
    CHECK(PlanSetTrackKey(base, 33, 150, 0.5f, false).outcome == EditOutcome::Rejected,
          "set: a bad request fails loud even when no key sits at oldTime");

    const float below100 = 100 - 0.001f;
    CHECK(DedupedInsertTime(base, 100) == below100, "dedup: an add at an occupied 100 searches downward");
    Track crowded = base;
    crowded.keys.insert(Key(below100, 0));
    CHECK(DedupedInsertTime(crowded, 100) == below100 - 0.001f, "dedup: downward search skips occupied times too");
    CHECK(DedupedInsertTime(base, 50) == 50 + 0.001f && DedupedInsertTime(base, 0) == 0 + 0.001f,
          "dedup: everywhere else it bumps upward exactly as before");

    // Why the rules exist: write the shapes the old rules let through, and reload.
    const int SCALE = ParticleSystem::TRACK_SCALE;
    {
        ParticleSystem bad;
        newSystem(bad);
        bad.getEmitter(0).tracks[SCALE]->keys = makeTrack({Key(0, 20), Key(50, 10), Key(100, 5), Key(150, 7), Key(160, 8)}).keys;
        ParticleSystem* r = reload(serialize(bad));
        CHECK(r == nullptr, "grounding: two keys past 100 make the saved file unloadable");
        delete r;
    }
    {
        ParticleSystem bad;
        newSystem(bad);
        bad.getEmitter(0).tracks[SCALE]->keys = makeTrack({Key(0, 20), Key(50, 10), Key(100, 5), Key(100 + 0.001f, 7)}).keys;
        ParticleSystem* r = reload(serialize(bad));
        const Track* t = r ? r->getEmitter(0).tracks[SCALE] : nullptr;
        CHECK(t != nullptr && t->keys.count(Key(100, 0)) == 2,
              "grounding: the old edge bump (100.001) reloads as a second key at 100");
        delete r;
    }
}

static void testAtlas()
{
    std::printf("(c) atlas grid and index\n");
    // The renderer's formula before it moved to AtlasGridSide (EmitterInstance.cpp).
    auto engineSide = [](unsigned long ts) { return (int)floor(sqrtf((float)(ts > 1ul ? ts : 1ul))); };
    bool same = true;
    for (unsigned long ts = 0; ts <= 5000; ts++) same = same && AtlasGridSide(ts) == engineSide(ts);
    const unsigned long big[] = {65535ul, 65536ul, 1000000ul, 4294967295ul};
    for (unsigned long ts : big) same = same && AtlasGridSide(ts) == engineSide(ts);
    CHECK(same, "AtlasGridSide matches the renderer's floor(sqrt(max(1, textureSize)))");
    CHECK(AtlasGridSide(64) == 8 && AtlasGridSide(16) == 4 && AtlasGridSide(20) == 4 && AtlasGridSide(0) == 1,
          "textureSize is a frame count: 64 -> 8x8, 16 and 20 -> 4x4, 0 -> 1x1");

    Emitter e;
    e.textureSize = 20;   // 4x4 grid: 16 cells, NOT 20
    CHECK(ClassifyAtlasIndex(e, 0).outcome == EditOutcome::Applied && ClassifyAtlasIndex(e, 15.9f).outcome == EditOutcome::Applied,
          "indices inside the grid pass");
    const EditResult out = ClassifyAtlasIndex(e, 16);
    CHECK(out.outcome == EditOutcome::OutOfGrid && !out.reason.empty(), "index 16 of textureSize 20 is out of the 4x4 grid (bound is side^2)");
    CHECK(ClassifyAtlasIndex(e, -0.5f).outcome == EditOutcome::OutOfGrid && ClassifyAtlasIndex(e, kNaN).outcome == EditOutcome::OutOfGrid,
          "negative and non-finite indices are out of grid");
    bool neverRejected = true;
    const float probes[] = {-100, -1, 0, 3, 16, 64, 1e9f, kInf, kNaN};
    for (float v : probes) neverRejected = neverRejected && ClassifyAtlasIndex(e, v).outcome != EditOutcome::Rejected;
    CHECK(neverRejected, "the classifier never rejects (the engine wraps out-of-grid indices)");
}

static PropertyValue num(double v)  { PropertyValue p = {false, false, v}; return p; }
static PropertyValue flag(bool b)   { PropertyValue p = {true, b, 0.0}; return p; }

static bool isRejected(const EditResult& r) { return r.outcome == EditOutcome::Rejected; }

static bool samePropertyValue(const PropertyValue& a, const PropertyValue& b)
{
    if (a.isBool != b.isBool) return false;
    if (a.isBool) return a.boolValue == b.boolValue;
    // Bit-exact, so -0.0 and +0.0 differ.
    return std::memcmp(&a.number, &b.number, sizeof(double)) == 0;
}

static bool hasName(const std::vector<std::string>& names, const std::string& name)
{
    for (const std::string& n : names) if (n == name) return true;
    return false;
}

// Set `field` to `value` on a fresh system, save, reload, and read it back.
static bool survivesSave(const std::string& field, const PropertyValue& value, std::string* got)
{
    ParticleSystem ps;
    newSystem(ps);
    Emitter& e = ps.getEmitter(0);
    // randomRotationAverage is saved only while randomRotation is on.
    if (field == "randomRotationAverage") e.randomRotation = true;
    ApplySetProperty(e, field, value);
    const PropertyValue applied = GetProperty(e, field);   // floats narrowed
    ParticleSystem* rp = reload(serialize(ps));
    if (!rp) { *got = "unloadable"; return false; }
    const PropertyValue back = GetProperty(rp->getEmitter(0), field);
    char buf[64];
    std::snprintf(buf, sizeof(buf), back.isBool ? (back.boolValue ? "true" : "false") : "%.9g", back.number);
    *got = buf;
    delete rp;
    return samePropertyValue(back, applied);
}

static void testSetProperty()
{
    std::printf("(d) tool-only: set-property\n");
    const std::vector<std::string> names = SettablePropertyNames();
    bool unique = true;
    for (size_t i = 0; i < names.size(); i++)
        for (size_t j = i + 1; j < names.size(); j++) unique = unique && names[i] != names[j];
    CHECK(unique, "settable names are unique");
    CHECK(hasName(names, "blendMode") && hasName(names, "acceleration[2]") && hasName(names, "randomColors[3]") &&
          hasName(names, "groups[0].type") && hasName(names, "groups[2].valZ") && hasName(names, "unknown49"),
          "settable names cover scalars, array elements and the speed/position groups");
    const char* excluded[] = {"index", "linkGroup", "spawnOnDeath", "spawnDuringLife", "unknown11", "has11",
                              "groups[1].type", "groups[1].minX", "name", "colorTexture", "normalTexture", "visible",
                              "stableId", "parent", "acceleration[3]", "groups[3].type"};
    bool none = true;
    for (const char* n : excluded) none = none && !hasName(names, n) && isRejected(ValidateSetProperty(n, num(0)));
    CHECK(none, "identity, link, spawn, derived and verb-owned fields are not settable");

    // Every settable field: legal values at its edges survive a save exactly.
    int roundTrips = 0;
    std::string firstMiss;
    for (const std::string& field : names)
    {
        std::vector<PropertyValue> values;
        const PropertyValue current = GetProperty(Emitter(), field);
        if (current.isBool) values = {flag(true), flag(false)};
        else
        {
            const double probes[] = {0, 1, 3, 13, 255, 4294967294.0, 4294967295.0, -2.5, 0.125, 1e30, -1e-30, -FLT_MAX};
            for (double v : probes)
                if (ValidateSetProperty(field, num(v)).outcome == EditOutcome::Applied) values.push_back(num(v));
        }
        for (const PropertyValue& v : values)
        {
            std::string got;
            if (survivesSave(field, v, &got)) { roundTrips++; continue; }
            if (firstMiss.empty())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s = %.9g reloads as %s", field.c_str(), v.isBool ? (double)v.boolValue : v.number, got.c_str());
                firstMiss = buf;
            }
        }
    }
    std::printf("  (%d accepted values round-tripped)\n", roundTrips);
    CHECK(firstMiss.empty(), firstMiss.empty() ? "every accepted value survives save + reload" : firstMiss.c_str());
    CHECK(roundTrips > 300, "the round-trip battery ran on every field");

    CHECK(isRejected(ValidateSetProperty("noSuchField", num(1))), "unknown field rejected");
    CHECK(isRejected(ValidateSetProperty("useBursts", num(1))) && isRejected(ValidateSetProperty("lifetime", flag(true))),
          "a number for a boolean, or a boolean for a number, rejected");
    CHECK(isRejected(ValidateSetProperty("nParticlesPerSecond", num(1.5))) &&
          isRejected(ValidateSetProperty("nParticlesPerSecond", num(-1))) &&
          isRejected(ValidateSetProperty("nParticlesPerSecond", num(4294967296.0))) &&
          isRejected(ValidateSetProperty("nParticlesPerSecond", num(NAN))),
          "integers: fractional, negative, above 0xFFFFFFFF and NaN rejected");
    CHECK(isRejected(ValidateSetProperty("blendMode", num(14))) && isRejected(ValidateSetProperty("nTriangles", num(0))) &&
          isRejected(ValidateSetProperty("nBursts", num(4294967295.0))) && isRejected(ValidateSetProperty("groundBehavior", num(4))) &&
          isRejected(ValidateSetProperty("emitFromMesh", num(4))) && isRejected(ValidateSetProperty("emitFromMesh", num(-1))) &&
          isRejected(ValidateSetProperty("groups[0].type", num(5))) && isRejected(ValidateSetProperty("groups[2].type", num(5))),
          "integer ranges: blendMode 14, nTriangles 0, nBursts 0xFFFFFFFF, groundBehavior 4, emitFromMesh -1/4, group type 5");
    CHECK(isRejected(ValidateSetProperty("gravity", num(1e39))) && isRejected(ValidateSetProperty("gravity", num(INFINITY))) &&
          isRejected(ValidateSetProperty("gravity", num(NAN))),
          "floats: beyond float range and non-finite rejected");
    CHECK(isRejected(ValidateSetProperty("randomRotationVariance", num(1.0001))) &&
          isRejected(ValidateSetProperty("randomRotationVariance", num(-0.1))) &&
          ValidateSetProperty("randomRotationVariance", num(1)).outcome == EditOutcome::Applied,
          "randomRotationVariance stays within [0, 1]");
    CHECK(!ValidateSetProperty("blendMode", num(1.5)).reason.empty(), "a rejection says why");

    // Why the integer ranges exist: write the value directly and reload.
    {
        ParticleSystem ps; newSystem(ps);
        ps.getEmitter(0).blendMode = 14; ps.getEmitter(0).nTriangles = 0; ps.getEmitter(0).nBursts = 0xFFFFFFFFul;
        ParticleSystem* r = reload(serialize(ps));
        CHECK(r && r->getEmitter(0).blendMode == 0 && r->getEmitter(0).nTriangles == 1 && r->getEmitter(0).nBursts == 0,
              "grounding: blendMode 14, nTriangles 0 and nBursts 0xFFFFFFFF reload as 0, 1 and 0");
        delete r;
    }
    {
        ParticleSystem ps; newSystem(ps);
        ps.getEmitter(0).groups[ParticleSystem::GROUP_POSITION].type = 5;
        ParticleSystem* r = reload(serialize(ps));
        CHECK(r == nullptr, "grounding: a group type of 5 makes the saved file unloadable");
        delete r;
    }

    CHECK(PropertyFootprint("acceleration[0]") == std::vector<std::string>({"acceleration[0]"}), "a plain field's footprint is itself");
    CHECK(PropertyFootprint("lifetime") == std::vector<std::string>({"lifetime", "groups[1]"}) &&
          PropertyFootprint("randomLifetimePerc") == std::vector<std::string>({"randomLifetimePerc", "groups[1]"}),
          "lifetime and randomLifetimePerc also derive groups[1]");
    CHECK(PropertyFootprint("randomRotation") == std::vector<std::string>({"randomRotation", "randomRotationAverage", "tracks[6]"}) &&
          PropertyFootprint("randomRotationAverage") == std::vector<std::string>({"randomRotationAverage", "tracks[6]"}),
          "randomRotation and its average reach the rotation track");

    Emitter e;
    ApplySetProperty(e, "groups[2].sphereEdge", num(7));
    ApplySetProperty(e, "randomColors[1]", num(0.5));
    ApplySetProperty(e, "isHeatParticle", flag(true));
    CHECK(e.groups[ParticleSystem::GROUP_POSITION].sphereEdge == 7 && e.randomColors[1] == 0.5f && e.isHeatParticle,
          "apply writes the named member");
}

static void testRenameAndTextures()
{
    std::printf("(d) tool-only: rename and texture names\n");
    ParticleSystem ps;
    newSystem(ps);
    Emitter* second = ps.addRootEmitter();
    second->name = "Em1";
    const Emitter& first = ps.getEmitter(0);
    CHECK(ValidateRename(ps, first, "Em0").outcome == EditOutcome::NoOpUnchanged, "renaming to the current name is unchanged");
    CHECK(ValidateRename(ps, first, "Fresh").outcome == EditOutcome::Applied, "a free name is accepted");
    const EditResult taken = ValidateRename(ps, first, "Em1");
    CHECK(isRejected(taken) && taken.reason.find("Em1_1") != std::string::npos, "a taken name is rejected with a free suggestion");
    CHECK(isRejected(ValidateRename(ps, first, "")) && isRejected(ValidateRename(ps, first, std::string("a\0b", 3))) &&
          isRejected(ValidateRename(ps, first, "\xC3\x28")),
          "empty, NUL-holding and non-UTF-8 names rejected");
    CHECK(ValidateRename(ps, first, "\xC3\xA9tincelle").outcome == EditOutcome::Applied, "a UTF-8 name is accepted");

    CHECK(isRejected(ValidateTextureName("", true)) && ValidateTextureName("", false).outcome == EditOutcome::Applied,
          "an empty colour texture is rejected, an empty normal texture is legal");
    CHECK(ValidateTextureName(std::string(kMaxTextureNameBytes, 'a'), true).outcome == EditOutcome::Applied &&
          isRejected(ValidateTextureName(std::string(kMaxTextureNameBytes + 1, 'a'), true)),
          "texture names are capped at kMaxTextureNameBytes");
    CHECK(isRejected(ValidateTextureName(std::string("a\0.tga", 6), true)) && isRejected(ValidateTextureName("\xFF.tga", false)),
          "NUL-holding and non-UTF-8 texture names rejected");
}

static void testRescaleRule()
{
    std::printf("(d) tool-only: rescale preconditions\n");
    Emitter e;
    e.useBursts = false;
    e.nParticlesPerSecond = 10;
    CHECK(ValidateRescale(e, 2, 0.5).outcome == EditOutcome::Applied, "ordinary scales accepted");
    const double bad[] = {0, -1, NAN, INFINITY, 1e-40, 1e39};
    bool all = true;
    for (double v : bad) all = all && isRejected(ValidateRescale(e, v, 1)) && isRejected(ValidateRescale(e, 1, v));
    CHECK(all, "zero, negative, non-finite, subnormal and beyond-float scales rejected");

    CHECK(isRejected(ValidateRescale(e, 1e-9, 1)), "a particle rate pushed past INT_MAX rejected");
    e.nParticlesPerSecond = 1;
    CHECK(isRejected(ValidateRescale(e, FLT_MAX, 1)), "a fractional rate whose burst delay overflows rejected");
    CHECK(ValidateRescale(e, 3, 1).outcome == EditOutcome::Applied, "a fractional rate with a finite burst delay accepted");

    e.useBursts = true;
    e.burstDelay = 0;
    CHECK(isRejected(ValidateRescale(e, 2, 1)), "a zero burst delay rejected (the conversion divides by it)");
    e.burstDelay = 1e-6f;
    CHECK(isRejected(ValidateRescale(e, 1e-3, 1)), "a burst rate pushed past INT_MAX rejected");
    CHECK(ValidateRescale(e, 1e-3, 1).reason.find("burst") != std::string::npos, "the reason names the burst delay");
    e.burstDelay = -1e-6f;
    CHECK(isRejected(ValidateRescale(e, 1e-3, 1)), "a negative burst rate pushed past INT_MIN rejected");
    e.burstDelay = -0.5f;
    CHECK(ValidateRescale(e, 2, 1).outcome == EditOutcome::Applied, "a negative burst delay that converts is accepted (no overflow)");
    e.burstDelay = 0.25f;
    CHECK(ValidateRescale(e, 2, 1).outcome == EditOutcome::Applied, "an ordinary burst delay accepted");
    e.burstDelay = 0;
    CHECK(ValidateRescale(e, 1, 2).outcome == EditOutcome::Applied, "a time scale of 1 skips the conversion");
    e.isWeatherParticle = true;
    CHECK(ValidateRescale(e, 2, 1).outcome == EditOutcome::Applied, "weather emitters skip the conversion");
}

static void testKeyCountAndLocks()
{
    std::printf("(d) tool-only: key count and locks\n");
    Track t = makeTrack({Key(0, 1), Key(100, 1)});
    CHECK(ValidateKeyCount(t, kMaxAloTrackKeys - 1).outcome == EditOutcome::Applied, "kMaxAloTrackKeys - 1 interior keys accepted");
    CHECK(isRejected(ValidateKeyCount(t, kMaxAloTrackKeys)), "kMaxAloTrackKeys interior keys rejected");

    Emitter locked;   // default: G/B/A locked to R
    CHECK(LockedDependents(locked, ParticleSystem::TRACK_RED_CHANNEL) == std::vector<int>({1, 2, 3}), "red drives green, blue and alpha");
    CHECK(LockedDependents(locked, ParticleSystem::TRACK_GREEN_CHANNEL).empty() && LockedDependents(locked, 4).empty(),
          "a locked channel and a non-colour track drive nothing");
    const EditResult onLocked = ValidateTrackEditLocks(locked, ParticleSystem::TRACK_BLUE_CHANNEL, true);
    CHECK(isRejected(onLocked) && onLocked.reason.find("tracks[0]") != std::string::npos,
          "editing a locked channel is rejected and names its source");
    CHECK(isRejected(ValidateTrackEditLocks(locked, ParticleSystem::TRACK_RED_CHANNEL, false)),
          "editing a source with dependents is rejected unless they are allowed");
    CHECK(ValidateTrackEditLocks(locked, ParticleSystem::TRACK_RED_CHANNEL, true).outcome == EditOutcome::Applied,
          "editing a source is accepted when its dependents are allowed");
    CHECK(ValidateTrackEditLocks(locked, ParticleSystem::TRACK_SCALE, false).outcome == EditOutcome::Applied,
          "non-colour tracks are never locked");
    CHECK(isRejected(ValidateTrackEditLocks(locked, -1, true)) && isRejected(ValidateTrackEditLocks(locked, ParticleSystem::NUM_TRACKS, true)),
          "unknown track rejected");

    ParticleSystem ps;
    newSystem(ps);   // every channel unlocked
    CHECK(ValidateTrackEditLocks(ps.getEmitter(0), ParticleSystem::TRACK_ALPHA_CHANNEL, false).outcome == EditOutcome::Applied &&
          LockedDependents(ps.getEmitter(0), ParticleSystem::TRACK_RED_CHANNEL).empty(),
          "unlocked channels edit freely");
}

int main()
{
    std::printf("test_particle_authoring\n");
    testParity();
    testBorderAndNotFound();
    testDedup();
    testLock();
    testInterpolationAndScalars();
    testFullTrackReplacement();
    testKeyBounds();
    testAtlas();
    testSetProperty();
    testRenameAndTextures();
    testRescaleRule();
    testKeyCountAndLocks();
    if (g_failed) { std::printf("FAILED: %d check(s)\n", g_failed); return 1; }
    std::printf("all checks passed\n");
    return 0;
}
