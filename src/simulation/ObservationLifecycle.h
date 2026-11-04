#ifndef OBSERVATION_LIFECYCLE_H
#define OBSERVATION_LIFECYCLE_H

#include "ObservationSpec.h"
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

struct ObservationSample
{
    int           frame;
    int           instances, emitters, particles;
    std::uint64_t rngDrawCount;
    std::uint32_t rngState;
    std::uint64_t runtimeGuardRefusals, hardGuardRefusals, indexCapRefusals;
    bool          overloadActive;
    bool          spawnerIdle;
    // Live particles per authored emitter index; sums to particles.
    std::vector<int> perEmitter;
};

enum class ObservationStatus
{
    Running,
    NaturalEnd,
    Capped,
    SpawnLimited,
    Failed
};

// Fixed allowance for capture.json, the summary and the log.
const std::uint64_t ObservationReserveBytes = 1024 * 1024;

// Early refusal only: GDI+ can exceed this estimate; actual writes enforce the quota.
// Telemetry is already serialised by then, so its bytes are exact.
inline std::uint64_t ObservationPreflightBytes(std::uint64_t measuredPngBytes,
                                               std::uint64_t pngs,
                                               std::uint64_t telemetryBytes)
{
    const std::uint64_t perPng = measuredPngBytes + measuredPngBytes / 2 + measuredPngBytes % 2;
    return perPng * pngs + telemetryBytes + ObservationReserveBytes;
}

// Counts each telemetry line as its sample is retained, so the per-emitter
// arrays can never hold more than the spec's budget, whatever the emitter count.
class TelemetryByteBudget
{
public:
    explicit TelemetryByteBudget(std::uint64_t maxBytes) : m_maxBytes(maxBytes) {}

    // False at the first line that takes the total plus the reserve past maxBytes.
    bool Add(std::uint64_t lineBytes)
    {
        m_bytes += lineBytes;
        return m_bytes + ObservationReserveBytes <= m_maxBytes;
    }
    std::uint64_t Bytes() const { return m_bytes; }

private:
    std::uint64_t m_maxBytes;
    std::uint64_t m_bytes = 0;
};

// True when the per-emitter counts account for every live particle.
inline bool PerEmitterSumMatches(const ObservationSample& sample)
{
    long long sum = 0;
    for (int count : sample.perEmitter) sum += count;
    return sum == sample.particles;
}

// One authored emitter's landmarks. Frames follow the global convention:
// peakFrame is a frame, peakParticles the emitter's maximum live count.
struct EmitterLandmarks
{
    std::optional<int> firstActive, peakFrame, lastActive;
    int                peakParticles = 0;
};

inline std::vector<EmitterLandmarks> PerEmitterLandmarks(const std::vector<ObservationSample>& samples, size_t emitters)
{
    std::vector<EmitterLandmarks> result(emitters);
    for (const ObservationSample& sample : samples)
        for (size_t i = 0; i < emitters && i < sample.perEmitter.size(); ++i)
        {
            const int count = sample.perEmitter[i];
            if (count <= 0) continue;
            EmitterLandmarks& emitter = result[i];
            if (!emitter.firstActive) emitter.firstActive = sample.frame;
            if (count > emitter.peakParticles)
            {
                emitter.peakParticles = count;
                emitter.peakFrame     = sample.frame;
            }
            emitter.lastActive = sample.frame;
        }
    return result;
}

inline const char* ObservationStatusString(ObservationStatus status)
{
    switch (status)
    {
        case ObservationStatus::Running:      return "running";
        case ObservationStatus::NaturalEnd:   return "natural-end";
        case ObservationStatus::Capped:       return "capped";
        case ObservationStatus::SpawnLimited: return "spawn-limited";
        case ObservationStatus::Failed:       return "failed";
    }
    return "failed";
}

class ObservationLifecycle
{
public:
    explicit ObservationLifecycle(const ObservationSpec& spec) : m_spec(spec) {}

