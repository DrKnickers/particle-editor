// Entry point for the WebView2/React host. Invoked unconditionally from
// WinMain (the sole UI since an earlier change removed the legacy Win32 UI and the
// `--new-ui`/`--legacy` flags). Constructs the hybrid WebView2 + D3D9
// composition window, owns the Engine instance for the session, and runs
// the host message pump.
//
// Every launch setting (--dev-ui, --test-host, the capture / drive / record
// one-shots, the perf knobs) travels in `options`; see HostLaunchOptions.h.
//
// Returns the WM_QUIT wParam (process exit code).
#ifndef HOST_RUN_H
#define HOST_RUN_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "HostLaunchOptions.h"

class ITextureManager;
class IShaderManager;
class IFileManager;

namespace host {

// `gameRoots` is the EmpireAtWarPaths vector that was used
// to build `fileManager`. Threaded through so the host's ModManager
// can scan their Mods\ subdirectories on startup.
int Run(HINSTANCE hInstance,
        int nCmdShow,
        ITextureManager& textureManager,
        IShaderManager&  shaderManager,
        IFileManager&    fileManager,
        const std::vector<std::wstring>& gameRoots,
        const HostLaunchOptions& options);

} // namespace host

#endif // HOST_RUN_H
