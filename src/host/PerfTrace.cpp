#include "PerfTrace.h"

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <mutex>
#include <utility>

namespace host::perf {
namespace {

std::mutex g_mutex;
Config g_config;
FILE* g_file = nullptr;
bool g_initialized = false;
long long g_freq = 0;
volatile LONGLONG g_spanSeq = 0;

long long QueryQpc()
{
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    return static_cast<long long>(qpc.QuadPart);
}

std::string MakeDefaultId(const char* prefix)
{
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "%s-%lu-%lld",
                  prefix,
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  QueryQpc());
    return std::string(buf);
}

std::string MakeSpanId()
{
    const LONGLONG seq = InterlockedIncrement64(&g_spanSeq);
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "span-%lu-%lld-%lld",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<long long>(seq),
                  QueryQpc());
    return std::string(buf);
}

void AddCommonFields(nlohmann::json& event)
{
    const long long timestampQpc = QueryQpc();
    event["schemaVersion"] = 1;
    event["source"] = "codex-perf-audit";
    event["auditId"] = g_config.auditId;
    event["instrumentationVersion"] = g_config.instrumentationVersion;
    event["sessionId"] = g_config.sessionId;
    event["runId"] = g_config.runId;
    event["processId"] = static_cast<unsigned long>(GetCurrentProcessId());
    event["timestampQpc"] = timestampQpc;
    event["timestampMs"] = g_freq > 0 ? (static_cast<double>(timestampQpc) * 1000.0 / static_cast<double>(g_freq)) : 0.0;
    event["qpcFrequency"] = g_freq;
}

} // namespace

SinkMode ParseSinkMode(const std::wstring& value)
{
    std::wstring mode = value;
    for (wchar_t& ch : mode) ch = static_cast<wchar_t>(std::towlower(ch));
    if (mode == L"null") return SinkMode::Null;
    if (mode == L"file") return SinkMode::File;
    return SinkMode::Off;
}

bool Init(const Config& cfg, std::string* err)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }

    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);
    g_freq = static_cast<long long>(freq.QuadPart);
    g_config = cfg;
    if (g_config.sessionId.empty()) g_config.sessionId = MakeDefaultId("session");
    if (g_config.runId.empty()) g_config.runId = MakeDefaultId("run");
    g_initialized = true;

    if (g_config.mode == SinkMode::File) {
        if (g_config.tracePath.empty()) {
            if (err) *err = "perf trace file sink requires a trace path";
            g_config.mode = SinkMode::Off;
            return false;
        }
        if (_wfopen_s(&g_file, g_config.tracePath.c_str(), L"wb") != 0 || g_file == nullptr) {
            if (err) *err = "could not open perf trace sink";
            g_config.mode = SinkMode::Off;
            return false;
        }
    }
    return true;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fflush(g_file);
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_initialized = false;
}

bool Enabled()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialized && g_config.mode != SinkMode::Off;
}

bool FileSinkEnabled()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_initialized && g_config.mode == SinkMode::File && g_file != nullptr;
}

long long NowQpc()
{
    return QueryQpc();
}

long long QpcFrequency()
{
    if (g_freq == 0) {
        LARGE_INTEGER freq = {};
        QueryPerformanceFrequency(&freq);
        g_freq = static_cast<long long>(freq.QuadPart);
    }
    return g_freq;
}

double NowMs()
{
    const long long freq = QpcFrequency();
    return freq > 0 ? (static_cast<double>(QueryQpc()) * 1000.0 / static_cast<double>(freq)) : 0.0;
}

const Config& CurrentConfig()
{
    return g_config;
}

void Emit(nlohmann::json event)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || g_config.mode == SinkMode::Off) return;
    AddCommonFields(event);
    if (g_config.mode == SinkMode::Null) return;
    if (!g_file) return;
    const std::string line = event.dump();
    std::fwrite(line.data(), 1, line.size(), g_file);
    std::fwrite("\n", 1, 1, g_file);
    std::fflush(g_file);
}

Span::Span(std::string eventName, nlohmann::json fields)
    : m_eventName(std::move(eventName))
    , m_fields(std::move(fields))
    , m_active(Enabled())
{
    if (!m_active) return;
    m_spanId = MakeSpanId();
    m_startQpc = QueryQpc();
    if (auto it = m_fields.find("parentSpanId"); it != m_fields.end() && it->is_string())
        m_parentSpanId = it->get<std::string>();
    nlohmann::json event = m_fields;
    event["eventName"] = m_eventName;
    event["eventType"] = "span_start";
    event["spanId"] = m_spanId;
    if (!m_parentSpanId.empty()) event["parentSpanId"] = m_parentSpanId;
    event["timestampQpc"] = m_startQpc;
    Emit(std::move(event));
}

Span::~Span()
{
    End();
}

Span::Span(Span&& other) noexcept
    : m_eventName(std::move(other.m_eventName))
    , m_spanId(std::move(other.m_spanId))
    , m_parentSpanId(std::move(other.m_parentSpanId))
    , m_fields(std::move(other.m_fields))
    , m_startQpc(other.m_startQpc)
    , m_active(other.m_active)
{
    other.m_active = false;
}

Span& Span::operator=(Span&& other) noexcept
{
    if (this != &other) {
        End();
        m_eventName = std::move(other.m_eventName);
        m_spanId = std::move(other.m_spanId);
        m_parentSpanId = std::move(other.m_parentSpanId);
        m_fields = std::move(other.m_fields);
        m_startQpc = other.m_startQpc;
        m_active = other.m_active;
        other.m_active = false;
    }
    return *this;
}

void Span::End(std::string status, std::string error)
{
    if (!m_active) return;
    m_active = false;
    const long long endQpc = QueryQpc();
    const long long freq = QpcFrequency();
    nlohmann::json event = m_fields;
    event["eventName"] = m_eventName;
    event["eventType"] = "span_end";
    event["spanId"] = m_spanId;
    if (!m_parentSpanId.empty()) event["parentSpanId"] = m_parentSpanId;
    event["timestampQpc"] = endQpc;
    event["startQpc"] = m_startQpc;
    event["endQpc"] = endQpc;
    event["durationMs"] = freq > 0 ? (static_cast<double>(endQpc - m_startQpc) * 1000.0 / static_cast<double>(freq)) : 0.0;
    event["status"] = status;
    if (!error.empty()) event["error"] = error;
    Emit(std::move(event));
}

} // namespace host::perf
