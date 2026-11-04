#include "simulation/ObservationLifecycle.h"
#include "host/ObservationSampleJson.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

static int g_failed = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL: %s\n", msg);                                                                          \
        }                                                                                                              \
    } while (0)

static ObservationSpec ValidSpec()
{
    ObservationSpec spec;
    spec.scenario.aloPath          = L"fixture.alo";
    spec.scenario.maxFrames        = 10;
    spec.lifecycle.quietTailFrames = 3;
    spec.output.strideInterval     = 3;
    return spec;
}

static ObservationSample Quiet(int frame)
{
    ObservationSample sample{};
    sample.frame       = frame;
    sample.spawnerIdle = true;
    return sample;
}

static bool HasReason(const std::vector<SelectedFrame>& selected, int frame, const char* reason)
{
    for (const SelectedFrame& item : selected)
        if (item.frame == frame)
            return std::find(item.reasons.begin(), item.reasons.end(), reason) != item.reasons.end();
    return false;
}

static std::vector<int> Frames(const std::vector<SelectedFrame>& selected, const char* reason = nullptr)
{
    std::vector<int> frames;
    for (const SelectedFrame& item : selected)
        if (reason == nullptr || HasReason(selected, item.frame, reason)) frames.push_back(item.frame);
    return frames;
}

