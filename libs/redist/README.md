# Vendored redistributables

## `MicrosoftEdgeWebview2Setup.exe`

- **What it is:** Microsoft's *Evergreen Bootstrapper* (~2 MB) for the WebView2 runtime.
  It is a downloader, not the runtime itself — it fetches and installs the current
  runtime, so it never goes stale and does not need re-vendoring.
- **Why vendored:** the `WebView2Loader.dll` we ship beside the exe is the **loader**, not
  the **runtime**. The runtime is a machine component — present on Windows 11 and on any
  Windows 10 with Edge, but absent on stripped or LTSC images. Without it the editor
  launches and immediately tells the user to go install something else, which is exactly
  the dead end `d3dx9_43.dll` is vendored to avoid. With it present, the WebView2 failure
  path in [`src/host/HostWindow.cpp`](../../src/host/HostWindow.cpp) offers to run the
  installer instead of just reporting the problem.
- **How to obtain it:** download `MicrosoftEdgeWebview2Setup.exe` from
  <https://developer.microsoft.com/microsoft-edge/webview2/> ("Evergreen Bootstrapper")
  and commit it here. This is a **one-time manual step** — it is deliberately not
  scripted, because a build script that downloads and then ships an executable is a
  supply-chain hazard.
- **Enforcement:** [`scripts/package-release.ps1`](../../scripts/package-release.ps1)
  treats it as a REQUIRED source and fails the packaging run if it is missing, so a
  release cannot quietly ship without it.
- **License:** redistributable under the Microsoft Edge WebView2 distribution terms, which
  permit shipping the bootstrapper alongside an application.

## `d3dx9_43.dll`

- **Architecture:** x64 (PE machine `0x8664`). The editor is x64-only; the 32-bit copy
  (from `SysWOW64`) will **not** load into the x64 exe — do not substitute it.
- **Source:** the D3DX9 component of the **DirectX End-User Runtime (June 2010)** redistributable
  (the same DLL Windows places in `System32` on a 64-bit machine that has the DX9 runtime).
- **Why vendored:** `d3dx9_43.dll` is a hard runtime dependency of the D3D9 renderer and is **not**
  present by default on modern Windows. Vendoring it makes `scripts/package-release.ps1` produce a
  complete, self-contained release zip deterministically on any machine or CI runner, instead of
  relying on a per-machine `System32` copy.
- **Maintenance:** committed once; do not rebuild or regenerate. It is bundled beside
  `ParticleEditor.exe` in every release zip (see `scripts/package-release.ps1`).
- **License:** redistributable under the Microsoft DirectX SDK (June 2010) EULA, which permits
  shipping the redistributable runtime DLLs alongside an application.
