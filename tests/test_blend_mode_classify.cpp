// Unit test for ParticleSystem::blendModeIsAlphaGated — the single source of
// truth the Atlas-Frames picker's `blendAlphaGated` DTO field mirrors. Asserts
// every exposed blend-mode enum value maps to the expected alpha-gated class.
// Standalone x64 console exe (no engine runtime); build shape mirrors
// tests/build_test_emitter_reorder.bat but header-only (single TU).
#include "ParticleSystem.h"
#include <cstdio>

static int g_fail = 0;
static void check(int mode, bool expected) {
    const bool got = ParticleSystem::blendModeIsAlphaGated(mode);
    if (got != expected) {
        printf("  FAIL: blendModeIsAlphaGated(%d) = %s, expected %s\n",
               mode, got ? "true" : "false", expected ? "true" : "false");
        ++g_fail;
    }
}

int main() {
    // Alpha-gated <=> the mode's shader (engine_render.cpp ShaderNames[blendMode]) uses
    // SrcBlend = SRCALPHA: PrimAlpha(2), PrimDepthSpriteAlpha(5),
    // PrimDiffuseAlpha(7), PrimParticleBumpAlpha(11), PrimAlphaScanlines(13).
    check(ParticleSystem::BLEND_TRANSPARENT,          true);   // 2  PrimAlpha
    check(ParticleSystem::BLEND_DEPTH_TRANSPARENT,    true);   // 5  PrimDepthSpriteAlpha
    check(ParticleSystem::BLEND_DIFFUSE_TRANSPARENT,  true);   // 7  PrimDiffuseAlpha
    check(ParticleSystem::BLEND_BUMP,                 true);   // 11 PrimParticleBumpAlpha (SRCALPHA)
    check(ParticleSystem::BLEND_SCANLINES,            true);   // 13 PrimAlphaScanlines (SRCALPHA)
    // NOT alpha-gated: opaque / additive / modulate / decal-bump multiply / stencil / heat.
    check(ParticleSystem::BLEND_NONE,                 false);  // 0  PrimOpaque
    check(ParticleSystem::BLEND_ADDITIVE,             false);  // 1  PrimAdditive
    check(ParticleSystem::BLEND_INVERSE,              false);  // 3  PrimModulate
    check(ParticleSystem::BLEND_DEPTH_ADDITIVE,       false);  // 4  PrimDepthSpriteAdditive
    check(ParticleSystem::BLEND_DEPTH_INVERSE,        false);  // 6  PrimDepthSpriteModulate
    check(ParticleSystem::BLEND_DECAL_BUMP,           false);  // 12 PrimDecalBumpAlpha (DESTCOLOR/SRCCOLOR multiply — NOT alpha-gated)
    check(ParticleSystem::BLEND_STENCIL_DARKEN,       false);  // 8  StencilDarken
    check(ParticleSystem::BLEND_STENCIL_DARKEN_BLUR,  false);  // 9  StencilDarkenFinalBlur
    check(ParticleSystem::BLEND_HEAT,                 false);  // 10 PrimHeat — DX8 technique is SRCALPHA, but heat is unexposed + distortion-only; kept false (conservative: never wrongly dims)
    check(999,                                        false);  // unknown -> default

    if (g_fail == 0) { printf("blend-mode classify: ALL PASS\n"); return 0; }
    printf("blend-mode classify: %d FAILED\n", g_fail);
    return 1;
}
