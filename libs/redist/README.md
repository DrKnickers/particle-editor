# Vendored redistributables

## `MicrosoftEdgeWebview2Setup.exe` — removed

Microsoft's *Evergreen Bootstrapper* (~1.6 MB) for the WebView2 runtime was vendored here
to be bundled in the release zip. It is **no longer vendored**: the release is now a
self-contained `ParticleEditor.exe` + `d3dx9_43.dll` (the WebView2 loader is statically
linked, so no `WebView2Loader.dll` ships either), and the bootstrapper is not bundled.
When the WebView2 **runtime** — a machine component, present on Windows 11 and Windows 10
with Edge, absent on stripped/LTSC images — is missing, the WebView2 failure path in
[`src/host/HostWindow.cpp`](../../src/host/HostWindow.cpp) opens the download page
(`https://aka.ms/webview2`); a bootstrapper a user manually places beside the exe is still
honored.

To re-vendor it (if a future release chooses to bundle the installer again): download the
"Evergreen Bootstrapper" from Microsoft's permalink
<https://go.microsoft.com/fwlink/p/?LinkId=2124703>, **verify its Authenticode signature**
(a build script that downloads and then ships an executable is a supply-chain hazard, so
re-vendoring is a deliberate one-time manual step — the last vendored copy was
1,691,856 bytes, SHA-256 `0223FA1E8D5BD5E4344FB8734E60D088E79F262C0A24444D01F240BC996F04E5`,
signed `CN=Microsoft Corporation`), and re-add the staging in
[`scripts/package-release.ps1`](../../scripts/package-release.ps1). Redistributable under
the Microsoft Edge WebView2 distribution terms.

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
