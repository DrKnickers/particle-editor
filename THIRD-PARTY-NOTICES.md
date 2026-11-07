# Third-party notices

The Particle Editor's own code is under [`LICENSE`](LICENSE). It also ships or builds with the components below, each under its own license.

## Shipped in the editor

| Component | Where | License |
|---|---|---|
| [Expat](https://libexpat.github.io/) 2.8.5 (XML parser) | `libs/expat-2.8.5/`, compiled into `ParticleEditor.exe` | MIT — [`licenses/Expat.txt`](licenses/Expat.txt), copied from `libs/expat-2.8.5/COPYING` |
| SipHash-2-4 (included in Expat) | `libs/expat-2.8.5/lib/siphash.h`, compiled into `ParticleEditor.exe`; derived by William Ahern from Jean-Philippe Aumasson and Daniel J. Bernstein's reference implementation | CC0 1.0 — [`licenses/CC0-1.0.txt`](licenses/CC0-1.0.txt) |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | `src/host/third_party/nlohmann/json.hpp`, compiled into `ParticleEditor.exe` | MIT — [`licenses/nlohmann-json.txt`](licenses/nlohmann-json.txt) |
| [Inter](https://github.com/rsms/inter) (UI typeface) | `web/apps/editor/public/fonts/inter/`, embedded in `ParticleEditor.exe` | SIL Open Font License 1.1 — [`licenses/OFL-Inter.txt`](licenses/OFL-Inter.txt) |
| Microsoft WebView2 SDK 1.0.3967.48 (static loader) | NuGet `Microsoft.Web.WebView2`, linked into `ParticleEditor.exe` | BSD-style terms from the SDK package — [`licenses/WebView2-SDK.txt`](licenses/WebView2-SDK.txt) and its [`third-party notices`](licenses/WebView2-SDK-NOTICE.txt) |
| `d3dx9_43.dll` (D3DX9, DirectX End-User Runtime June 2010) | `libs/redist/`, shipped beside the exe in release zips | Microsoft terms — [`licenses/DirectX-End-User-Runtime.txt`](licenses/DirectX-End-User-Runtime.txt) and [`licenses/DirectX-SDK.txt`](licenses/DirectX-SDK.txt) |
| Web UI dependencies (React, Radix UI, Zustand, lucide-react, Floating UI, and their runtime dependencies), clsx, tailwind-merge, Tailwind CSS and Vite's preload helper | bundled into the embedded web UI | MIT, ISC, 0BSD and included component notices — full texts and package versions in [`licenses/Web-UI.txt`](licenses/Web-UI.txt) |

## Used by the project website (`site/`)

| Component | Where | License |
|---|---|---|
| [IBM Plex Mono](https://github.com/IBM/plex) | `site/fonts/` | SIL Open Font License 1.1 — [`site/fonts/OFL-IBMPlexMono.txt`](site/fonts/OFL-IBMPlexMono.txt) |
| [Schibsted Grotesk](https://github.com/schibsted/schibsted-grotesk) | `site/fonts/` | SIL Open Font License 1.1 — [`site/fonts/OFL-SchibstedGrotesk.txt`](site/fonts/OFL-SchibstedGrotesk.txt) |

The release zip includes `LICENSE`, this notice and the full [`licenses/`](licenses/) folder at
its root. License texts are copied from the corresponding upstream source, installed package
or SDK. The engine-host package also includes its manifest-listed `README.txt`; it does not
bundle the user's EAW Terrain Editor or game data, which setup reads from the user's own install.
