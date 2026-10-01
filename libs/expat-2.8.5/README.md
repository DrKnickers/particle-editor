# Expat 2.8.5 (vendored)

The XML parser behind `src/xml.cpp`, built as a static UTF-16 (`wchar_t`)
library by `expatw_static.vcxproj` and linked into the editor and the
expat-linking unit tests. MIT licensed — see [`COPYING`](COPYING).

- **Upstream:** <https://github.com/libexpat/libexpat>, release `R_2_8_5`
  (2026-09-22).
- **Tarball:** <https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.xz>
- **SHA-256 (`expat-2.8.5.tar.xz`):**
  `1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182`

## What is here

| Path | Origin |
|---|---|
| `COPYING` | upstream, unmodified |
| `include/expat/expat.h`, `include/expat/expat_external.h` | upstream `lib/`, unmodified (public API; consumers `#include "expat/expat.h"`) |
| `lib/*` | upstream `lib/`, unmodified — only the files a Windows build compiles or includes |
| `expat_config.h` | **local** — MSVC expansion of upstream `expat_config.h.cmake` (the tarball's pre-generated `expat_config.h` is a Linux configure result) |
| `expatw_static.vcxproj` | **local** — upstream ships CMake only |

Hash-table salting uses `rand_s()` (`lib/random_rand_s.c`), never the CRT
`srand()`/`rand()` stream.

## Updating

Download and verify the new release tarball, replace `COPYING`,
`include/expat/` and `lib/` with the matching upstream files, re-check
`expat_config.h` against the new `expat_config.h.cmake` and the source list
against upstream `CMakeLists.txt` (`_EXPAT_C_SOURCES`), rename this folder to
the new version, and update every path that names it (`ParticleEditor.sln`,
`src/ParticleEditor.vcxproj`, `tests/build-native.mjs`,
`scripts/run-all-tests.mjs`).
