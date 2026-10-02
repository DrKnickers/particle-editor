# Contributing

Thanks for the interest. This fork is a side project, so review can take days rather than hours — patience appreciated.

## Bug reports

Open an issue using the **Bug report** template. The most useful reports include:

- The editor version (Help → About: it'll say *"Particle Editor v0.4.2"* or similar).
- Your OS / Windows version.
- The mod loaded at the time, if any.
- Exact reproduction steps — what was clicked / opened / edited, in what order.
- What you expected vs what happened.

If the editor crashed and produced a dialog with an exception trace, paste that verbatim — it points right at the function.

## Pull requests

The workflow is conventional:

1. **Fork → branch → commit → PR against `master`.** All work goes through PRs, including from maintainers.
2. **Build before opening.** *Debug | x64* and *Release | x64* must both compile clean. The public repository's CI runs these automatically once your PR is open.
3. **PR body uses the established shape** — *Summary* + *Test plan checklist*. Match the existing PRs; readers and maintainers rely on it.
4. **One feature per PR.** Bundle the docs update for that feature into the same PR. Don't mix unrelated changes — easier to review, easier to revert.

### How an accepted PR lands

The public `master` is published from the maintainer's working repository, so a PR is not merged on GitHub directly — a merge there would be overwritten by the next publish. Instead:

1. The PR is reviewed and discussed here as usual.
2. Once accepted, the change is applied to the working repository with credit to you, and goes through the full test gate there.
3. It reaches public `master` with the next publish, and the PR is closed with a pointer to the commit that carries it.

### Commit messages

Conventional Commits (`feat:` / `fix:` / `docs:` / `refactor:` / `chore:`) for the subject line. Body explains *why*, not *what* — the diff already shows what.

### Coding conventions

The codebase has been around since 2008 and inherits Mike.NL's GlyphX-era style. Match the surrounding code:

- Win32 + D3D9 + C++17 for the host, React + TypeScript for the UI. No new dependencies without prior discussion.
- **Where logic goes.** Decision logic that needs no window, device, or WebView goes in a small header under `src/` (for example `SpawnSchedule.h`, tested by `tests/test_spawn_schedule.cpp`) with its own `tests/test_<name>.cpp`, listed in `tests/native-tests.json` so `node scripts/run-native-unit-tests.mjs` builds and runs it. The host code in `src/host/` then calls it. Keep those headers free of Win32/D3D types so the test builds stay fast.
- **Layering.** `src/host/` is the WebView2 host and bridge; the rest of `src/` is the particle engine, file formats, and policy headers. Some core files still include host headers: `main.cpp` starts the host and its capture modes, the engine uses the compositor and module-path helpers, and a few files borrow the string-conversion, registry, perf-trace, and image-encoding helpers. `src/UI/` holds the texture palette's data and thumbnail code rather than UI, since the legacy Win32 UI is gone. Don't add new core→host includes.
- `ParticleEditor.rc` is the hand-authored application resource source (version info, icon, bitmaps, shaders, skydome textures, strings — no dialogs); `EmbeddedWebAssets.rc` is generated. Keep `.rc` files UTF-8 with BOM, no exceptions — the encoding has historically been a source of mojibake bugs. New resource IDs go in `src/Resources/resource.h`; take the next free number and bump `_APS_NEXT_RESOURCE_VALUE`.

### What goes where in docs

- **[CHANGELOG.md](CHANGELOG.md)** — public-facing release history. A feature PR adds one user-facing bullet under `## [Unreleased]` when it lands (no bullet for non-user-facing changes — docs, CI, internal refactors); cutting a release rolls that section up. Follow the existing bullets: present tense, the user's point of view, no trailing period, ending with the PR link.

## Start here

See [How the pieces fit](src/README.md#how-the-pieces-fit) for a picture of
the interface, host, engine and file paths.

- **Use the editor.** Start with the [download instructions](README.md#download)
  and the [user guide](https://drknickers.github.io/particle-editor/guide/).
- **Change the interface.** You need Node 22 or newer and the pnpm version
  pinned in [web/package.json](web/package.json). From the repository root, run:

  ```bash
  cd web
  pnpm install
  pnpm --filter ./apps/editor dev
  ```

  Open http://localhost:5174. Try changing the `Skip time:` label in
  [BasicTab.tsx](web/apps/editor/src/screens/property-tabs/BasicTab.tsx), then save and
  look for the updated label in the browser. The
  [dev script](web/apps/editor/package.json) starts Vite. Its
  [settings](web/apps/editor/vite.config.ts) require port 5174 to be free.
  The browser uses the [mock bridge](web/apps/editor/src/bridge/index.ts).
  This is a mock editor: it does not render particles or read and write native
  files. You can work on the interface without the Windows build tools.

  Guide edits have a separate route in [site/README.md](site/README.md#guide-markdown--committed-html).
  They need only Node. From the repository root, run
  `node scripts/build-guide.mjs` to build the guide, then
  `node scripts/build-guide.mjs --check` to check for stale pages and broken
  guide links. Vite does not serve the guide.
- **Change native behaviour.** Follow the [two-build instructions](#build--it-takes-two-builds)
  below. Build the web interface first, then the Windows host.

- **Find the code.** Use the [native source map](src/README.md) for file formats,
  simulation and request handlers, or the [web source map](web/apps/editor/README.md)
  for panels, state owners and a worked property edit.
  For a new emitter setting, follow [Adding a property field](web/apps/editor/README.md#adding-a-property-field)
  through the schema, browser default, host, saved file and tests.

## Words this project uses

| Word | Meaning |
|---|---|
| Host | The Windows program. It opens the window, runs the particle engine and holds the web interface. |
| Bridge | The connection that lets the web interface ask the host to do something and receive its answer. |
| Request kind | The name of an action sent over the bridge, such as `emitters/set-track-key`. Search for that name to find its handler. |
| DTO | Short for "data transfer object". A group of values sent over the bridge, such as an emitter's properties. |
| Mock | A stand-in for the host. It supplies sample data so the interface can run in a browser. |
| Golden | A saved picture or description of the interface. Tests compare new output with it to catch changes. |
| Lane | One group of checks in the test runner, such as `vitest` or `cpp-unit`. |
| Policy header | A small C++ header that holds a rule without needing a window or graphics device. It has its own test. |
| Legacy | Mike.NL's original Win32 editor. This editor preserves its behaviour. |

| User term | Name in code | Meaning and source |
|---|---|---|
| Effect | `ParticleSystem` | The definition saved in a `.alo` file. It holds emitters, not running particles. See [ParticleSystem.h](src/ParticleSystem.h) and `ParticleSystem::write` in [ParticleSystemSerialization.cpp](src/ParticleSystemSerialization.cpp). The C++ class named `Effect` instead wraps a graphics shader, a program that draws a surface; see [Effect.h](src/Effect.h). |
| Instance | `ParticleSystemInstance` | One running copy of an effect. It owns running emitters and refers to the definition. It is not saved in `.alo`. See [ParticleSystemInstance.h](src/ParticleSystemInstance.h), `m_system`, `m_emitters` and `SetMaxLifetime` (seconds; zero means no spawner lifetime cap). |
| Emitter number or ID | `Emitter` field `index`, bridge `id`; `stableId` is different | `index` is the zero-based position in the emitter list. Requests use it as `id`; moves and deletions can change it. `stableId` keeps a row's identity across moves. It is not saved and is issued again when loading or undo rebuilds emitters. See [ParticleSystem.h](src/ParticleSystem.h), `stableId`, and [ParticleSystemSerialization.cpp](src/ParticleSystemSerialization.cpp), `writeProperties`: the positional index is written, but its saved value is ignored when reading. |
| Index curve | Track name `index`, `TRACK_INDEX` | Chooses a frame in a texture atlas, an image split into cells. This is separate from the emitter's list position. The engine rounds the sampled value down before choosing a cell. The spinner allows 0 to 1,000,000 and nudges by 1; typed fractions are allowed. See [EmitterInstance.cpp](src/EmitterInstance.cpp), `texIndex`, and [CurveEditorPanel.tsx](web/apps/editor/src/screens/curve-editor/CurveEditorPanel.tsx), `spinnerBoundsForTrack`. Track values are saved by `writeTracks` in [ParticleSystemSerialization.cpp](src/ParticleSystemSerialization.cpp). |
| Rotation curve | Track name `rotationSpeed`, `TRACK_ROTATION_SPEED` | The visible label is Rotation. With random rotation off, the engine integrates this curve over elapsed seconds to get turns, then converts turns to radians. Values therefore describe turns per second, not degrees. The spinner allows -1,000,000 to 1,000,000, stepping by 0.1. See `CHANNELS` and `spinnerBoundsForTrack` in [CurveEditorPanel.tsx](web/apps/editor/src/screens/curve-editor/CurveEditorPanel.tsx), and `IntegrateTrack` in [EmitterInstance.cpp](src/EmitterInstance.cpp). With random rotation on, `writeTracks` in [ParticleSystemSerialization.cpp](src/ParticleSystemSerialization.cpp) writes the random rotation average instead of the curve. |
| Curve time | `TrackKey` field `time` | 0 is birth and 100 is the end of that particle's lifetime, not 100 seconds. The engine computes percentage time in `UpdateParticle` in [EmitterInstance.cpp](src/EmitterInstance.cpp). `writeTracks` in [ParticleSystemSerialization.cpp](src/ParticleSystemSerialization.cpp) divides interior key times by 100 for storage; the [schema](web/packages/bridge-schema/src/index.ts), `TrackKey`, carries the 0 to 100 form. |

## Which check for which change

Start with the smallest useful check while you edit. Before a PR, run the
listed lanes from the repository root with
`node scripts/run-all-tests.mjs --lane <names>`. Replace `<names>` with the
comma-separated names from the table. Selected lanes do not add their build
steps for you. Keep `web-build` before `scripts` and the Windows builds.
Every code PR builds both Debug and Release.

| Change | Smallest useful check | Lanes before a PR | What these checks do not prove |
|---|---|---|---|
| Label or style | View the change in the browser; run `pnpm --filter ./apps/editor build` from `web/`. | `lint,vitest,web-build,playwright-web,msbuild-debug,playwright-native,msbuild-release` | The mock cannot prove particle rendering or native file operations. Check the real editor for those. |
| Property behaviour | Run `pnpm --filter ./apps/editor test` from `web/` for the interface and mock tests. | `lint,vitest,web-build,playwright-web,msbuild-debug,playwright-native,msbuild-release` | Mock results do not prove the host accepts a value or saves it correctly. Native tests cover the real host. |
| Native logic | Run `node scripts/run-native-unit-tests.mjs --filter <name>` for the rule you changed. | `web-build,scripts,cpp-unit,msbuild-debug,cpp-unit-exe,playwright-native,msbuild-release` | A small C++ test does not prove that the window, bridge or graphics device works. |
| Rendering | Compare a capture from the real editor with its saved golden. | Full gate: `node scripts/run-all-tests.mjs` | Saved scenes do not cover every effect or graphics device. Inspect the changed effect in the real editor too. |
| Guide prose | Run `node scripts/build-guide.mjs`, then `node scripts/build-guide.mjs --check`. | `web-build,scripts,site` | The builder checks guide links and stale pages. It does not prove that the instructions are correct or that the page looks right. Read the built page. |

Windows builds and native browser, rendering, recording and drive checks need
the Windows desktop and tools described below. Some also need the game files.
If you cannot run a check, say which one is missing in the PR.

Some tests read source files as text. If you move code, check these as well as
the tests for its behaviour:

- [contract-drift.test.ts](web/apps/editor/src/bridge/__tests__/contract-drift.test.ts)
  checks the bridge contract.
- [vcxproj-parity.test.mjs](scripts/vcxproj-parity.test.mjs) checks that native
  files are listed in both Visual Studio project files.
- [bridge-emit-chokepoint.test.mjs](scripts/bridge-emit-chokepoint.test.mjs)
  checks how bridge messages are sent.
- [test_capture_golden_profile.cpp](tests/test_capture_golden_profile.cpp),
  [test_device_state.cpp](tests/test_device_state.cpp) and
  [test_startup_callback_guard.cpp](tests/test_startup_callback_guard.cpp)
  read the host window files. The device test also reads engine, asset and
  manager files.
- [test_bounce_catch_up.cpp](tests/test_bounce_catch_up.cpp) reads
  [EmitterInstance.cpp](src/EmitterInstance.cpp).

[doc-paths.test.mjs](scripts/doc-paths.test.mjs) checks local links in the
contributor documents. Run it alone with `node --test scripts/doc-paths.test.mjs`.
[doc-search-text.test.mjs](scripts/doc-search-text.test.mjs) also checks search
text in the source maps and file contents comments. Run it alone with
`node --test scripts/doc-search-text.test.mjs`. These checks do not prove that
the explanations are correct.
Guide links use the guide builder's own check.

## Build — it takes TWO builds

The editor is a C++ host **plus** a React/WebView2 UI, and the C++ build **embeds** the UI — so the two builds run **in order: web first, then C++.**

1. **Web UI bundle (first)** — `cd web && pnpm install`, then `pnpm --filter ./apps/editor build` → produces `web/apps/editor/dist`.
2. **C++ host** — Visual Studio 2022 (Platform Toolset v143), x64, DirectX SDK June 2010 (for `d3dx9.h` / `d3dx9_43.lib`). Build the **`.sln`** (`msbuild ParticleEditor.sln /p:Configuration=Release /p:Platform=x64`), not the bare `src\ParticleEditor.vcxproj` (which links `src\x64\<Config>\ParticleEditor.exe` instead of the `x64\<Config>\` exe the tests launch). First time in a fresh worktree, restore NuGet once: `msbuild ParticleEditor.sln /t:Restore /p:RestorePackagesConfig=true` (the restore is order-independent; only the Build needs `dist`). The build compiles `web/apps/editor/dist` into the exe as RCDATA (`scripts/embed-web-dist.mjs`), so the editor ships as a **single self-contained exe** with no separate `web/` folder; `src/host/HostWindow.cpp` serves it on the `app.local` virtual origin via a `WebResourceRequested` handler.

**Symptom of skipping step 1:** the C++ Build fails at the `GenerateEmbeddedWebAssets` step with `web\apps\editor\dist\index.html not found — build the editor web bundle first`. (Before the bundle was embedded, this instead surfaced at runtime as `ERR_NAME_NOT_RESOLVED` for `app.local`.) Drive the UI with the **Release** x64 build — Debug's attached console freezes the GUI.

To check/refresh the a11y goldens, run `pnpm --filter ./apps/editor a11y:drift` (exit 0 = clean, 2 = drift with goldens refreshed in the tree).

See [`README.md`](README.md) for runtime details.

## Running the tests — one command

```
node scripts/run-all-tests.mjs
```

That is the whole verification recipe: it runs every automated layer in dependency order — web typecheck, Vitest, the web bundle build, script tests, mock-browser Playwright, static-site Playwright, all standalone C++ unit tests (`tests/test_*.cpp`, built in parallel from `tests/native-tests.json`), both MSBuild configs, the native Playwright suite against the real `ParticleEditor.exe`, render-golden image comparisons (`scripts/render-goldens.mjs`; bless intentional rendering changes with `--update`), the `--record` frame smoke, and the `--drive` pixel smoke with its assertion scenarios — and exits nonzero if anything fails, with a per-lane summary. Expect the full run to take minutes (it rebuilds everything on purpose; a green gate on stale binaries is worse than a slow one).

Useful flags: `--list` (lane names), `--lane vitest,cpp-unit` (subset), `--allow-missing drive-smoke` (downgrade a missing prereq to a visible SKIP on machines without the game install), `--skip-build` (unsafe, for iterating). Individual lanes remain available directly: `pnpm --filter ./apps/editor test` / `test:web` / `test:native`, and `node scripts/run-native-unit-tests.mjs --filter <name>` for a single C++ test.

**Not on Windows, or no game install?** The web half runs anywhere Node 22+ and pnpm 10+ do (see `web/package.json`); the public CI runs these on Ubuntu. From `web/`:

```
pnpm install
pnpm --filter ./apps/editor lint
pnpm --filter ./apps/editor build
pnpm --filter ./apps/editor test
pnpm --filter ./apps/editor test:scripts
pnpm --filter ./apps/editor exec playwright install chromium
pnpm --filter ./apps/editor test:web
```

The C++ builds and the native lanes need Windows, Visual Studio, and the DirectX SDK, and some also need the game install; CI builds both C++ configurations on every PR.

## Static analysis

`cppcheck` runs over the C++ with no build or compile database
(`winget install Cppcheck.Cppcheck`):

```bash
cppcheck --enable=warning,style,performance,portability --std=c++17 \
  --quiet --suppress=missingIncludeSystem --suppress=missingInclude \
  -I src src/ChunkReader.cpp src/ChunkWriter.cpp src/AloModel.cpp
```

(`clang-tidy` / `clangd` are also available but need a `compile_commands.json`,
which the VS-generator MSBuild build doesn't emit.)

## Code of conduct

Be decent. Disagreements about code are welcome — disagreements about people aren't.
