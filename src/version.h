#pragma once
// ---------------------------------------------------------------------------
// Canonical application version — the single source of truth.
//
// Every place that displays or embeds the editor's version reads from here:
//   - src/ParticleEditor.rc  (binary VS_VERSION_INFO: file/product version)
//   - src/main.cpp           (legacy Win32 "About" dialog)
//   - web/apps/editor/app-version.ts  (shared parser, regex on PE_VERSION_STR)
//       consumed by vite.config.ts   (default React UI "About", build-time)
//       and        vitest.config.ts  (test-time mirror of that inject)
//
// To bump the version, edit the four lines below ONLY. Follow SemVer
// (MAJOR.MINOR.PATCH) — see VERSIONING.md for the bump rules and the 1.0.0
// trigger. PE_VERSION_STR must match the three integers above it.
//
// This is the fork's own version line (0.x). The "1.5" historically shown
// here was upstream's number (Mike Lankamp's GlyphX Particle Editor); that
// lineage now lives as a credit line in the About dialogs, not as the
// version. See git tags v0.1.0 / v0.2.0 for the release history.
// ---------------------------------------------------------------------------

#define PE_VERSION_MAJOR 0
#define PE_VERSION_MINOR 3
#define PE_VERSION_PATCH 0
#define PE_VERSION_STR   "0.3.0"
