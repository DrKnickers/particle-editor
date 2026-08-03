// Malformed/truncated-input fuzz for the static-mesh .alo decoder
// (src/AloModel.cpp), mirroring tests/test_meg_fuzz.cpp in shape.
//
// Builds a small VALID .alo byte image in memory (same chunk builders as
// tests/test_alo_model.cpp: skeleton + two-submesh mesh + connections), then
// runs deterministic xorshift-seeded mutation rounds (byte flips, truncations,
// u32 length/count-field corruption) plus an exhaustive truncation sweep, and
// asserts for every mutant:
//   - no crash / no hang (bounded loop; the gate runner enforces a timeout),
//   - failures surface ONLY as the DOCUMENTED error path (AloModel.h:159-163:
//     WrongFileException / BadFileException -- WrongFile derives from BadFile
//     -- or ReadException for truncation), never any other exception type,
//   - a mutant that still loads "cleanly" satisfies the decoder's structural
//     invariants (never silent success with garbage): every sub-mesh's vertex
//     blob is empty (legacy/skip path) or exactly vertexCount * 144 B with
//     1 <= vertexCount <= 0xFFFF (AloModel.cpp:198-201), index bytes match
//     primitiveCount * 6 with primitiveCount <= 1M (AloModel.cpp:214-217),
//     and bones/connections respect their 4096 caps (AloModel.cpp:328,351;
//     ResourceLimits.h:17-18).
//
// DISPATCH-KIND FLIPS (now COVERED, issue #487): flipping the container/data
// kind of a chunk type the decoder dispatches on used to crash it -- a chunk
// expected as a CONTAINER but arriving as DATA (e.g. skeleton 0x200 as a data
// chunk) made the sub-reader's `while (r.next() != -1)` walk escape the chunk,
// consume SIBLING chunks plus the parent's -1 terminator, and double-pop
// ChunkReader::m_curDepth to -1 (Debug assert ChunkReader.cpp:52 / Release OOB
// read of m_offsets[-1]); same class via nextMini's `assert(m_size >= 0)`
// (ChunkReader.cpp:11) when a mini-chunk host (0x10102-0x10106 / 0x602) was
// flipped to a container. AloModel.cpp now guards its dispatch sites
// (VerifyContainer / VerifyDataLeaf) so these mutants take the documented
// BadFile path instead. The former expectedKindMismatch() skip has been
// removed, so the fuzzer now exercises this whole class rather than dodging it.
//
// Standalone console exe; see tests/build_test_alo_fuzz.bat.

#include "AloModel.h"
#include "files.h"
#include "exceptions.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>   // getenv / strtol for ALO_FUZZ_ROUNDS
#include <cstring>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

typedef std::vector<unsigned char> Bytes;

