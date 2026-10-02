#ifndef HOST_CAPTURE_RUNNER_H
#define HOST_CAPTURE_RUNNER_H
//
// --capture / --capture-ref one-shot: load a .alo (or resolve a reference
// object), render N deterministic frames, write the engine RT + a composite
// PNG, exit. Extracted from HostWindowImpl::Run's inline capture segments,
// following the DriveRunner/ClipRunner hook shape: the shared message pump stays in Run();
// this runner owns setup (Init), the per-pump-iteration capture step (Tick),
// and the exit-code mapping (ExitCode).
//
// Init's setup segment binds Run()'s names through the .cpp's alias prelude.
// Tick's pacing / layout-gate / frame-count / exit-code state machine lives in
// CaptureTicker, which this runner wires to the live host through hooks so the
// machine itself is unit-tested (tests/test_capture_runner.cpp). Genuinely
// shared pump state (m_uiReady / m_sceneRectSeen) stays on HostWindowImpl and
// is read through const references.

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "CaptureTicker.h"

class Engine;
class ModManager;
class ParticleSystem;
class SpawnerDriver;

namespace host {

class AlphaCompositor;

// Copies of the Impl's m_capture* CLI values, in the Impl's field order.
struct CaptureRunnerParams {
    std::wstring captureAlo;
    std::wstring captureRef;
    std::wstring capturePng;
    int          captureFrames        = 180;
    int          captureSkydomeSlot   = 0;
    bool         captureGoldenProfile = false;
    bool         captureHasAmbient    = false;
    float        captureAmbient[3]    = {0, 0, 0};
    bool         captureHasSun        = false;
    float        captureSun[3]        = {0, 0, 0};
    bool         captureHasSunI       = false;
    float        captureSunIntensity  = 1.0f;
};

class CaptureRunner {
public:
    using Params = CaptureRunnerParams;

    // Live references into HostWindowImpl — the runner never owns any of it.
    // unique_ptr references (not raw pointers) so the moved code's `.get()`,
    // boolean tests, and the file-load `particleSystem = std::move(loaded)`
    // slot assignment stay verbatim.
    struct Deps {
        std::unique_ptr<Engine>&                engine;
        std::unique_ptr<::ModManager>&          modManager;
        std::unique_ptr<ParticleSystem>&        particleSystem;
        std::unique_ptr<SpawnerDriver>&         spawnerDriver;
        std::unique_ptr<host::AlphaCompositor>& alphaCompositor;
        HWND                                    hMain;
        const bool&                             uiReady;        // Impl m_uiReady
        const bool&                             sceneRectSeen;  // Impl m_sceneRectSeen
        std::function<void()>                   renderD3D9;     // Impl RenderD3D9()
        std::function<void(const std::string&)> log;            // Impl Log sink
    };

    enum class TickResult { Running, Done };

    CaptureRunner(Params params, Deps deps);

    // Holds live references + per-run counters — an accidental copy would
    // alias the same HostWindowImpl while forking failed/frame state.
    CaptureRunner(const CaptureRunner&)            = delete;
    CaptureRunner& operator=(const CaptureRunner&) = delete;
    CaptureRunner(CaptureRunner&&)                 = delete;
    CaptureRunner& operator=(CaptureRunner&&)      = delete;

    // The pre-pump setup segment: mod-select + .alo load + spawner burst +
    // deterministic clock, or the synchronous --capture-ref catalog resolve;
    // skydome/lighting/camera overrides; the redbug/shadow-repro env hooks.
    // Failures set the failed flag (the run still pumps and exits 2, matching
    // the old inline behavior).
    void Init();

    // The pump's capture step, called once per iteration right after
    // RenderD3D9() — paces the sim, holds the layout-determinism gate, steps
    // the frozen clock, and on the target frame writes the engine RT + the
    // composite (with the app/ready wait + settle). Returns Done when the
    // run is complete and the pump should quit.
    TickResult Tick();

    bool Failed() const { return m_ticker.Failed(); }
    // Same mapping the inline code returned from Run(): 2 = bad load /
    // failed write, 0 = success.
    int ExitCode() const { return m_ticker.ExitCode(); }

private:
    // printf-style forwarder so moved Log(...) call sites stay verbatim.
    void Log(const char* fmt, ...);
    // Forwarder so moved RenderD3D9() call sites stay verbatim.
    void RenderD3D9() { m_deps.renderD3D9(); }
    // The ticker's app/ready wait: pump + render until React's first paint
    // (30 s cap), then settle ~150 ms so the composition has caught up.
    CaptureTicker::UiWait WaitForUiAndSettle();

    const Params  m_params;
    Deps          m_deps;
    CaptureTicker m_ticker;
};

}  // namespace host

#endif  // HOST_CAPTURE_RUNNER_H
