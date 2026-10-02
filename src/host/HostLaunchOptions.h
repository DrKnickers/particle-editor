// HostLaunchOptions — every launch setting WinMain hands to the host. main.cpp
// parses the command line (and the ALO_PERF_* environment fallbacks) once and
// fills this struct; host::Run, HostWindow and HostWindowImpl take it by const
// reference instead of a long positional parameter list whose adjacent
// bool/float runs were easy to transpose.
//
// The defaults below are the values main.cpp uses when a flag is absent.
#ifndef HOST_HOST_LAUNCH_OPTIONS_H
#define HOST_HOST_LAUNCH_OPTIONS_H

#include <string>

namespace host {

struct HostLaunchOptions
{
    // --dev-ui: probe http://localhost:5174 (Vite dev server) and navigate
    // there instead of the bundled app.local build. If the probe fails the
    // host shows a MessageBox and returns 1 immediately.
    bool useDevUi = false;

    // --test-host: pass `--remote-debugging-port=9222` to WebView2 via the
    // environment's AdditionalBrowserArguments and enable DevTools (F12). This
    // exposes a CDP endpoint for Playwright contract tests. Opt-in only:
    // production launches (no flag) never expose the port.
    bool useTestHost = false;

    // --capture <alo> <png>: one-shot frame-capture mode. When captureAlo +
    // capturePng are both non-empty, the host loads captureAlo, renders
    // captureFrames frames, writes the engine's render target to capturePng, and
    // exits. Used to inspect/diff rendering fidelity offline (engine pixels
    // are invisible to Playwright under composition). Empty paths = normal
    // interactive run.
    std::wstring captureAlo;
    std::wstring capturePng;
    // --frames N: default ~180 frames ≈ 3 s of sim (loop paces ~16 ms/frame)
    // so a freshly-spawned effect has time to fill before the snapshot.
    int captureFrames = 180;
    // --skydome <slot>: apply this skydome slot in --capture mode before
    // rendering (0 = Off / solid colour). Lets a capture verify particles
    // render correctly over a background skydome (regression for the
    // RenderSkydome vertex-declaration leak).
    int captureSkydome = 0;
    // --golden-profile: internal render-oracle profile. Valid only for
    // --capture with the canonical skydome slot 1 view.
    bool captureGoldenProfile = false;
    // --capture-ref <objectName> <png>: render a game reference object (with
    // its shadow) headlessly instead of a particle system. When non-empty
    // (with capturePng), the host builds the GameObject catalog synchronously,
    // selects the named reference object, renders captureFrames frames, and
    // writes the result to capturePng. Mutually exclusive with captureAlo.
    std::wstring captureRef;

    // [world-lit] --ambient r,g,b / --sun r,g,b / --sun-intensity f: drive
    // scene lighting in a headless --capture run. Each has* flag is opt-in;
    // when false the engine's ctor-default lighting is left untouched.
    bool  hasAmbient   = false;
    float ambient[3]   = {0, 0, 0};
    bool  hasSun       = false;
    float sun[3]       = {0, 0, 0};
    bool  hasSunIntensity = false;
    float sunIntensity = 1.0f;

    // --drive <script.json>: launch the full editor (no CDP), replay an
    // allowlisted ordered list of bridge commands in-process via
    // BridgeDispatcher::DispatchSync, capture the composed window, then exit.
    // Non-empty = ephemeral drive mode (no settings/MRU/autosave persistence,
    // per-PID WebView2 profile + log). See DriveRunner / DriveScript.h.
    std::wstring driveScriptPath;
    // --record <timeline.json>: launch the full editor (no CDP), drive a
    // deterministic fixed-fps timeline (camera tweens + synthetic cursor +
    // allowlisted bridge events), emit a numbered PNG sequence, then exit.
    // Non-empty = ephemeral record mode (same persistence isolation as drive).
    // See ClipRunner / ClipTimeline.h.
    std::wstring recordScriptPath;

    // Performance-audit-only knobs (--perf-trace, --perf-trace-mode,
    // --perf-artifact-dir, --perf-webview-profile, or their ALO_PERF_*
    // environment variables). Empty values preserve the normal launch path;
    // explicit paths are used by reproducible perf runs.
    std::wstring perfTracePath;
    std::wstring perfTraceMode;
    std::wstring perfArtifactDir;
    std::wstring perfWebViewProfile;
};

} // namespace host

#endif // HOST_HOST_LAUNCH_OPTIONS_H
