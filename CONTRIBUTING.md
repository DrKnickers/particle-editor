# Contributing

Thanks for the interest. This fork is maintained as a side project, so review can take days rather than hours — patience appreciated.

## Bug reports

Open an issue using the **Bug report** template. The most useful reports include:

- The editor version (Help → About: it'll say *"Particle Editor v0.2.0"* or whichever).
- Your OS / Windows version.
- The mod loaded at the time, if any.
- Exact reproduction steps — what was clicked / opened / edited, in what order.
- What you expected vs what happened.

If the editor crashed and produced a dialog with an exception trace, paste that verbatim — it points right at the function.

## Pull requests

The workflow is conventional:

1. **Fork → branch → commit → PR against `master`.** All work goes through PRs, including from maintainers.
2. **Build before opening.** *Debug | x64* and *Release | x64* must both compile clean. The public mirror CI runs these automatically once your PR is open; the private working repo skips the heavy C++ job, so local C++ builds remain required there.
3. **PR body uses the [PULL_REQUEST_TEMPLATE](.github/PULL_REQUEST_TEMPLATE.md) shape** — *Summary* + *Test plan checklist*. Match the existing PR shape; readers and maintainers rely on it.
4. **One feature per PR.** Bundle the docs update for that feature into the same PR. Don't mix unrelated changes — easier to review, easier to revert.

### Commit messages

Conventional Commits (`feat:` / `fix:` / `docs:` / `refactor:` / `chore:`) for the subject line. Body explains *why*, not *what* — the diff already shows what.

### Coding conventions

The codebase has been around since 2008 and inherits Mike.NL's GlyphX-era style. Match the surrounding code:

- Win32 + D3D9 + C++. No new dependencies without prior discussion.
- Plain `LTEXT` / `BUTTON` / `STATIC` controls in the `.rc`s. New custom controls go under `src/UI/`.
- Resource IDs are clustered by feature — `IDC_SPAWNER_*` in the 1300s, `IDC_BLOOM_*` in the 1400s, etc. Pick the next sequential ID in the right cluster.
- German (`.de.rc`) and English (`.en.rc`) resources must stay in sync. UTF-8 with BOM, no exceptions — the file encoding has historically been a source of mojibake bugs.

### What goes where in docs

- **[CHANGELOG.md](CHANGELOG.md)** — public-facing release history, updated only when a new version ships.
- **[DEVELOPMENT_LOG.md](DEVELOPMENT_LOG.md)** — per-PR engineering detail. Every non-trivial PR adds an entry with *What ships* / *How we tackled it* / *Issues encountered* — see existing entries for the shape. After adding the entry, run `scripts/build-devlog-index.ps1` to refresh the generated `## Index` at the top (don't hand-edit the index). It's a big file — to find past work, read the `## Index`, not the whole file.
- **[ROADMAP.md](ROADMAP.md)** — planned work, grouped by horizon. Items are named descriptively; the section is the tier, and a GitHub issue (`#NN`) gives a stable handle when one is needed.

The full project conventions live in [`CLAUDE.md`](CLAUDE.md) at the repo root — that's the authoritative spec for how Plans get written, how lessons get captured, and how the trust-but-verify rule applies. Worth a read before non-trivial work.

## Build — it takes TWO builds

The editor is a C++ host **plus** a React/WebView2 UI. Building the C++ `.sln` alone is **not** enough to run it.

1. **C++ host** — Visual Studio 2022 (Platform Toolset v143), x64, DirectX SDK June 2010 (for `d3dx9.h` / `d3dx9_43.lib`). Build the **`.sln`** (`msbuild ParticleEditor.sln /p:Configuration=Release /p:Platform=x64`), not the bare `src\ParticleEditor.vcxproj` (which looks for packages under `src\packages`). First time in a fresh worktree, restore NuGet once: `msbuild ParticleEditor.sln /t:Restore /p:RestorePackagesConfig=true`.
2. **Web UI bundle** — `cd web && pnpm install`, then `pnpm --filter ./apps/editor build` → produces `web/apps/editor/dist`. The host serves the React app from the `app.local` virtual host mapped to that `dist` (`src/host/HostWindow.cpp`).

**Symptom of skipping step 2:** the editor launches but the WebView shows **`ERR_NAME_NOT_RESOLVED` for `app.local`** — the virtual-host mapping has no `dist` to point at. Build the web bundle and **restart the exe** (the mapping is registered once at startup, so an in-window Refresh isn't enough). Drive the UI with the **Release** x64 build — Debug's attached console freezes the GUI.

To check/refresh the a11y goldens, run `pnpm --filter ./apps/editor a11y:drift` (exit 0 = clean, 2 = drift with goldens refreshed in the tree). A weekly scheduled task `a11y-goldens-drift-check` also runs it on a fresh `master` and opens a review PR on drift — inspect/reschedule/stop it with the `list_scheduled_tasks` / `update_scheduled_task` tools. Its prompt is [`docs/automation/a11y-drift-weekly-routine.md`](docs/automation/a11y-drift-weekly-routine.md); if you edit that doc, **re-register** the task (they don't auto-sync).

See [`README.md`](README.md) for runtime details and [`DEVELOPMENT_LOG.md`](DEVELOPMENT_LOG.md) → *Reference* → *Build Environment Requirements* for the canonical build matrix.

## Running the tests — one command

```
node scripts/run-all-tests.mjs
```

That is the whole verification recipe: it runs every automated layer in dependency order — web typecheck, Vitest (~1,230 tests), the web bundle build, script tests, mock-browser Playwright, all standalone C++ unit tests (`tests/test_*.cpp`, built via their `build_*.bat`s), both MSBuild configs, the native Playwright suite against the real `ParticleEditor.exe`, render-golden image comparisons (`scripts/render-goldens.mjs`; bless intentional rendering changes with `--update`), and the `--drive` pixel smoke with its assertion scenarios — and exits nonzero if anything fails, with a per-lane summary. Expect the full run to take minutes (it rebuilds everything on purpose; a green gate on stale binaries is worse than a slow one).

Useful flags: `--list` (lane names), `--lane vitest,cpp-unit` (subset), `--allow-missing drive-smoke` (downgrade a missing prereq to a visible SKIP on machines without the game install), `--skip-build` (unsafe, for iterating). Individual lanes remain available directly: `pnpm --filter ./apps/editor test` / `test:web` / `test:native`, and `node scripts/run-native-unit-tests.mjs --filter <name>` for a single C++ test.

## Static analysis

`cppcheck` runs over the C++ with no build or compile database
(`winget install Cppcheck.Cppcheck`):

```bash
cppcheck --enable=warning,style,performance,portability --std=c++17 \
  --quiet --suppress=missingIncludeSystem --suppress=missingInclude \
  -I src src/ChunkReader.cpp src/ChunkWriter.cpp src/AloModel.cpp
```

A 2026-06 pass flagged `ChunkReader::m_position` / `m_miniOffset` left
uninitialized in the constructor (`src/ChunkReader.cpp:140`) — worth fixing
next time that file is open. (`clang-tidy` / `clangd` are also available but
need a `compile_commands.json`, which the VS-generator MSBuild build doesn't
emit.)

## Code of conduct

Be decent. Disagreements about code are welcome — disagreements about people aren't.
