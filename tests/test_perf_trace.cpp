#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>

#include "PerfTrace.h"
#include "third_party/nlohmann/json.hpp"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static std::wstring temp_path(const wchar_t* leaf)
{
    wchar_t buf[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, buf);
    return std::wstring(buf) + leaf;
}

static std::vector<std::string> read_lines(const std::wstring& path)
{
    std::vector<std::string> lines;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return lines;

    char buf[4096] = {};
    while (std::fgets(buf, sizeof(buf), file)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        lines.push_back(line);
    }
    std::fclose(file);
    return lines;
}

int main()
{
    using namespace host::perf;

    {
        Config cfg;
        cfg.mode = SinkMode::Off;
        cfg.sessionId = "s-off";
        cfg.runId = "r-off";
        CHECK(Init(cfg));
        CHECK(!Enabled());
        Emit({{"eventName", "off.test"}, {"eventType", "instant"}});
        Shutdown();
    }

    {
        Config cfg;
        cfg.mode = SinkMode::Null;
        cfg.sessionId = "s-null";
        cfg.runId = "r-null";
        CHECK(Init(cfg));
        CHECK(Enabled());
        CHECK(!FileSinkEnabled());
        Emit({{"eventName", "null.test"}, {"eventType", "instant"}});
        Shutdown();
    }

    {
        const std::wstring path = temp_path(L"particle_editor_perf_trace_test.ndjson");
        DeleteFileW(path.c_str());
        Config cfg;
        cfg.mode = SinkMode::File;
        cfg.tracePath = path;
        cfg.sessionId = "s-file";
        cfg.runId = "r-file";
        CHECK(Init(cfg));
        CHECK(Enabled());
        CHECK(FileSinkEnabled());
        Emit({{"eventName", "file.instant"}, {"eventType", "instant"}});
        Emit({
            {"eventName", "file.spoof"},
            {"eventType", "instant"},
            {"source", "renderer-spoof"},
            {"auditId", "wrong"},
            {"sessionId", "wrong"},
            {"runId", "wrong"},
            {"processId", 0},
            {"timestampQpc", 1},
            {"timestampMs", 1},
            {"qpcFrequency", 1},
        });
        {
            Span span("file.span", {{"workflowId", "unit"}});
            span.End("ok");
        }
        Shutdown();

        const auto lines = read_lines(path);
        CHECK(lines.size() == 4);
        if (!lines.empty()) {
            auto j = nlohmann::json::parse(lines[0]);
            CHECK(j["source"] == "perf-audit");
            CHECK(j["schemaVersion"] == 1);
            CHECK(j["auditId"] == "perf-audit-2026-06-28");
            CHECK(j["instrumentationVersion"] == "1");
            CHECK(j["sessionId"] == "s-file");
            CHECK(j["runId"] == "r-file");
            CHECK(j.contains("timestampQpc"));
            CHECK(j.contains("timestampMs"));
            CHECK(j.contains("qpcFrequency"));
        }
        if (lines.size() >= 2) {
            auto spoof = nlohmann::json::parse(lines[1]);
            CHECK(spoof["source"] == "perf-audit");
            CHECK(spoof["auditId"] == "perf-audit-2026-06-28");
            CHECK(spoof["sessionId"] == "s-file");
            CHECK(spoof["runId"] == "r-file");
            CHECK(spoof["processId"].get<unsigned long>() == static_cast<unsigned long>(GetCurrentProcessId()));
            CHECK(spoof["timestampQpc"].get<long long>() != 1);
            CHECK(spoof["qpcFrequency"].get<long long>() != 1);
        }
        if (lines.size() >= 4) {
            auto start = nlohmann::json::parse(lines[2]);
            auto end = nlohmann::json::parse(lines[3]);
            CHECK(start["eventType"] == "span_start");
            CHECK(end["eventType"] == "span_end");
            CHECK(start.contains("spanId"));
            CHECK(end.contains("spanId"));
            CHECK(start["spanId"] == end["spanId"]);
            CHECK(end.contains("durationMs"));
            CHECK(end["status"] == "ok");
        }
        DeleteFileW(path.c_str());
    }

    {
        Config cfg;
        cfg.mode = SinkMode::File;
        cfg.tracePath = L"Z:\\this\\path\\should\\not\\exist\\trace.ndjson";
        std::string err;
        CHECK(!Init(cfg, &err));
        CHECK(!err.empty());
        Shutdown();
    }

    return g_fail == 0 ? 0 : 1;
}
