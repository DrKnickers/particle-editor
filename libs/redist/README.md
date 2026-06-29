# Vendored redistributables

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