    ObservationStatus Push(const ObservationSample& sample)
    {
        if (m_status != ObservationStatus::Running) throw std::logic_error("observation already ended");
        if (sample.frame != m_finalFrame + 1)
            throw std::invalid_argument("observation frame must follow the previous frame, starting at 0");
        m_finalFrame           = sample.frame;
        m_runtimeGuardRefusals = sample.runtimeGuardRefusals;
        m_hardGuardRefusals    = sample.hardGuardRefusals;
        m_indexCapRefusals     = sample.indexCapRefusals;
        if (sample.runtimeGuardRefusals != 0 && !m_firstRuntimeGuardFrame) m_firstRuntimeGuardFrame = sample.frame;
        if (sample.hardGuardRefusals != 0 && !m_firstHardGuardFrame) m_firstHardGuardFrame = sample.frame;
        if (sample.indexCapRefusals != 0 && !m_firstIndexCapFrame) m_firstIndexCapFrame = sample.frame;
        if (sample.particles > 0)
        {
            if (!m_firstActiveFrame) m_firstActiveFrame = sample.frame;
            if (sample.particles > m_peakParticles)
            {
                m_peakParticles = sample.particles;
                m_peakFrame     = sample.frame;
            }
            m_lastActiveFrame = sample.frame;
        }
        const bool quiet = sample.instances == 0 && sample.emitters == 0 && sample.particles == 0 && sample.spawnerIdle;
        m_quietFrames    = quiet ? m_quietFrames + 1 : 0;
        // A completed quiet tail wins even when its last frame is the cap.
        if (m_quietFrames >= m_spec.lifecycle.quietTailFrames)
            m_status = m_runtimeGuardRefusals == 0 && m_hardGuardRefusals == 0 && m_indexCapRefusals == 0
                           ? ObservationStatus::NaturalEnd
                           : ObservationStatus::SpawnLimited;
        else if (sample.frame >= ObservationRunCap(m_spec))
            m_status = ObservationStatus::Capped;
        return m_status;
    }

    ObservationStatus  Status() const { return m_status; }
    int                FinalFrame() const { return m_finalFrame; }
    std::uint64_t      RuntimeGuardRefusals() const { return m_runtimeGuardRefusals; }
    std::uint64_t      HardGuardRefusals() const { return m_hardGuardRefusals; }
    std::uint64_t      IndexCapRefusals() const { return m_indexCapRefusals; }
    std::optional<int> FirstRuntimeGuardFrame() const { return m_firstRuntimeGuardFrame; }
    std::optional<int> FirstHardGuardFrame() const { return m_firstHardGuardFrame; }
    std::optional<int> FirstIndexCapFrame() const { return m_firstIndexCapFrame; }
    std::optional<int> FirstActiveFrame() const { return m_firstActiveFrame; }
    std::optional<int> PeakFrame() const { return m_peakFrame; }
    std::optional<int> LastActiveFrame() const { return m_lastActiveFrame; }
    bool               EndedBeforeReview() const
    {
        return m_spec.lifecycle.type == ObservationSpec::Lifecycle::Type::Continuous &&
               m_status != ObservationStatus::Running && m_finalFrame < m_spec.lifecycle.warmupFrames;
    }
    int ReviewStartFrame() const
    {
        if (m_spec.lifecycle.type == ObservationSpec::Lifecycle::Type::Oneshot) return 0;
        return EndedBeforeReview() ? m_finalFrame : m_spec.lifecycle.warmupFrames;
    }
    int ReviewEndFrame() const { return m_finalFrame; }

private:
    ObservationSpec    m_spec;
    ObservationStatus  m_status               = ObservationStatus::Running;
    int                m_finalFrame           = -1;
    int                m_quietFrames          = 0;
    std::uint64_t      m_runtimeGuardRefusals = 0, m_hardGuardRefusals = 0, m_indexCapRefusals = 0;
    std::optional<int> m_firstRuntimeGuardFrame, m_firstHardGuardFrame, m_firstIndexCapFrame;
    std::optional<int> m_firstActiveFrame, m_peakFrame, m_lastActiveFrame;
    int                m_peakParticles = 0;
};

struct SelectedFrame
{
    int                      frame;
    std::vector<std::string> reasons;
};

inline std::vector<SelectedFrame> SelectFrames(const std::vector<ObservationSample>& samples,
                                               const ObservationSpec&                spec)
{
    if (samples.empty()) return {};
    const int final = samples.back().frame;
    int start = spec.lifecycle.type == ObservationSpec::Lifecycle::Type::Continuous ? spec.lifecycle.warmupFrames : 0;
    if (start > final) start = final;
    std::map<int, std::vector<std::string>> selected;
    selected[0].push_back("frame-zero");
    for (int frame = start; frame <= final; frame += spec.output.strideInterval) selected[frame].push_back("stride");
    if ((final - start) % spec.output.strideInterval != 0) selected[final].push_back("stride");
    std::optional<int> first, peak, last;
    int                peakParticles = 0;
    for (const ObservationSample& sample : samples)
    {
        if (sample.particles <= 0) continue;
        if (!first) first = sample.frame;
        if (sample.particles > peakParticles)
        {
            peakParticles = sample.particles;
            peak          = sample.frame;
        }
        last = sample.frame;
    }
    if (first) selected[*first].push_back("first-active");
    if (peak) selected[*peak].push_back("peak");
    if (last) selected[*last].push_back("last-active");
    selected[final].push_back("final");
    std::vector<SelectedFrame> result;
    for (const auto& entry : selected) result.push_back({entry.first, entry.second});
    return result;
}

#endif
