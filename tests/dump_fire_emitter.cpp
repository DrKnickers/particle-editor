// T0 (§4 plan): dump the flipbook example's Fire emitter — every property and
// every track key — and DIFF it against a freshly-constructed default emitter,
// to pin the EXACT build target for the §4 build clip. Read-only, dev-box only.
//   usage: dump_fire_emitter.exe <path-to-flipbook.alo>
#include <windows.h>
#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include "files.h"
#include "exceptions.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// ~Emitter -> ParticleSystemInstance::RemoveEmitter is D3D-coupled; stub it
// (same as test_particle_system_io.cpp / test_alo_roundtrip.cpp).
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

using Emitter = ParticleSystem::Emitter;

static const char* interpName(int it) {
    switch (it) { case 0: return "LINEAR"; case 1: return "SMOOTH"; case 2: return "STEP"; default: return "UNKNOWN"; }
}
static const char* trackName(int i) {
    static const char* n[] = { "RED", "GREEN", "BLUE", "ALPHA", "SCALE", "INDEX", "ROT_SPEED" };
    return (i >= 0 && i < ParticleSystem::NUM_TRACKS) ? n[i] : "?";
}
static const char* blendName(unsigned long b) {
    switch (b) { case 0: return "NONE"; case 1: return "ADDITIVE"; case 2: return "TRANSPARENT"; case 3: return "INVERSE";
        case 4: return "DEPTH_ADDITIVE"; case 5: return "DEPTH_TRANSPARENT"; case 6: return "DEPTH_INVERSE"; case 7: return "DIFFUSE_TRANSPARENT"; default: return "?"; }
}

static void dumpTracks(const char* label, const Emitter& e) {
    printf("--- %s tracks ---\n", label);
    for (int t = 0; t < ParticleSystem::NUM_TRACKS; ++t) {
        const Emitter::Track& tr = *e.tracks[t];
        printf("  %-9s interp=%-6s keys=%zu:", trackName(t), interpName(tr.interpolation), tr.keys.size());
        for (const Emitter::Track::Key& k : tr.keys) printf("  (t=%.4f v=%.4f)", k.time, k.value);
        printf("\n");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: dump_fire_emitter <flipbook.alo>\n"); return 2; }
    std::wstring wpath;
    { const char* p = argv[1]; while (*p) wpath.push_back((wchar_t)(unsigned char)*p++); }

    PhysicalFile* f = nullptr;
    try { f = new PhysicalFile(wpath); } catch (...) { printf("FATAL: open failed\n"); return 2; }
    std::unique_ptr<ParticleSystem> ps;
    try { ps.reset(new ParticleSystem(f)); } catch (...) { printf("FATAL: parse failed\n"); f->Release(); return 2; }
    f->Release();

    const Emitter* fire = nullptr;
    printf("=== emitters in file ===\n");
    for (const Emitter* e : ps->getEmitters()) {
        printf("  [%zu] '%s'  tex='%s'\n", e->index, e->name.c_str(), e->colorTexture.c_str());
        if (e->name == "Fire") fire = e;
    }
    if (!fire) { printf("FATAL: no 'Fire' emitter\n"); return 2; }

    Emitter def;  // default via setDefaults()

    printf("\n=== FIRE properties  (DIFF = differs from a default emitter) ===\n");
#define PB(field) printf("  %-26s = %-14d %s\n", #field, (int)fire->field, (fire->field != def.field) ? "<== DIFF (default=" : ""); if (fire->field != def.field) printf("        (default %s = %d)\n", #field, (int)def.field)
#define PF(field) printf("  %-26s = %-14.5f %s\n", #field, (double)fire->field, (fire->field != def.field) ? "<== DIFF" : ""); if (fire->field != def.field) printf("        (default %s = %.5f)\n", #field, (double)def.field)
#define PU(field) printf("  %-26s = %-14lu %s\n", #field, (unsigned long)fire->field, (fire->field != def.field) ? "<== DIFF" : ""); if (fire->field != def.field) printf("        (default %s = %lu)\n", #field, (unsigned long)def.field)
#define PS(field) printf("  %-26s = '%s' %s\n", #field, fire->field.c_str(), (fire->field != def.field) ? "<== DIFF" : ""); if (fire->field != def.field) printf("        (default %s = '%s')\n", #field, def.field.c_str())

    PS(name); PS(colorTexture); PS(normalTexture);
    PB(useBursts); PU(nBursts); PU(nParticlesPerBurst); PU(nParticlesPerSecond);
    PF(lifetime); PF(randomLifetimePerc); PF(initialDelay); PF(burstDelay); PF(freezeTime); PF(skipTime);
    PF(gravity); PF(inwardSpeed); PF(inwardAcceleration); PF(bounciness);
    printf("  acceleration               = %.5f, %.5f, %.5f  %s\n", fire->acceleration[0], fire->acceleration[1], fire->acceleration[2],
        (fire->acceleration[0]!=def.acceleration[0]||fire->acceleration[1]!=def.acceleration[1]||fire->acceleration[2]!=def.acceleration[2]) ? "<== DIFF" : "");
    PF(randomScalePerc);
    PB(randomRotation); PB(randomRotationDirection); PF(randomRotationAverage); PF(randomRotationVariance);
    printf("  blendMode                  = %lu (%s) %s\n", fire->blendMode, blendName(fire->blendMode), (fire->blendMode!=def.blendMode)?"<== DIFF":"");
    if (fire->blendMode!=def.blendMode) printf("        (default blendMode = %lu (%s))\n", def.blendMode, blendName(def.blendMode));
    PU(textureSize);
    PB(hasTail); PF(tailSize); PB(isWorldOriented); PB(noDepthTest); PB(isHeatParticle); PB(isWeatherParticle);
    PB(affectedByWind); PB(objectSpaceAcceleration); PB(doColorAddGrayscale); PB(linkToSystem);
    PF(parentLinkStrength); PU(groundBehavior); PU(emitFromMesh); PF(emitFromMeshOffset);
    printf("  randomColors               = %.4f, %.4f, %.4f, %.4f\n", fire->randomColors[0], fire->randomColors[1], fire->randomColors[2], fire->randomColors[3]);

    printf("\n=== groups (position/speed random ranges) — FIRE then DEFAULT ===\n");
    for (int which = 0; which < 2; ++which) {
        const Emitter& src = which ? def : *fire;
        printf("  %s:\n", which ? "DEFAULT" : "FIRE");
        for (int g = 0; g < ParticleSystem::NUM_GROUPS; ++g) {
            const Emitter::Group& gr = src.groups[g];
            printf("    group[%d] type=%u sphereR=%.3f cylR=%.3f cylH=%.3f side=%.3f box=(%.2f,%.2f,%.2f)-(%.2f,%.2f,%.2f) val=(%.3f,%.3f,%.3f)\n",
                g, gr.type, gr.sphereRadius, gr.cylinderRadius, gr.cylinderHeight, gr.sideLength,
                gr.minX, gr.minY, gr.minZ, gr.maxX, gr.maxY, gr.maxZ, gr.valX, gr.valY, gr.valZ);
        }
    }

    printf("\n");
    dumpTracks("FIRE", *fire);
    printf("\n");
    dumpTracks("DEFAULT", def);
    return 0;
}
