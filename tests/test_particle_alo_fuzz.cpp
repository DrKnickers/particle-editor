// Chunk-reader hardening + malformed-input fuzz for the PARTICLE .alo parser
// (ChunkReader + ParticleSystem(IFile*)), the counterpart of test_alo_fuzz
// (which covers only the static-mesh decoder). Audit 2026-09-30 H2.
//
// Four parts, all on byte images synthesized in-test (no game data):
//
//  (a) VALID CORPUS ROUND-TRIP. Systems built through the public API are
//      serialized, reloaded, and serialized again; the two images must be
//      byte-identical.
//
//  (b) CHUNKREADER EDGES. ChunkType is a signed long, so an on-disk type
//      0xFFFFFFFF used to equal the -1 end-of-chunk sentinel (returned after
//      the reader had already descended); next()/nextMini()/skip() past the
//      end of the file walked m_curDepth below zero and indexed m_offsets[-1];
//      nextMini() inside a CONTAINER chunk parsed container bytes as
//      mini-chunks. All of those were guarded only by assert(), so the
//      Release build had no guard at all. Each must now throw BadFileException.
//
//  (c) TOLERANT TAIL. The system-body loop skips unknown chunks. An unknown
//      DATA chunk used to be skipped twice (once by the loop, again by the next
//      next()), which popped out of the 0x0900 root and lost the 0x0002
//      leaveParticles chunk after it.
//
//  (d) SEEDED MUTATION FUZZ + TRUNCATION SWEEP over the corpus: every mutant
//      must load or fail with the documented BadFileException / ReadException,
//      never another exception and never a crash; a mutant that loads must
//      serialize again.
//
// Built Debug AND Release (/O2 /DNDEBUG) -- see the test_particle_alo_fuzz
// entry in tests/native-tests.json -- so the shipped no-assert parser is the one
// being fuzzed.

#include "effect/ParticleSystem.h"
#include "simulation/ParticleSystemInstance.h"
#include "effect/ChunkFile.h"
#include "common/files.h"
#include "common/exceptions.h"
#include "effect/LinkGroup.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>   // getenv / strtol for PARTICLE_ALO_FUZZ_ROUNDS
#include <cstring>
#include <string>
#include <vector>

// Link stub: ~Emitter calls ParticleSystemInstance::RemoveEmitter, whose real
// body is D3D-coupled. These tests never register an EmitterInstance, so a no-op
// keeps the link graph on the pure data-model TUs (mirrors test_alo_roundtrip).
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Emitter = ParticleSystem::Emitter;
typedef std::vector<unsigned char> Bytes;

// ---- byte helpers ------------------------------------------------------------
static void u32le(Bytes& b, uint32_t v)
{
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
    b.push_back((unsigned char)((v >> 16) & 0xFF));
    b.push_back((unsigned char)((v >> 24) & 0xFF));
}