int main()
{
    std::printf("test_observation_lifecycle\n");
    using Status                   = ObservationStatus;
    using Type                     = ObservationSpec::Lifecycle::Type;
    const ObservationSpec defaults = ValidSpec();
    CHECK(ObservationPreflightBytes(10000, 2, 5632) == 1084208,
          "preflight includes one and a half measured PNG bytes, the telemetry bytes and the reserve");
    CHECK(ObservationPreflightBytes(3, 2, 512) == 1049098,
          "fractional per-PNG bytes round upward before multiplying the count");
    CHECK(ObservationPreflightBytes(10000000, 0, 512) == 1049088, "no PNGs still reserve telemetry and metadata bytes");
    CHECK(ObservationPreflightBytes(64ull * 1024 * 1024, 3665, 3601ull * 512) == 368933872128ull,
          "3600-frame large-PNG playback preflight keeps exact 64-bit arithmetic");
    {
        // A 65,536-emitter effect: every line carries 65,536 counts, all zero here.
        ObservationSample narrow = Quiet(0);
        ObservationSample wide   = Quiet(0);
        wide.perEmitter.assign(65536, 0);
        CHECK(ObservationSampleLine(wide).size() == ObservationSampleLine(narrow).size() + 2 * 65536 - 1,
              "a line's per-emitter array costs one digit and one comma per emitter");
        const std::uint64_t budget       = 4ull * 1024 * 1024;
        auto                refusalFrame = [&](bool counts) -> int
        {
            TelemetryByteBudget accounted(budget);
            for (int frame = 0; frame < 3601; ++frame)
            {
                ObservationSample sample = Quiet(frame);
                if (counts) sample.perEmitter.assign(65536, 0);
                if (!accounted.Add(ObservationSampleLine(sample).size())) return frame;
            }
            return -1;
        };
        int           predicted = -1;
        std::uint64_t total     = 0;
        for (int frame = 0; frame < 3601 && predicted < 0; ++frame)
        {
            ObservationSample sample = Quiet(frame);
            sample.perEmitter.assign(65536, 0);
            total += ObservationSampleLine(sample).size();
            if (total + 1024 * 1024 > budget) predicted = frame;
        }
        const int refused = refusalFrame(true);
        CHECK(refused == predicted && refused > 1,
              "the budget retains several wide samples and refuses at the frame their serialised lengths predict");
        CHECK(refusalFrame(false) != refused,
              "omitting the per-emitter bytes moves the refusal, so the accounting is observable");
        TelemetryByteBudget exact(1024 * 1024 + 100);
        CHECK(exact.Add(60) && exact.Add(40) && exact.Bytes() == 100 && !exact.Add(1),
              "the budget admits a line that lands exactly on maxBytes and refuses the next byte");
    }
    {
        ObservationSample sample = Quiet(0);
        sample.particles         = 5;
        sample.perEmitter        = {2, 0, 3};
        CHECK(PerEmitterSumMatches(sample), "per-emitter counts that sum to the particle count match");
        sample.perEmitter = {2, 0, 2};
        CHECK(!PerEmitterSumMatches(sample), "a missing particle breaks the per-emitter sum");
        sample.particles = 0;
        sample.perEmitter.clear();
        CHECK(PerEmitterSumMatches(sample), "an effect with no emitters and no particles matches");
    }
    {
        // Emitter 0 is a parent; emitter 1 overlaps it; emitter 2 is a child that starts after its parent.
        const int counts[][3] = {{0, 0, 0}, {3, 0, 0}, {5, 2, 0}, {5, 4, 0}, {1, 4, 6}, {0, 0, 6}, {0, 0, 0}};
        std::vector<ObservationSample> series;
        for (int frame = 0; frame < 7; ++frame)
        {
            ObservationSample sample = Quiet(frame);
            sample.perEmitter.assign(counts[frame], counts[frame] + 3);
            for (int count : sample.perEmitter) sample.particles += count;
            series.push_back(sample);
        }
        const auto none = PerEmitterLandmarks({Quiet(0), Quiet(1)}, 2);
        CHECK(none.size() == 2 && !none[0].firstActive && !none[1].peakFrame && none[1].peakParticles == 0,
              "an emitter that never emits has null landmarks and peak zero");
        const auto marks = PerEmitterLandmarks(series, 3);
        CHECK(*marks[0].firstActive == 1 && *marks[0].peakFrame == 2 && *marks[0].lastActive == 4 &&
                  marks[0].peakParticles == 5,
              "one emitter's landmarks are frames, with its first peak kept in a tie");
        CHECK(*marks[1].firstActive == 2 && *marks[1].peakFrame == 3 && *marks[1].lastActive == 4 &&
                  marks[1].peakParticles == 4,
              "an overlapping emitter keeps its own landmarks");
        CHECK(*marks[2].firstActive == 4 && *marks[2].peakFrame == 4 && *marks[2].lastActive == 5 &&
                  marks[2].peakParticles == 6,
              "a child that starts after its parent is landmarked after it");
        ObservationSample frameZero = Quiet(0);
        frameZero.particles         = 1;
        frameZero.perEmitter        = {1};
        const auto zero             = PerEmitterLandmarks({frameZero}, 1);
        CHECK(zero[0].peakFrame && *zero[0].peakFrame == 0, "frame zero is a valid per-emitter peak");
    }
    CHECK(ValidateObservationSpec(defaults).empty(), "synthetic spec is valid");
    const Status statuses[] = {Status::Running, Status::NaturalEnd, Status::Capped, Status::SpawnLimited,
                               Status::Failed};
    const char*  names[]    = {"running", "natural-end", "capped", "spawn-limited", "failed"};
    for (int i = 0; i < 5; ++i)
        CHECK(std::strcmp(ObservationStatusString(statuses[i]), names[i]) == 0,
              "status has the specified wire spelling");
    {
        ObservationLifecycle run(defaults);
        CHECK(run.Status() == Status::Running && run.FinalFrame() == -1, "run starts before frame zero");
        ObservationSample sample = Quiet(0);
        sample.particles         = 1;
        CHECK(run.Push(sample) == Status::Running, "active frame starts the run");
        CHECK(run.Push(Quiet(1)) == Status::Running && run.Push(Quiet(2)) == Status::Running,
              "one frame short of the quiet tail is still running");
        CHECK(run.Push(Quiet(3)) == Status::NaturalEnd && run.FinalFrame() == 3,
              "natural end occurs exactly on the quiet tail boundary");
        CHECK(run.ReviewStartFrame() == 0 && run.ReviewEndFrame() == 3 && !run.EndedBeforeReview(),
              "oneshot reviews frame zero through final");
        CHECK(run.RuntimeGuardRefusals() == 0 && run.HardGuardRefusals() == 0 && run.IndexCapRefusals() == 0 &&
                  !run.FirstRuntimeGuardFrame() && !run.FirstHardGuardFrame() && !run.FirstIndexCapFrame(),
              "unfired limits have zero totals and no first frame");
        bool threw = false;
        try
        {
            run.Push(Quiet(4));
        }
        catch (const std::logic_error&)
        {
            threw = true;
        }
        CHECK(threw && run.FinalFrame() == 3, "ended runs reject later samples without mutation");
    }
    for (int cause = 0; cause < 4; ++cause)
    {
        ObservationLifecycle run(defaults);
        run.Push(Quiet(0));
        ObservationSample pending = Quiet(1);
        if (cause == 0) pending.emitters = 1;
        if (cause == 1) pending.spawnerIdle = false;
        if (cause == 2) pending.instances = 1;
        if (cause == 3) pending.particles = 1;
        CHECK(run.Push(pending) == Status::Running, "delayed emitter, armed spawner or live population is not quiet");
        run.Push(Quiet(2));
        CHECK(run.Push(Quiet(3)) == Status::Running && run.Push(Quiet(4)) == Status::NaturalEnd,
              "nonquiet frame resets the consecutive quiet tail");
    }
    for (int counter = 0; counter < 3; ++counter)
    {
        ObservationLifecycle run(defaults);
        for (int frame = 0; frame < 3; ++frame)
        {
            ObservationSample sample = Quiet(frame);
            if (frame >= 1)
            {
                if (counter == 0) sample.runtimeGuardRefusals = 7;
                if (counter == 1) sample.hardGuardRefusals = 7;
                if (counter == 2) sample.indexCapRefusals = 7;
            }
            CHECK(run.Push(sample) == (frame == 2 ? Status::SpawnLimited : Status::Running),
                  "each cause counter turns a quiet end into spawn-limited");
        }
        CHECK(run.RuntimeGuardRefusals() == (counter == 0 ? 7ull : 0ull) &&
                  run.HardGuardRefusals() == (counter == 1 ? 7ull : 0ull) &&
                  run.IndexCapRefusals() == (counter == 2 ? 7ull : 0ull),
              "counter totals stay attributed to their cause");
        CHECK(run.FirstRuntimeGuardFrame() == (counter == 0 ? std::optional<int>(1) : std::nullopt) &&
                  run.FirstHardGuardFrame() == (counter == 1 ? std::optional<int>(1) : std::nullopt) &&
                  run.FirstIndexCapFrame() == (counter == 2 ? std::optional<int>(1) : std::nullopt),
              "first nonzero frame is retained independently for each counter");
    }
    {
        ObservationLifecycle run(defaults);
        for (int frame = 0; frame <= 2; ++frame)
        {
            ObservationSample sample = Quiet(frame);
            sample.hardGuardRefusals = 1;
            CHECK(run.Push(sample) == (frame == 2 ? Status::SpawnLimited : Status::Running),
                  "empty frame-zero hard refusal finishes the quiet tail as spawn-limited");
        }
        CHECK(run.FirstHardGuardFrame() == 0 && run.HardGuardRefusals() == 1, "frame-zero refusal is preserved");
    }
    {
        ObservationSpec spec    = defaults;
        spec.scenario.maxFrames = 3;
        ObservationLifecycle run(spec);
        for (int frame = 0; frame <= 3; ++frame)
        {
            ObservationSample sample    = Quiet(frame);
            sample.particles            = 1;
            sample.runtimeGuardRefusals = static_cast<std::uint64_t>(frame);
            sample.hardGuardRefusals    = frame >= 2 ? 1 : 0;
            sample.indexCapRefusals     = frame == 3 ? 5 : 0;
            sample.overloadActive       = frame > 0;
            CHECK(run.Push(sample) == (frame == 3 ? Status::Capped : Status::Running),
                  "cap includes frames zero through cap even with refusals");
        }
        CHECK(run.FinalFrame() == 3 && run.RuntimeGuardRefusals() == 3 && run.HardGuardRefusals() == 1 &&
                  run.IndexCapRefusals() == 5,
              "capped run carries all cumulative totals");
        CHECK(run.FirstRuntimeGuardFrame() == 1 && run.FirstHardGuardFrame() == 2 && run.FirstIndexCapFrame() == 3,
              "first refusal frames remain pinned while totals grow");
    }
    for (bool limited : {false, true})
    {
        ObservationSpec spec    = defaults;
        spec.scenario.maxFrames = 2;
        ObservationLifecycle run(spec);
        for (int frame = 0; frame <= 2; ++frame)
        {
            ObservationSample sample = Quiet(frame);
            sample.indexCapRefusals  = limited ? 1 : 0;
            run.Push(sample);
        }
        CHECK(run.Status() == (limited ? Status::SpawnLimited : Status::NaturalEnd),
              "quiet tail completed at the cap takes precedence over capped");
    }
    for (int kind = 0; kind < 3; ++kind)
    {
        ObservationLifecycle run(defaults);
        for (int frame = 0; frame <= defaults.scenario.maxFrames; ++frame)
        {
            ObservationSample sample = Quiet(frame);
            sample.instances         = 1;
            sample.emitters          = 1;
            sample.particles         = kind == 0 ? frame + 1 : 1;
            run.Push(sample);
        }
        CHECK(run.Status() == Status::Capped, "weather, freezeTime and infinite bursts stay alive through the cap");
    }
    {
        ObservationSpec spec        = defaults;
        spec.lifecycle.type         = Type::Continuous;
        spec.lifecycle.warmupFrames = 5;
        spec.lifecycle.reviewFrames = 5;
        ObservationLifecycle run(spec);
        run.Push(Quiet(0));
        run.Push(Quiet(1));
        run.Push(Quiet(2));
        CHECK(run.Status() == Status::NaturalEnd && run.EndedBeforeReview() && run.ReviewStartFrame() == 2 &&
                  run.ReviewEndFrame() == 2,
              "continuous death before warmup collapses review window to final");
        spec.lifecycle.warmupFrames = 2;
        spec.lifecycle.reviewFrames = 3;
        ObservationLifecycle boundary(spec);
        boundary.Push(Quiet(0));
        boundary.Push(Quiet(1));
        boundary.Push(Quiet(2));
        CHECK(!boundary.EndedBeforeReview() && boundary.ReviewStartFrame() == 2 && boundary.ReviewEndFrame() == 2,
              "death exactly at warmup has entered review");
        ObservationLifecycle capped(spec);
        for (int frame = 0; frame <= 5; ++frame)
        {
            ObservationSample sample = Quiet(frame);
            sample.emitters          = 1;
            capped.Push(sample);
        }
        CHECK(capped.Status() == Status::Capped && capped.FinalFrame() == 5 && capped.ReviewStartFrame() == 2 &&
                  capped.ReviewEndFrame() == 5,
              "continuous uses warmup plus review as cap, below scenario.maxFrames");
    }
    {
        ObservationLifecycle run(defaults);
        bool                 threw = false;
        try
        {
            run.Push(Quiet(1));
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw && run.FinalFrame() == -1, "first Push must be frame zero");
        run.Push(Quiet(0));
        for (int frame : {0, 2, -1})
        {
            threw = false;
            try
            {
                run.Push(Quiet(frame));
            }
            catch (const std::invalid_argument&)
            {
                threw = true;
            }
            CHECK(threw && run.FinalFrame() == 0, "duplicate, skipped and reversed frames leave state untouched");
        }
        CHECK(run.Push(Quiet(1)) == Status::Running, "ordered Push recovers after rejected sample");
    }

    std::vector<ObservationSample> samples;
    const int                      particles[] = {0, 0, 2, 5, 5, 0, 1, 0, 0, 0, 0};
    for (int frame = 0; frame <= 10; ++frame)
    {
        ObservationSample sample = Quiet(frame);
        sample.particles         = particles[frame];
        samples.push_back(sample);
    }
    auto selected = SelectFrames(samples, defaults);
    CHECK(Frames(selected, "stride") == std::vector<int>({0, 3, 6, 9, 10}),
          "fixed stride covers a non-divisible window and appends final");
    CHECK(Frames(selected) == std::vector<int>({0, 2, 3, 6, 9, 10}), "selection is sorted and deduplicated");
    CHECK(HasReason(selected, 0, "frame-zero") && HasReason(selected, 2, "first-active") &&
              HasReason(selected, 3, "peak") && !HasReason(selected, 4, "peak") &&
              HasReason(selected, 6, "last-active") && HasReason(selected, 10, "final"),
          "selection preserves reasons and picks the first peak in a tie");
    CHECK(selected[2].reasons == std::vector<std::string>({"stride", "peak"}), "overlapping reasons share one frame");
    ObservationSpec spec        = defaults;
    spec.lifecycle.type         = Type::Continuous;
    spec.scenario.maxFrames     = 20;
    spec.lifecycle.warmupFrames = 4;
    spec.lifecycle.reviewFrames = 10;
    selected                    = SelectFrames(samples, spec);
    CHECK(Frames(selected, "stride") == std::vector<int>({4, 7, 10}), "continuous strides begin at warmup");
    CHECK(HasReason(selected, 2, "first-active") && HasReason(selected, 3, "peak") &&
              HasReason(selected, 0, "frame-zero"),
          "activity landmarks and frame zero survive outside continuous review");
    spec.lifecycle.warmupFrames = 12;
    spec.lifecycle.reviewFrames = 1;
    selected                    = SelectFrames(samples, spec);
    CHECK(Frames(selected, "stride") == std::vector<int>({10}), "early continuous end has final as its only stride");
    spec                       = defaults;
    spec.output.strideInterval = 2;
    selected                   = SelectFrames(samples, spec);
    CHECK(Frames(selected, "stride") == std::vector<int>({0, 2, 4, 6, 8, 10}) &&
              selected.back().reasons == std::vector<std::string>({"stride", "final"}),
          "divisible window includes final once with both reasons");
    {
        ObservationLifecycle           run(defaults);
        std::vector<ObservationSample> emptyEffect{Quiet(0), Quiet(1), Quiet(2)};
        for (auto sample : emptyEffect) run.Push(sample);
        CHECK(!run.FirstActiveFrame() && !run.PeakFrame() && !run.LastActiveFrame(), "never-emits landmarks are null");
        selected = SelectFrames(emptyEffect, defaults);
        CHECK(Frames(selected, "first-active").empty() && Frames(selected, "peak").empty() &&
                  Frames(selected, "last-active").empty(),
              "never-emits selects no activity landmarks");
    }
    spec                           = defaults;
    spec.lifecycle.quietTailFrames = 1;
    ObservationLifecycle frameZero(spec);
    CHECK(frameZero.Push(Quiet(0)) == Status::NaturalEnd && frameZero.FinalFrame() == 0,
          "frame zero may complete the quiet tail");
    selected = SelectFrames({Quiet(0)}, spec);
    CHECK(selected.size() == 1 && selected[0].frame == 0 &&
              selected[0].reasons == std::vector<std::string>({"frame-zero", "stride", "final"}),
          "final frame zero has deduplicated selection with every applicable reason");
    ObservationSample active = Quiet(0);
    active.particles         = 1;
    selected                 = SelectFrames({active}, spec);
    CHECK(selected[0].reasons ==
              std::vector<std::string>({"frame-zero", "stride", "first-active", "peak", "last-active", "final"}),
          "all six reasons can coincide on one selected frame");
    CHECK(SelectFrames({}, spec).empty(), "empty samples produce no selected frames");
    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
