# Third-party notices

The Particle Editor's own code is under [`LICENSE`](LICENSE). It also ships or builds with the components below, each under its own license.

## Shipped in the editor

| Component | Where | License |
|---|---|---|
| [Expat](https://libexpat.github.io/) 2.8.5 (XML parser) | `libs/expat-2.8.5/`, compiled into `ParticleEditor.exe` | MIT — [`libs/expat-2.8.5/COPYING`](libs/expat-2.8.5/COPYING) |
| [nlohmann/json](https://github.com/nlohmann/json) | `src/host/third_party/nlohmann/json.hpp`, compiled into `ParticleEditor.exe` | MIT — SPDX header in the file |
| [Inter](https://github.com/rsms/inter) (UI typeface) | `web/apps/editor/public/fonts/inter/`, embedded in `ParticleEditor.exe` | SIL Open Font License 1.1 — [`licenses/OFL-Inter.txt`](licenses/OFL-Inter.txt) |
| Microsoft WebView2 SDK (static loader) | NuGet `Microsoft.Web.WebView2`, linked into `ParticleEditor.exe` | Microsoft WebView2 SDK license, shipped in the NuGet package |
| `d3dx9_43.dll` (D3DX9, DirectX End-User Runtime June 2010) | `libs/redist/`, shipped beside the exe in release zips | Microsoft DirectX redistributable terms — see [`libs/redist/README.md`](libs/redist/README.md) |
| Web UI dependencies (React, Radix UI, Zustand, lucide-react, and others) | bundled into the embedded web UI | Each package's own license (MIT or similar); `pnpm licenses list` in `web/` prints them |

## Used by the project website (`site/`)

| Component | Where | License |
|---|---|---|
| [IBM Plex Mono](https://github.com/IBM/plex) | `site/fonts/` | SIL Open Font License 1.1 — [`site/fonts/OFL-IBMPlexMono.txt`](site/fonts/OFL-IBMPlexMono.txt) |
| [Schibsted Grotesk](https://github.com/schibsted/schibsted-grotesk) | `site/fonts/` | SIL Open Font License 1.1 — [`site/fonts/OFL-SchibstedGrotesk.txt`](site/fonts/OFL-SchibstedGrotesk.txt) |

The license texts in [`licenses/`](licenses/) are copied verbatim from each font's upstream repository.
