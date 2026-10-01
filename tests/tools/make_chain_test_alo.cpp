// Chain-investigation v3 — author the in-game depth-3 test .alo with the
// editor's OWN data model instead of ad-hoc byte patches (the v2 trap).
//
// Loads vanilla P_S_ASSAULTCONC.ALO (6 emitters, all roots, no links),
// rewires it via the editor's validated reparentEmitter:
//
//   default(#2, W_SPARKLE1)  -> life ->  detail(#1, W_NemSmoke_Highlight)
//                                          -> life ->  Smoke(#0, w_smoke)
//   default(#3), default(#4), flash(#5)  = untouched controls
//
// Root choice fixes the v2 confound: the v2 chain hung off `flash` (tiny
// single burst) and gen-2 never fired ("sparkles only"). The `default`
// sparkle emitters demonstrably fire in-game (the user saw them), and all
// three textures are proven visible (in vanilla all six emitters are roots).
//
// Spawn rates on the chain are clamped to bound per-particle multiplication
// (the v1 crash was a depth-6 full-rate particle bomb, not a depth verdict).
//
// The tool then RELOADS its own output and asserts the chain round-tripped,
// so the file handed to the game is verified by the same loader the editor
// uses. Usage:
//   make_chain_test_alo.exe <vanilla.alo> <out.alo>

#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include "files.h"
#include <cstdio>
#include <string>

// Link stub (same rationale as test_emitter_reorder.cpp): ~Emitter references
// ParticleSystemInstance::RemoveEmitter, whose real body is D3D-coupled; no
// EmitterInstance is ever registered here, so a no-op keeps the link graph on
// the pure data-model TUs.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_failed = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  ok: %s\n", msg); } \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Emitter = ParticleSystem::Emitter;

static void printSpawn(const char* tag, const Emitter* e)
{
    std::printf("  %-10s name=%-10s bursts=%d nBursts=%lu perBurst=%lu perSec=%lu life=%.2f\n",
        tag, e->name.c_str(), (int)e->useBursts, e->nBursts,
        e->nParticlesPerBurst, e->nParticlesPerSecond, e->lifetime);
}

// Bound a chain emitter's spawn output so depth-3 per-particle multiplication
// stays in the hundreds, not millions.
static void clampSpawn(Emitter* e, unsigned long perSecCap, unsigned long perBurstCap)
{
    if (e->nParticlesPerSecond > perSecCap) e->nParticlesPerSecond = perSecCap;
    if (e->useBursts)
    {
        if (e->nParticlesPerBurst > perBurstCap) e->nParticlesPerBurst = perBurstCap;
        if (e->nBursts == 0 || e->nBursts > 3) e->nBursts = 3;  // 0 = infinite: cap it
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf("usage: make_chain_test_alo <vanilla.alo> <out.alo>\n");
        return 2;
    }
    const std::wstring inPath(argv[1], argv[1] + strlen(argv[1]));
    const std::wstring outPath(argv[2], argv[2] + strlen(argv[2]));

    // ---- Load vanilla (editor loader: parses, validates the graph). ----
    IFile* in = new PhysicalFile(inPath, PhysicalFile::READ);
    ParticleSystem ps(in);
    in->Release();

    std::vector<Emitter*>& em = ps.getEmitters();
    CHECK(em.size() == 6, "vanilla has 6 emitters");
    if (em.size() != 6) return 1;

    Emitter* smoke  = em[0];
    Emitter* detail = em[1];
    Emitter* root   = em[2];   // first 'default' sparkle emitter
    CHECK(smoke->name  == "Smoke",   "emitter 0 is Smoke");
    CHECK(detail->name == "detail",  "emitter 1 is detail");
    CHECK(root->name   == "default", "emitter 2 is default (chain root)");
    for (Emitter* e : em) CHECK(e->parent == NULL, "vanilla emitter is a root");

    std::printf("vanilla spawn params:\n");
    printSpawn("root",   root);
    printSpawn("detail", detail);
    printSpawn("smoke",  smoke);

    // ---- Rewire via the editor's validated op. ----
    CHECK(ps.reparentEmitter(detail, root,  true), "reparent detail under default (life)");
    CHECK(ps.reparentEmitter(smoke, detail, true), "reparent Smoke under detail (life)");

    // ---- Bound the combinatorics (v1 lesson). ----
    // Smoke's cap is higher than the others': its instances ride detail
    // particles that live only 0.08 s, so at a low rate gen-3 could emit ZERO
    // particles and read as a false "depth stops at 2". 30/sec ≈ 2.4 particles
    // per 0.08 s window — reliably visible, still bounded (~tens per volley).
    clampSpawn(root,   6,  4);
    clampSpawn(detail, 6,  4);
    clampSpawn(smoke,  30, 3);
    std::printf("clamped spawn params:\n");
    printSpawn("root",   root);
    printSpawn("detail", detail);
    printSpawn("smoke",  smoke);

    // ---- Write with the editor's writer. ----
    IFile* out = new PhysicalFile(outPath, PhysicalFile::WRITE);
    ps.write(out);
    out->Release();

    // ---- Round-trip: reload our own output, assert the chain survived. ----
    IFile* back = new PhysicalFile(outPath, PhysicalFile::READ);
    ParticleSystem rt(back);
    back->Release();

    std::vector<Emitter*>& r = rt.getEmitters();
    CHECK(r.size() == 6, "round-trip has 6 emitters");
    if (r.size() == 6)
    {
        CHECK(r[2]->spawnDuringLife == 1, "default.life -> detail");
        CHECK(r[1]->spawnDuringLife == 0, "detail.life -> Smoke");
        CHECK(r[1]->parent == r[2],       "detail.parent == default");
        CHECK(r[0]->parent == r[1],       "Smoke.parent == detail (depth 3)");
        CHECK(r[0]->spawnDuringLife == (size_t)-1 && r[0]->spawnOnDeath == (size_t)-1,
              "Smoke is a leaf");
        CHECK(r[3]->parent == NULL && r[4]->parent == NULL && r[5]->parent == NULL,
              "controls (default x2, flash) untouched roots");
        CHECK(r[2]->nParticlesPerSecond <= 6 && r[1]->nParticlesPerSecond <= 6
              && r[0]->nParticlesPerSecond <= 30, "clamps persisted");
    }

    std::printf("\n=== %s ===\n", g_failed == 0 ? "v3 chain file VERIFIED" : "FAILED");
    return g_failed == 0 ? 0 : 1;
}