// ---- little-endian byte writers + chunk builders (test_alo_model.cpp) ------
static void u32le(Bytes& b, uint32_t v) {
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
    b.push_back((unsigned char)((v >> 16) & 0xFF));
    b.push_back((unsigned char)((v >> 24) & 0xFF));
}
static void u16le(Bytes& b, uint16_t v) {
    b.push_back((unsigned char)(v & 0xFF));
    b.push_back((unsigned char)((v >> 8) & 0xFF));
}
static void f32le(Bytes& b, float f) {
    uint32_t u; std::memcpy(&u, &f, 4); u32le(b, u);
}
static void cstr(Bytes& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end()); b.push_back(0);
}
static Bytes leaf(uint32_t id, const Bytes& payload) {
    Bytes b; u32le(b, id); u32le(b, (uint32_t)payload.size());
    b.insert(b.end(), payload.begin(), payload.end()); return b;
}
static Bytes container(uint32_t id, const std::vector<Bytes>& kids) {
    Bytes payload;
    for (const auto& k : kids) payload.insert(payload.end(), k.begin(), k.end());
    Bytes b; u32le(b, id); u32le(b, (uint32_t)payload.size() | 0x80000000u);
    b.insert(b.end(), payload.begin(), payload.end()); return b;
}
static void mini(Bytes& b, unsigned char id, const Bytes& payload) {
    b.push_back(id); b.push_back((unsigned char)payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
}
static Bytes floatParam(const std::string& name, float v) {
    Bytes p, nm, val; cstr(nm, name); mini(p, 1, nm); f32le(val, v); mini(p, 2, val);
    return leaf(0x10103, p);
}
static Bytes texParam(const std::string& name, const std::string& fn) {
    Bytes p, nm, val; cstr(nm, name); mini(p, 1, nm); cstr(val, fn); mini(p, 2, val);
    return leaf(0x10105, p);
}
static Bytes countsChunk(uint32_t verts, uint32_t prims) {
    Bytes p; u32le(p, verts); u32le(p, prims); p.resize(128, 0);
    return leaf(0x10001, p);
}
static Bytes geometry(uint32_t n, uint32_t prims, const std::string& fmt) {
    std::vector<Bytes> kids;
    kids.push_back(countsChunk(n, prims));
    Bytes f; cstr(f, fmt); kids.push_back(leaf(0x10002, f));
    Bytes verts; verts.resize((size_t)n * 144, 0); kids.push_back(leaf(0x10007, verts));
    Bytes idx; for (uint32_t i = 0; i < prims * 3; ++i) u16le(idx, (uint16_t)(i & 3));
    kids.push_back(leaf(0x10004, idx));
    return container(0x10000, kids);
}
static Bytes material(const std::string& shader, const std::vector<Bytes>& params) {
    std::vector<Bytes> kids; Bytes nm; cstr(nm, shader); kids.push_back(leaf(0x10101, nm));
    for (const auto& pr : params) kids.push_back(pr);
    return container(0x10100, kids);
}
static Bytes mesh(const std::string& name, const std::vector<std::pair<Bytes, Bytes>>& subs) {
    std::vector<Bytes> kids;
    Bytes nm; cstr(nm, name); kids.push_back(leaf(0x0401, nm));
    Bytes info; info.resize(128, 0); kids.push_back(leaf(0x0402, info));
    for (const auto& s : subs) { kids.push_back(s.first); kids.push_back(s.second); }
    return container(0x0400, kids);
}
static Bytes bone(const std::string& name, uint32_t parent) {
    Bytes nm; cstr(nm, name);
    Bytes d; u32le(d, parent); u32le(d, 1);
    const float ident[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
    for (int i = 0; i < 12; ++i) f32le(d, ident[i]);
    return container(0x0202, { leaf(0x0203, nm), leaf(0x0205, d) });
}
static Bytes skeleton(const std::vector<Bytes>& bones) {
    Bytes info; u32le(info, (uint32_t)bones.size()); info.resize(128, 0);
    std::vector<Bytes> kids; kids.push_back(leaf(0x0201, info));
    for (const auto& b : bones) kids.push_back(b);
    return container(0x0200, kids);
}
static Bytes connectionObj(uint32_t objIdx, uint32_t boneIdx) {
    Bytes p, v, v2;
    u32le(v, objIdx);  mini(p, 2, v);
    u32le(v2, boneIdx); mini(p, 3, v2);
    return leaf(0x0602, p);
}
static Bytes connections(const std::vector<Bytes>& objs) {
    Bytes counts, v; u32le(v, (uint32_t)objs.size()); mini(counts, 1, v);
    std::vector<Bytes> kids; kids.push_back(leaf(0x0601, counts));
    for (const auto& o : objs) kids.push_back(o);
    return container(0x0600, kids);
}
static Bytes assemble(const std::vector<Bytes>& roots) {
    Bytes b; for (const auto& r : roots) b.insert(b.end(), r.begin(), r.end()); return b;
}

// ---- deterministic xorshift32 ----------------------------------------------
static uint32_t g_rng = 0xC0FFEE01u;
static uint32_t xr()
{
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}

// ---- one decode attempt with outcome classification -------------------------
enum Outcome
{
    LOADED_OK,          // parsed cleanly AND passed the structural invariants
    REJECTED,           // documented error path (BadFile/WrongFile/Read)
    BAD_INVARIANT,      // parsed "cleanly" but the result violates an invariant
    BAD_EXCEPTION,      // any other exception type escaped
};

// Records WHICH invariant a BAD_INVARIANT mutant violated, so a deep-round
// failure is diagnosable without a second instrumented build.
static std::string g_invariantReason;

static bool modelInvariantsHold(const AloModel& m)
{
    g_invariantReason.clear();
    auto fail = [](const char* why) { g_invariantReason = why; return false; };

    if (m.meshes.empty()) return fail("meshes empty (ctor would have thrown)");
    if (m.bones.size() > 4096)       return fail("bones > 4096");
    if (m.connections.size() > 4096) return fail("connections > 4096");
    for (const AloMesh& me : m.meshes)
    {
        for (const AloSubMesh& sm : me.subMeshes)
        {
            if (!sm.rawVertexBytes.empty())
            {
                if (sm.vertexCount < 1)         return fail("vertexCount < 1 with non-empty vertex blob");
                if (sm.vertexCount > 0xFFFFu)   return fail("vertexCount > 0xFFFF");
                if (sm.rawVertexBytes.size()
                        != (unsigned long long)sm.vertexCount * kAloVertexStride)
                    return fail("vertex blob size != vertexCount * stride");
            }
            if (!sm.indexBytes.empty())
            {
                if (sm.primitiveCount > 0x100000u) return fail("primitiveCount > 1M");
                if (sm.indexBytes.size()
                        != (unsigned long long)sm.primitiveCount * 6u)
                    return fail("index blob size != primitiveCount * 6");
            }
        }
    }
    return true;
}

static Outcome decode(const Bytes& image)
{
    MemoryFile* f = new MemoryFile();   // rc=1
    if (!image.empty()) f->write(image.data(), (unsigned long)image.size());
    f->seek(0);
    Outcome out;
    try
    {
        AloModel m = LoadAloModel(f);
        out = modelInvariantsHold(m) ? LOADED_OK : BAD_INVARIANT;
    }
    catch (BadFileException&) { out = REJECTED; }   // incl. WrongFileException
    catch (ReadException&)    { out = REJECTED; }
    catch (...)               { out = BAD_EXCEPTION; }
    f->Release();
    return out;
}

int main()
{
    std::printf("test_alo_fuzz\n");

    // ---- the valid base image -------------------------------------------------
    Bytes base = assemble({
        skeleton({ bone("Root", 0xFFFFFFFFu), bone("Base", 0) }),
        mesh("fuzz", {
            { material("MeshGloss.fx", { texParam("BaseTexture", "hull.dds"),
                                         floatParam("Shininess", 32.0f) }),
              geometry(4, 2, "alD3dVertN") },
            { material("MeshAdditive.fx", {}),
              geometry(6, 3, "alD3dVertNU2") },
        }),
        connections({ connectionObj(0, 1) }),
    });
    CHECK(decode(base) == LOADED_OK, "base image parses cleanly and holds invariants");

    // ---- exhaustive truncation sweep -------------------------------------------
    // Every prefix of the file must either load or throw the documented types.
    {
        size_t badExc = 0, badInv = 0, rejected = 0, loaded = 0;
        for (size_t len = 0; len < base.size(); ++len)
        {
            Bytes t(base.begin(), base.begin() + len);
            switch (decode(t))
            {
                case LOADED_OK:     ++loaded;  break;
                case REJECTED:      ++rejected; break;
                case BAD_INVARIANT: ++badInv;  if (badInv == 1)
                    std::printf("  first invariant violation at truncation %zu\n", len); break;
                case BAD_EXCEPTION: ++badExc;  if (badExc == 1)
                    std::printf("  first foreign exception at truncation %zu\n", len); break;
            }
        }
        std::printf("  truncation sweep: %zu lengths, %zu rejected, %zu loaded\n",
                    base.size(), rejected, loaded);
        CHECK(badExc == 0, "truncation: only documented exception types escape");
        CHECK(badInv == 0, "truncation: no clean load ever violates invariants");
    }

    // ---- seeded mutation rounds -------------------------------------------------
    // Each round mutates a fresh copy with byte flips, a truncation, or a u32
    // (length/count/type field) overwrite at a random offset.
    //
    // 2000 by default keeps the cpp-unit gate lane fast. ALO_FUZZ_ROUNDS raises
    // it for a deep audit pass (the 2026-07 release audit asked for >= 10,000
    // per parser family) without making every gate run pay for it. The xorshift
    // seed is fixed either way, so round N is identical across runs and a
    // failure at round N reproduces exactly.
    {
        int kRounds = 2000;
        if (const char* env = std::getenv("ALO_FUZZ_ROUNDS"))
        {
            const long v = std::strtol(env, nullptr, 10);
            if (v > 0 && v <= 10000000L) kRounds = (int)v;
        }
        std::printf("  (mutation rounds: %d%s)\n", kRounds,
                    std::getenv("ALO_FUZZ_ROUNDS") ? " [ALO_FUZZ_ROUNDS]" : " [default]");
        size_t badExc = 0, badInv = 0, rejected = 0, loaded = 0;
        int firstBadRound = -1;
        std::string g_firstBadReason;
        for (int round = 0; round < kRounds; ++round)
        {
            Bytes img = base;
            switch (xr() % 3)
            {
                case 0:   // flip 1..8 random bytes
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
                case 2:   // corrupt a u32 -- targets sizes/counts/types/lengths
                {
                    if (img.size() >= 4)
                    {
                        size_t at = xr() % (img.size() - 3);
                        static const uint32_t evil[] = {
                            0u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu,
                            0x10000u, 0xFFFFu, 0x0400u,
                        };
                        uint32_t v = (xr() % 8 == 0) ? xr() : evil[xr() % 7];
                        img[at + 0] = (unsigned char)(v & 0xFF);
                        img[at + 1] = (unsigned char)((v >> 8) & 0xFF);
                        img[at + 2] = (unsigned char)((v >> 16) & 0xFF);
                        img[at + 3] = (unsigned char)((v >> 24) & 0xFF);
                    }
                    break;
                }
            }
            switch (decode(img))
            {
                case LOADED_OK:     ++loaded;   break;
                case REJECTED:      ++rejected; break;
                case BAD_INVARIANT: ++badInv;
                if (firstBadRound < 0) { firstBadRound = round; g_firstBadReason = g_invariantReason; }
                break;
                case BAD_EXCEPTION: ++badExc;   if (firstBadRound < 0) firstBadRound = round; break;
            }
        }
        std::printf("  mutation rounds: %d total, %zu rejected, %zu loaded\n",
                    kRounds, rejected, loaded);
        if (firstBadRound >= 0)
            std::printf("  first bad round: %d  reason: %s\n", firstBadRound,
                        g_firstBadReason.empty() ? "(exception, not invariant)"
                                                 : g_firstBadReason.c_str());
        CHECK(badExc == 0, "mutations: only documented exception types escape");
        CHECK(badInv == 0, "mutations: no clean load ever violates invariants");
        CHECK(rejected + loaded == (size_t)kRounds,
              "mutations: every round classified");
        // Sanity on the harness itself: the corpus must actually hit both sides.
        CHECK(rejected > 0, "mutations: corpus produced rejects (mutator not a no-op)");
        CHECK(loaded > 0, "mutations: corpus produced survivors (parser is tolerant by design)");
    }

    // ---- the base image still round-trips after the fuzz (no global state) ----
    CHECK(decode(base) == LOADED_OK, "base image still parses cleanly after fuzzing");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
