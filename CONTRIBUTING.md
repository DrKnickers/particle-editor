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
