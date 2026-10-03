// Unit tests for src/host/CaptureTicker.{h,cpp} — the --capture per-frame state
// machine CaptureRunner drives, run with stub hooks + a fake clock (no host /
// WebView2 / D3D9). Covers pacing, the layout-determinism gate and its 10 s
// timeout, the counted-frame clock step, the engine-RT + composite writes and
// their names, and the exit-code mapping.
#include <cstdio>
#include <string>
#include <vector>
#include "host/CaptureTicker.h"

static int g_fail = 0;
static int g_check = 0;
#define CHECK(cond) do { ++g_check; if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

using host::CaptureTicker;

// Stub host: a fake clock advanced by the pacing sleep, readiness flags, and a
// record of every hook call.
struct Host {
    double now = 1000.0;
    bool   clock = true;          // false = no QPC (NowMs reports < 0)
    bool   uiReady = true;
    bool   layoutReady = true;
    int    sleeps = 0;
    int    steps = 0;
    bool   engineOk = true;
    bool   compositeOk = true;
    CaptureTicker::UiWait wait = { true, false, 150.0 };
    std::vector<std::wstring> engineWrites;
    std::vector<std::wstring> compositeWrites;
    std::vector<std::string>  logs;

    void Wire(CaptureTicker& t) {
        t.SetHooks(
            [this](int ms) { ++sleeps; now += ms; },
            [this] { return clock ? now : -1.0; },
            [this] { return uiReady; },
            [this] { return layoutReady; },
            [this] { ++steps; },
            [this](const std::string& s) { logs.push_back(s); });
        t.SetCaptureHooks(
            [this](const std::wstring& p) { engineWrites.push_back(p); return engineOk; },
            [this](const std::wstring& p) { compositeWrites.push_back(p); return compositeOk; },
            [this] { return wait; });
    }
};

static int RunToDone(CaptureTicker& t, int maxIters = 100000) {
    int i = 0;
    while (t.Tick() != CaptureTicker::Result::Done) {
        if (++i > maxIters) { CHECK(!"RunToDone exceeded iteration cap"); break; }
    }
    return i + 1;   // Tick calls made
}

static bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines)
        if (line.find(needle) != std::string::npos) return true;
    return false;
}

