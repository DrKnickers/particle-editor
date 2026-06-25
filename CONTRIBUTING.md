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
2. **Build before opening.** *Debug | x64* and *Release | x64* must both compile clean. CI runs these automatically once your PR is open.
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
- **[DEVELOPMENT_LOG.md](DEVELOPMENT_LOG.md)** — per-PR engineering detail. Every non-trivial PR adds an entry with *What ships* / *How we tackled it* / *Issues encountered* — see existing entries for the shape.
- **[ROADMAP.md](ROADMAP.md)** — planned work, grouped by horizon. Items are named descriptively; the section is the tier, and a GitHub issue (`#NN`) gives a stable handle when one is needed.

The full project conventions live in [`CLAUDE.md`](CLAUDE.md) at the repo root — that's the authoritative spec for how Plans get written, how lessons get captured, and how the trust-but-verify rule applies. Worth a read before non-trivial work.

## Build

Visual Studio 2022 (Platform Toolset v143), x64, DirectX SDK June 2010 (for `d3dx9.h` / `d3dx9_43.lib`). Open `ParticleEditor.sln`, pick a configuration, build. No package manager needed.

See [`README.md`](README.md) for runtime details and [`DEVELOPMENT_LOG.md`](DEVELOPMENT_LOG.md) → *Reference* → *Build Environment Requirements* for the canonical build matrix.

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
