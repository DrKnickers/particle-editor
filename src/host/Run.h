// Entry point for the WebView2/React host. Invoked unconditionally from
// WinMain (the sole UI since removed the legacy Win32 UI and the
// `--new-ui`/`--legacy` flags). Constructs the hybrid WebView2 + D3D9
// composition window, owns the Engine instance for the session, and runs
// the host message pump.
//
// useDevUi — when true, probe http://localhost:5174 (Vite dev server)
// and navigate there instead of the bundled app.local build. If the
// probe fails the function shows a MessageBox and returns 1 immediately.
//
// useTestHost — when true (Task 2.2), pass
// `--remote-debugging-port=9222` to WebView2 via the environment's
// AdditionalBrowserArguments and enable DevTools (F12). This exposes a
// CDP endpoint for Playwright contract tests. Opt-in only: production
// launches (no flag) never expose the port.
//
// Returns the WM_QUIT wParam (process exit code).
#ifndef HOST_RUN_H
#define HOST_RUN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

class ITextureManager;
class IShaderManager;
class IFileManager;

namespace host {

// D6: `gameRoots` is the EmpireAtWarPaths vector that was used
// to build `fileManager`. Threaded through so the host's ModManager
// can scan their Mods\ subdirectories on startup. Legacy mode reads
// the same vector inside `main(APPLICATION_INFO*, argv)`.
// captureAlo / capturePng / captureFrames — rendering-fidelity]
// one-shot frame-capture mode. When captureAlo + capturePng are both
// non-empty, the host loads captureAlo, renders captureFrames frames,
// writes the engine's render target to capturePng, and exits. Used to
// inspect/diff rendering fidelity offline (engine pixels are invisible
// to Playwright under composition). Empty paths = normal interactive run.
int Run(HINSTANCE hInstance,
        int nCmdShow,
        ITextureManager& textureManager,
        IShaderManager&  shaderManager,
        IFileManager&    fileManager,
        const std::vector<std::wstring>& gameRoots,
        bool useDevUi   = false,
        bool useTestHost = false,
        const std::wstring& captureAlo = L"",
        const std::wstring& capturePng = L"",
        int captureFrames = 60,
        // --skydome <slot>: apply this skydome slot in --capture mode before
        // rendering (0 = Off / solid colour). Lets a capture verify particles
        // render correctly over a background skydome (regression for the
        // RenderSkydome vertex-declaration leak — see lessons).
        int captureSkydome = 0,
        // --capture-ref <objectName>: render a game reference object (with its
        // shadow) headlessly instead of a particle system. When non-empty (with
        // capturePng), the host builds the GameObject catalog synchronously,
        // selects the named reference object, renders captureFrames frames, and
        // writes the engine RT to capturePng. Mutually exclusive with captureAlo.
        const std::wstring& captureRef = L"",
        // [world-lit] --ambient / --sun / --sun-intensity: drive scene
        // lighting in a headless --capture run. Each *has* flag is opt-in;
        // when false the engine's ctor-default lighting is left untouched.
        bool hasAmbient = false, float ambR = 0.0f, float ambG = 0.0f, float ambB = 0.0f,
        bool hasSun = false, float sunR = 0.0f, float sunG = 0.0f, float sunB = 0.0f,
        bool hasSunI = false, float sunIntensity = 1.0f);

} // namespace host

#endif // HOST_RUN_H