static void putU32(Bytes& b, size_t at, uint32_t v)
{
    b[at + 0] = (unsigned char)(v & 0xFF);
    b[at + 1] = (unsigned char)((v >> 8) & 0xFF);
    b[at + 2] = (unsigned char)((v >> 16) & 0xFF);
    b[at + 3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t getU32(const Bytes& b, size_t at)
{
    return (uint32_t)b[at + 0]
         | ((uint32_t)b[at + 1] << 8)
         | ((uint32_t)b[at + 2] << 16)
         | ((uint32_t)b[at + 3] << 24);
}

static MemoryFile* fileOf(const Bytes& image)
{
    MemoryFile* f = new MemoryFile();   // rc=1
    if (!image.empty()) f->write(image.data(), (unsigned long)image.size());
    f->seek(0);
    return f;
}

static Bytes serialize(ParticleSystem& ps)
{
    MemoryFile* f = new MemoryFile();   // rc=1
    ps.write(f);
    f->seek(0);
    Bytes out(f->size());
    if (!out.empty()) f->read(out.data(), (unsigned long)out.size());
    f->Release();
    return out;
}

// ---- outcome classification ---------------------------------------------------
enum Outcome
{
    LOADED_OK,      // parsed cleanly and re-serialized
    REJECTED,       // documented error path (BadFile/WrongFile/Read)
    BAD_EXCEPTION,  // any other exception type escaped
};

static Outcome load(const Bytes& image, ParticleSystem** out = NULL)
{
    MemoryFile* f = fileOf(image);
    Outcome result;
    try
    {
        ParticleSystem* ps = new ParticleSystem(f);
        try
        {
            (void)serialize(*ps);   // a loaded system must be writable again
        }
        catch (...)
        {
            delete ps;
            throw;
        }
        if (out) *out = ps; else delete ps;
        result = LOADED_OK;
    }
    catch (BadFileException&) { result = REJECTED; }   // incl. WrongFileException
    catch (ReadException&)    { result = REJECTED; }
    catch (...)               { result = BAD_EXCEPTION; }
    f->Release();
    return result;
}

// ---- the synthesized corpus ---------------------------------------------------
// Un-share the colour tracks setDefaults aliases so the corpus is
// deterministic.
static void pinFields(Emitter* e)
{
    for (int t = 0; t < ParticleSystem::NUM_TRACKS; ++t)
        e->tracks[t] = &e->trackContents[t];
}

static void buildSingle(ParticleSystem& ps)
{
    ps.setName("FuzzSingle");
    Emitter* e = ps.addRootEmitter();
    e->name          = "Spark";
    e->colorTexture  = "FX\\SPARK.TGA";
    e->normalTexture = "FX\\SPARK_N.TGA";
    e->lifetime      = 1.5f;
    pinFields(e);
}

static void buildGraph(ParticleSystem& ps)
{
    ps.setName("FuzzGraph");
    ps.setLeaveParticles(true);
    Emitter* root = ps.addRootEmitter();
    root->name         = "Root";
    root->colorTexture = "FX\\ROOT.TGA";
    root->linkGroup    = 7;
    Emitter* death = ps.addDeathEmitter(root);
    death->name         = "OnDeath";
    death->colorTexture = "FX\\DEATH.TGA";
    Emitter* life = ps.addLifetimeEmitter(root);
    life->name         = "DuringLife";
    life->colorTexture = "FX\\LIFE.TGA";
    life->trackContents[ParticleSystem::TRACK_SCALE].keys.insert(
        Emitter::Track::Key(50.0f, 2.0f));
    Emitter* second = ps.addRootEmitter();
    second->name         = "Second";
    second->colorTexture = "FX\\SECOND.TGA";
    for (size_t i = 0; i < ps.getEmitters().size(); ++i)
        pinFields(ps.getEmitters()[i]);
    LinkExemptFlags flags = GetDefaultLinkExemptFlags();
    flags.lifetime = true;
    ps.setLinkExemptFlags(7u, flags);
}

static std::vector<Bytes> corpus()
{
    std::vector<Bytes> out;
    { ParticleSystem ps; buildSingle(ps); out.push_back(serialize(ps)); }
    { ParticleSystem ps; buildGraph(ps);  out.push_back(serialize(ps)); }
    return out;
}

// Splice a sibling chunk into the 0x0900 root just before its trailing 0x0002
// leaveParticles chunk (8-byte header + 1-byte payload, always written last),
// and grow the root's size to match.
static Bytes insertBeforeLeaveParticles(const Bytes& image, const Bytes& sibling)
{
    Bytes out = image;
    const size_t at = out.size() - 9;
    out.insert(out.begin() + at, sibling.begin(), sibling.end());
    const uint32_t rootHeader = getU32(out, 4);
    putU32(out, 4, (rootHeader & 0x80000000u)
                 + (rootHeader & 0x7FFFFFFFu) + (uint32_t)sibling.size());
    return out;
}

// ---- (b) ChunkReader edge helpers ---------------------------------------------
enum ReaderStep { NEXT, NEXT_MINI, SKIP };

// Run `steps` on a fresh reader over `image`; the LAST step must throw
// BadFileException and every earlier one must succeed.
static bool lastStepThrowsBadFile(const Bytes& image, const std::vector<ReaderStep>& steps)
{
    MemoryFile* f = fileOf(image);
    bool ok = false;
    size_t i = 0;
    try
    {
        ChunkReader r(f);
        for (; i < steps.size(); ++i)
        {
            switch (steps[i])
            {
                case NEXT:      (void)r.next();     break;
                case NEXT_MINI: (void)r.nextMini(); break;
                case SKIP:      r.skip();           break;
            }
        }
    }
    catch (BadFileException&) { ok = (i + 1 == steps.size()); }
    catch (...)               { ok = false; }
    f->Release();
    return ok;
}

// ---- deterministic xorshift32 ---------------------------------------------------
static uint32_t g_rng = 0x5EED0A10u;
static uint32_t xr()
{
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}

int main()
{
    std::printf("test_particle_alo_fuzz\n");

    const std::vector<Bytes> images = corpus();

    // =====================================================================
    // (a) VALID CORPUS ROUND-TRIP
    // =====================================================================
    for (size_t c = 0; c < images.size(); ++c)
    {
        ParticleSystem* ps = NULL;
        const bool loaded = load(images[c], &ps) == LOADED_OK && ps;
        CHECK(loaded, c == 0 ? "corpus[0] (single emitter) loads"
                             : "corpus[1] (spawn graph + link data) loads");
        if (loaded)
        {
            const Bytes again = serialize(*ps);
            if (again != images[c])
            {
                size_t at = 0;
                while (at < again.size() && at < images[c].size() && again[at] == images[c][at]) ++at;
                std::printf("  corpus[%zu]: sizes %zu -> %zu, first difference at byte %zu\n",
                            c, images[c].size(), again.size(), at);
            }
            CHECK(again == images[c], c == 0 ? "corpus[0] round-trips byte-identical"
                                             : "corpus[1] round-trips byte-identical");
            if (c == 1)
            {
                CHECK(ps->getEmitters().size() == 4 && ps->getLeaveParticles(),
                      "corpus[1]: emitters and leaveParticles survive");
            }
        }
        delete ps;
    }

    // =====================================================================
    // (b) CHUNKREADER EDGES
    // =====================================================================
    {
        // On-disk type 0xFFFFFFFF as the first chunk.
        Bytes img;
        u32le(img, 0xFFFFFFFFu); u32le(img, 0);
        CHECK(lastStepThrowsBadFile(img, { NEXT }),
              "chunk type 0xFFFFFFFF -> BadFileException");
    }
    {
        // ... and nested inside a container.
        Bytes img;
        u32le(img, 0x0900); u32le(img, 0x80000008u);
        u32le(img, 0xFFFFFFFFu); u32le(img, 0);
        CHECK(lastStepThrowsBadFile(img, { NEXT, NEXT }),
              "nested chunk type 0xFFFFFFFF -> BadFileException");
    }
    {
        // next() after the top level already returned -1 (depth would go < 0).
        Bytes img;
        u32le(img, 0x0001); u32le(img, 0);
        CHECK(lastStepThrowsBadFile(img, { NEXT, NEXT, NEXT }),
              "next() past end of file (negative depth) -> BadFileException");
    }
    {
        // nextMini() after the top level already returned -1.
        Bytes img;
        u32le(img, 0x0001); u32le(img, 0);
        CHECK(lastStepThrowsBadFile(img, { NEXT, NEXT, NEXT_MINI }),
              "nextMini() past end of file (negative depth) -> BadFileException");
    }
    {
        // skip() after the top level already returned -1 (pop below 0).
        Bytes img;
        u32le(img, 0x0900); u32le(img, 0x80000000u);
        CHECK(lastStepThrowsBadFile(img, { NEXT, NEXT, SKIP }),
              "skip() past end of file (negative depth) -> BadFileException");
    }
    {
        // nextMini() inside a CONTAINER (m_size < 0): the payload must not be
        // parsed as mini-chunks.
        Bytes img;
        u32le(img, 0x0900); u32le(img, 0x80000002u);
        img.push_back(0x01); img.push_back(0x00);
        CHECK(lastStepThrowsBadFile(img, { NEXT, NEXT_MINI }),
              "nextMini() inside a container (m_size < 0) -> BadFileException");
    }
    {
        // nextMini() after the data chunk's own -1 (m_size reset to -1).
        Bytes img;
        u32le(img, 0x0900); u32le(img, 0x8000000Au);
        u32le(img, 0x0002); u32le(img, 2);
        img.push_back(0x01); img.push_back(0x00);
        CHECK(lastStepThrowsBadFile(img, { NEXT, NEXT, NEXT_MINI, NEXT_MINI, NEXT_MINI }),
              "nextMini() after its chunk ended (m_size < 0) -> BadFileException");
    }

    // =====================================================================
    // (c) TOLERANT TAIL: unknown chunk before 0x0002 keeps leaveParticles
    // =====================================================================
    {
        const Bytes& base = images[1];   // leaveParticles == true

        Bytes unknownData;
        u32le(unknownData, 0x0042); u32le(unknownData, 4); u32le(unknownData, 0xA5A5A5A5u);
        ParticleSystem* ps = NULL;
        Outcome o = load(insertBeforeLeaveParticles(base, unknownData), &ps);
        CHECK(o == LOADED_OK && ps, "unknown data chunk before 0x0002 loads");
        CHECK(ps && ps->getLeaveParticles(),
              "unknown data chunk followed by 0x0002 keeps leaveParticles");
        delete ps; ps = NULL;

        Bytes unknownContainer;
        u32le(unknownContainer, 0x0043); u32le(unknownContainer, 0x8000000Cu);
        u32le(unknownContainer, 0x0044); u32le(unknownContainer, 4); u32le(unknownContainer, 0);
        o = load(insertBeforeLeaveParticles(base, unknownContainer), &ps);
        CHECK(o == LOADED_OK && ps && ps->getLeaveParticles(),
              "unknown container chunk followed by 0x0002 keeps leaveParticles");
        delete ps; ps = NULL;

        Bytes sentinelType;
        u32le(sentinelType, 0xFFFFFFFFu); u32le(sentinelType, 0);
        CHECK(load(insertBeforeLeaveParticles(base, sentinelType)) == REJECTED,
              "system-level chunk type 0xFFFFFFFF -> rejected, not a silent early end");
    }

    // =====================================================================
    // (d) TRUNCATION SWEEP + SEEDED MUTATION FUZZ
    // =====================================================================
    {
        size_t badExc = 0, rejected = 0, loaded = 0;
        for (size_t c = 0; c < images.size(); ++c)
        {
            for (size_t len = 0; len < images[c].size(); ++len)
            {
                Bytes t(images[c].begin(), images[c].begin() + len);
                switch (load(t))
                {
                    case LOADED_OK:     ++loaded;   break;
                    case REJECTED:      ++rejected; break;
                    case BAD_EXCEPTION: if (++badExc == 1)
                        std::printf("  first foreign exception: corpus[%zu] truncated to %zu\n", c, len);
                        break;
                }
            }
        }
        std::printf("  truncation sweep: %zu rejected, %zu loaded\n", rejected, loaded);
        CHECK(badExc == 0, "truncation: only documented exception types escape");
        CHECK(rejected > 0, "truncation: prefixes are rejected");
    }

    // 2000 rounds keeps the cpp-unit lane fast; PARTICLE_ALO_FUZZ_ROUNDS raises
    // it for a deep pass. The seed is fixed, so round N reproduces exactly.
    {
        int kRounds = 2000;
        if (const char* env = std::getenv("PARTICLE_ALO_FUZZ_ROUNDS"))
        {
            const long v = std::strtol(env, nullptr, 10);
            if (v > 0 && v <= 10000000L) kRounds = (int)v;
        }
        std::printf("  (mutation rounds: %d)\n", kRounds);
        size_t badExc = 0, rejected = 0, loaded = 0;
        int firstBadRound = -1;
        for (int round = 0; round < kRounds; ++round)
        {
            Bytes img = images[xr() % images.size()];
            switch (xr() % 3)
            {
                case 0:   // flip 1..8 random bits
                {
                    int flips = 1 + (int)(xr() % 8);
                    for (int i = 0; i < flips; ++i)
                        img[xr() % img.size()] ^= (unsigned char)(1u << (xr() % 8));
                    break;
                }
                case 1:   // truncate to a random length
                {
                    img.resize(xr() % img.size());
                    break;
                }
                case 2:   // corrupt a u32 -- chunk types, sizes, counts
                {
                    size_t at = xr() % (img.size() - 3);
                    static const uint32_t evil[] = {
                        0u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu,
                        0x80000008u, 0xFFu, 0x0700u, 0x0002u,
                    };
                    putU32(img, at, (xr() % 8 == 0) ? xr() : evil[xr() % 8]);
                    break;
                }
            }
            switch (load(img))
            {
                case LOADED_OK:     ++loaded;   break;
                case REJECTED:      ++rejected; break;
                case BAD_EXCEPTION: ++badExc; if (firstBadRound < 0) firstBadRound = round; break;
            }
        }
        std::printf("  mutation rounds: %d total, %zu rejected, %zu loaded\n",
                    kRounds, rejected, loaded);
        if (firstBadRound >= 0)
            std::printf("  first foreign exception at round %d\n", firstBadRound);
        CHECK(badExc == 0, "mutations: only documented exception types escape");
        CHECK(rejected + loaded == (size_t)kRounds, "mutations: every round classified");
        CHECK(rejected > 0, "mutations: corpus produced rejects (mutator not a no-op)");
        CHECK(loaded > 0, "mutations: corpus produced survivors (parser is tolerant by design)");
    }

    // =====================================================================
    // (e) A load failure caught as std::exception keeps its message (the host
    //     catches std::exception; wexception used to hide what() instead of
    //     overriding it, so the host saw "Unknown exception").
    // =====================================================================
    {
        std::string got;
        try { throw BadFileException(L"bad chunk"); }
        catch (const std::exception& e) { got = e.what(); }
        CHECK(got == "bad chunk", "BadFileException message reaches catch (const std::exception&)");

        got.clear();
        try { throw BadFileException(L"caf\x00E9.alo"); }
        catch (const std::exception& e) { got = e.what(); }
        CHECK(got == "caf?.alo", "non-ASCII message characters narrow to '?'");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