int main()
{
    // 1. Happy path: ready UI, N counted frames, both PNGs, exit 0.
    {
        Host h; CaptureTicker t(3, L"C:\\out\\shot.png"); h.Wire(t);
        CHECK(t.Tick() == CaptureTicker::Result::Running);
        CHECK(t.Tick() == CaptureTicker::Result::Running);
        CHECK(h.engineWrites.empty());
        CHECK(t.Tick() == CaptureTicker::Result::Done);
        CHECK(h.sleeps == 3);                      // paced every iteration
        CHECK(h.steps == 3);                       // one clock step per counted frame
        CHECK(t.CapturedFrames() == 3);
        CHECK(h.engineWrites.size() == 1 && h.engineWrites[0] == L"C:\\out\\shot.png");
        CHECK(h.compositeWrites.size() == 1 &&
              h.compositeWrites[0] == L"C:\\out\\shot-composite.png");
        CHECK(!t.Failed() && t.ExitCode() == 0);
        CHECK(Contains(h.logs, "engine-RT C:\\out\\shot.png -> ok"));
        CHECK(Contains(h.logs, "ui-ready=1"));
        // Done is sticky: a further Tick does nothing.
        CHECK(t.Tick() == CaptureTicker::Result::Done);
        CHECK(h.sleeps == 3 && h.engineWrites.size() == 1);
    }

    // 2. Layout gate holds the frame counter (and the sim clock) until React's
    // first paint AND first scene-rect have landed.
    {
        Host h; h.uiReady = false; h.layoutReady = false;
        CaptureTicker t(2, L"a.png"); h.Wire(t);
        for (int i = 0; i < 10; ++i) CHECK(t.Tick() == CaptureTicker::Result::Running);
        CHECK(t.CapturedFrames() == 0 && h.steps == 0);
        CHECK(h.sleeps == 10);                     // still paced while held
        h.uiReady = true;                          // paint alone is not enough
        CHECK(t.Tick() == CaptureTicker::Result::Running);
        CHECK(t.CapturedFrames() == 0);
        h.layoutReady = true;
        RunToDone(t);
        CHECK(t.CapturedFrames() == 2 && h.steps == 2);
        CHECK(!Contains(h.logs, "layout gate timed out"));
        CHECK(t.ExitCode() == 0);
    }

    // 3. The gate gives up after 10 s of held time, warns ONCE, and proceeds
    // ungated.
    {
        Host h; h.layoutReady = false;
        CaptureTicker t(5, L"a.png"); h.Wire(t);
        int held = 0;
        while (t.CapturedFrames() == 0 && held < 10000) { t.Tick(); ++held; }
        // The first held Tick starts the timer; each Tick advances 16 ms.
        CHECK(held >= 10000 / CaptureTicker::kPaceMs);
        CHECK(held <= 10000 / CaptureTicker::kPaceMs + 2);
        RunToDone(t);
        CHECK(t.CapturedFrames() == 5);
        int warnings = 0;
        for (const auto& l : h.logs) if (l.find("layout gate timed out") != std::string::npos) ++warnings;
        CHECK(warnings == 1);
        CHECK(Contains(h.logs, "sceneRect=0"));
        CHECK(t.ExitCode() == 0);                  // a degraded gate is not a failure
    }

    // 4. With no clock at all the gate is treated as already timed out.
    {
        Host h; h.clock = false; h.uiReady = false;
        CaptureTicker t(1, L"a.png"); h.Wire(t);
        CHECK(t.Tick() == CaptureTicker::Result::Done);
        CHECK(Contains(h.logs, "layout gate timed out"));
    }

    // 5. A failed engine-RT write fails the run (exit 2) but the composite is
    // still attempted — it is most useful exactly when a render broke.
    {
        Host h; h.engineOk = false;
        CaptureTicker t(1, L"a.png"); h.Wire(t);
        RunToDone(t);
        CHECK(t.Failed() && t.ExitCode() == 2);
        CHECK(h.compositeWrites.size() == 1);
        CHECK(Contains(h.logs, "-> FAILED; composite"));
    }

    // 6. A UI that never painted yields a distinctly named composite and a
    // warning, but the exit code stays engine-RT-driven.
    {
        Host h; h.wait = { false, true, 30000.0 };
        CaptureTicker t(1, L"C:\\out\\shot.png"); h.Wire(t);
        RunToDone(t);
        CHECK(h.compositeWrites.size() == 1 &&
              h.compositeWrites[0] == L"C:\\out\\shot-composite-TIMEOUT.png");
        CHECK(Contains(h.logs, "TIMEOUT)"));
        CHECK(Contains(h.logs, "30s timeout"));
        CHECK(t.ExitCode() == 0);
    }

    // 7. The window closing mid-wait is reported as ABORTED, not TIMEOUT.
    {
        Host h; h.wait = { false, false, 50.0 };
        CaptureTicker t(1, L"a.png"); h.Wire(t);
        RunToDone(t);
        CHECK(Contains(h.logs, "ABORTED)"));
        CHECK(Contains(h.logs, "window closed mid-wait"));
        CHECK(t.ExitCode() == 0);
    }

    // 8. A composite write failure is logged but does not fail the run.
    {
        Host h; h.compositeOk = false;
        CaptureTicker t(1, L"a.png"); h.Wire(t);
        RunToDone(t);
        CHECK(Contains(h.logs, "composite a-composite.png -> FAILED"));
        CHECK(t.ExitCode() == 0);
    }

    // 9. A setup failure (MarkFailed from Init) maps to exit 2 even when the
    // writes succeed.
    {
        Host h;
        CaptureTicker t(1, L"a.png"); h.Wire(t);
        t.MarkFailed();
        RunToDone(t);
        CHECK(t.Failed() && t.ExitCode() == 2);
    }

    std::printf("%s (%d checks, %d failed)\n",
                g_fail ? "=== FAILED ===" : "=== ALL PASS ===", g_check, g_fail);
    return g_fail ? 1 : 0;
}
