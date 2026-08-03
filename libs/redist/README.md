# Vendored redistributables

## `MicrosoftEdgeWebview2Setup.exe`

- **What it is:** Microsoft's *Evergreen Bootstrapper* (~2 MB) for the WebView2 runtime.
  It is a downloader, not the runtime itself — it fetches and installs the current
  runtime, so it never goes stale and does not need re-vendoring.
- **No longer shipped by default.** The release is now a self-contained
  `ParticleEditor.exe` + `d3dx9_43.dll` (the WebView2 loader is statically linked, so no
  `WebView2Loader.dll` ships either). When the WebView2 **runtime** — a machine component,
  present on Windows 11 and Windows 10 with Edge, absent on stripped/LTSC images — is
  missing, the WebView2 failure path in
  [`src/host/HostWindow.cpp`](../../src/host/HostWindow.cpp) opens the download page
  (`https://aka.ms/webview2`).
- **Why still vendored:** the bootstrapper is retained here for optional side-by-side use
  (dropped next to the exe it is still honored for a one-click install) and in case a
  future release chooses to bundle it again. It is a downloader, not the runtime itself, so
  it never goes stale.
- **How it was obtained:** downloaded 2026-07-25 from Microsoft's permalink
  <https://go.microsoft.com/fwlink/p/?LinkId=2124703> (the "Evergreen Bootstrapper"
  download on <https://developer.microsoft.com/microsoft-edge/webview2/>), then verified
  before committing:

  | Check | Result |
  |---|---|
  | Size | 1,691,856 bytes (1.61 MB) |
  | PE header | `MZ` — valid Windows executable |
  | Authenticode | **Valid** |
  | Signer | `CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US` |
  | SHA-256 | `0223FA1E8D5BD5E4344FB8734E60D088E79F262C0A24444D01F240BC996F04E5` |

  Re-vendoring is a **one-time manual step** and deliberately not scripted: a build script
  that downloads and then ships an executable is a supply-chain hazard. If you ever replace
  this file, verify the Authenticode signature again and update the table above — a vendored
  binary that ships to users should never be taken on trust.
- **Packaging:** [`scripts/package-release.ps1`](../../scripts/package-release.ps1) does
  **not** stage this file — the default release omits it. (It formerly treated it as a
  required source; that changed when the release became a self-contained exe.)
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
