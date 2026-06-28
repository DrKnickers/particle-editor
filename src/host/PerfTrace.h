#ifndef HOST_PERF_TRACE_H
#define HOST_PERF_TRACE_H

#include <string>
#include "third_party/nlohmann/json.hpp"

namespace host::perf {

enum class SinkMode { Off, Null, File };

struct Config {
    SinkMode mode = SinkMode::Off;
    std::wstring tracePath;
    std::wstring artifactDir;
    std::string auditId = "perf-audit-2026-06-28";
    std::string instrumentationVersion = "1";
    std::string sessionId;
    std::string runId;
};

SinkMode ParseSinkMode(const std::wstring& value);
bool Init(const Config& cfg, std::string* err = nullptr);
void Shutdown();
bool Enabled();
bool FileSinkEnabled();
long long NowQpc();
long long QpcFrequency();
double NowMs();
const Config& CurrentConfig();
void Emit(nlohmann::json event);

class Span {
public:
    Span(std::string eventName, nlohmann::json fields = {});
    ~Span();

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

    Span(Span&& other) noexcept;
    Span& operator=(Span&& other) noexcept;

    void End(std::string status = "ok", std::string error = "");

private:
    std::string m_eventName;
    std::string m_spanId;
    std::string m_parentSpanId;
    nlohmann::json m_fields;
    long long m_startQpc = 0;
    bool m_active = false;
};

} // namespace host::perf

#endif // HOST_PERF_TRACE_H
