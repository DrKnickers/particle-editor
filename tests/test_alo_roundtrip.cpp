// Regression test for ParticleSystem .alo load hardening + save fidelity.
//
// Two parts:
//
//  (a) ROUND-TRIP FIDELITY (audit C1 feasible proof). SaveParticleSystem's
//      temp-then-rename atomicity lives in main.cpp with a heavy dep set and
//      can't be linked / fault-injected standalone. The durable, linkable proof
//      for the save path is round-trip fidelity: build a ParticleSystem via the
//      public API, serialize with ParticleSystem::write(IFile*), reload via the
//      ParticleSystem(IFile*) ctor, and assert key fields survive intact. A
//      broken writer/reader (the thing a temp-then-rename guards the on-disk copy
//      against) surfaces here as a fidelity failure.
//
//  (b) MALFORMED CORPUS (audit A2/A3/A4 + string terminator). Craft .alo chunk
//      byte images that must be REJECTED with BadFileException (not crash) or, for
//      the two documented CLAMP fixes, load with the field clamped rather than
//      throwing. The valid envelopes are produced by serializing a real system and
//      byte-patching one field to its crafted value (located by a unique sentinel),
//      so each image stays structurally valid up to the targeted corruption.
//
// See tests/build_test_alo_roundtrip.bat.

#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include "ChunkFile.h"
#include "files.h"
#include "exceptions.h"
#include "LinkGroup.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Link stub: ~Emitter calls ParticleSystemInstance::RemoveEmitter, whose real
// body is D3D-coupled. These tests never register an EmitterInstance, so a no-op
// keeps the link graph on the pure data-model TUs (mirrors test_emitter_reorder).
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Emitter = ParticleSystem::Emitter;
using Track   = ParticleSystem::Emitter::Track;
typedef std::vector<unsigned char> Bytes;

// Serialize a ParticleSystem to a byte image via ParticleSystem::write(IFile*).
static Bytes serialize(ParticleSystem& ps)
{
    MemoryFile* f = new MemoryFile();   // rc=1
    ps.write(f);
    f->seek(0);
    Bytes out(f->size());
    if (!out.empty()) f->read(out.data(), (unsigned long)out.size());
    f->Release();                       // rc=0
    return out;
}

// Load a ParticleSystem from a byte image. Returns true if it constructed
// cleanly (no throw), false if it threw BadFileException; `other` flags any
// other exception type (recorded as a failure by the caller).
static bool loads(const Bytes& image, ParticleSystem** out, bool& other)
{
    other = false;
    MemoryFile* f = new MemoryFile();   // rc=1
    if (!image.empty()) f->write(image.data(), (unsigned long)image.size());
    f->seek(0);
    try
    {
        ParticleSystem* ps = new ParticleSystem(f);
        f->Release();
        if (out) *out = ps; else delete ps;
        return true;
    }
    catch (BadFileException&) { f->Release(); return false; }   // WrongFileException derives from this
    catch (...)               { other = true; f->Release(); return false; }
}

static void expectBadFile(const Bytes& image, const char* label)
{
    bool other = false;
    bool ok = loads(image, NULL, other);
    CHECK(!ok && !other, label);
}

static bool chunkWalkThrowsBadFile(const Bytes& image, bool& other)
{
    other = false;
    MemoryFile* f = new MemoryFile();
    if (!image.empty()) f->write(image.data(), (unsigned long)image.size());
    f->seek(0);
    try
    {
        ChunkReader r(f);
        (void)r.next();
        (void)r.next();
        f->Release();
        return false;
    }
    catch (BadFileException&) { f->Release(); return true; }
    catch (...)               { other = true; f->Release(); return false; }
}

// Find the unique occurrence of `needle` in `hay`; -1 if not found or ambiguous.
static long findUnique(const Bytes& hay, const Bytes& needle)
{
    long at = -1;
    if (needle.empty() || needle.size() > hay.size()) return -1;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
    {
        if (std::memcmp(&hay[i], needle.data(), needle.size()) == 0)
        {
            if (at != -1) return -1;    // ambiguous
            at = (long)i;
        }
    }
    return at;
}

static void putU32(Bytes& b, size_t at, uint32_t v)
{
    b[at + 0] = (unsigned char)(v & 0xFF);
    b[at + 1] = (unsigned char)((v >> 8) & 0xFF);
    b[at + 2] = (unsigned char)((v >> 16) & 0xFF);
    b[at + 3] = (unsigned char)((v >> 24) & 0xFF);
}

// Build a one-emitter system whose distinctive field values give us unique
// sentinels to locate-and-patch in the serialized image.
static void buildOne(ParticleSystem& ps)
{
    ps.setName("RoundTripSys");
    Emitter* e = ps.addRootEmitter();
    e->name         = "Em0";
    e->colorTexture = "FX\\C.TGA";
    e->normalTexture = "FX\\N.TGA";
    e->lifetime     = 2.5f;
    // Pick a blendMode whose value (13) cannot collide with any interpolation
    // enum (0..2): blendMode is also written in a mini-chunk type 0x04, so a
    // value of 1 would make 04 04 01 00 00 00 ambiguous with the scale-track
    // IT_SMOOTH sentinel used in b4 below.
    e->blendMode    = ParticleSystem::BLEND_SCANLINES; // 13
    // Sentinel for the A2 nTriangles mini-chunk (0x05). The writer stores
    // max(1,nTriangles)-1, so this writes 0x11111111 -> unique to find.
    e->nTriangles   = 0x11111112ul;
    // Sentinel group type for the A3 patch. write() forces groups[1].type=1,
    // so use groups[0]; 0x22 is a valid (<5) type at write time.
    e->groups[ParticleSystem::GROUP_SPEED].type = ParticleSystem::GT_CYLINDER; // 4
    // Sentinel interpolation for the A4 patch on the scale track (>=4 = float).
    // IT_SMOOTH (1) is used by NO other track by default (only TRACK_INDEX is
    // IT_STEP; the rest are IT_LINEAR), so 04 04 01 00 00 00 is unique.
    e->trackContents[ParticleSystem::TRACK_SCALE].interpolation = Track::IT_SMOOTH; // 1
    for (int t = 0; t < ParticleSystem::NUM_TRACKS; ++t)
        e->tracks[t] = &e->trackContents[t];
}

// ---- hardening: absolute normal-chunk size cap (2026-07 audit) --------------
// A crafted .alo whose top-level chunk claims a payload within its parent bound
// (the whole file) but over kMaxAloChunkBytes must be rejected by
// ChunkReader::next(). The cap previously guarded only mini-chunks, so a
// multi-hundred-MiB file could authorize an equally huge single chunk. The stub
// REPORTS a huge size() while serving only the 8-byte header, so the test
// exercises the exact bound arithmetic without allocating a >256 MiB buffer.
namespace
{
class HugeHeaderFile : public IFile
{
    unsigned char m_hdr[8];
    unsigned long m_claimedSize;
    unsigned long m_pos = 0;
public:
    HugeHeaderFile(uint32_t type, uint32_t chunkSize, unsigned long claimedFileSize)
        : m_claimedSize(claimedFileSize)
    {
        m_hdr[0] = (unsigned char)(type & 0xFF);
        m_hdr[1] = (unsigned char)((type >> 8) & 0xFF);
        m_hdr[2] = (unsigned char)((type >> 16) & 0xFF);
        m_hdr[3] = (unsigned char)((type >> 24) & 0xFF);
        m_hdr[4] = (unsigned char)(chunkSize & 0xFF);
        m_hdr[5] = (unsigned char)((chunkSize >> 8) & 0xFF);
        m_hdr[6] = (unsigned char)((chunkSize >> 16) & 0xFF);
        m_hdr[7] = (unsigned char)((chunkSize >> 24) & 0xFF);
    }
    bool          eof() override               { return m_pos >= m_claimedSize; }
    unsigned long size() override              { return m_claimedSize; }
    void          seek(unsigned long o) override { m_pos = o; }
    unsigned long tell() override              { return m_pos; }
    unsigned long read(void* buffer, unsigned long n) override
    {
        for (unsigned long i = 0; i < n; ++i)
            ((unsigned char*)buffer)[i] = (m_pos + i < 8) ? m_hdr[m_pos + i] : 0;
        m_pos += n;
        return n;
    }
    unsigned long write(const void*, unsigned long) override { return 0; }
};
} // namespace

static void testNormalChunkSizeCap()
{
    // 0x12000000 (288 MiB) payload: inside the claimed file, over the 256 MiB cap.
    const uint32_t huge = 0x12000000u;
    HugeHeaderFile* f = new HugeHeaderFile(0x0900, huge, huge + 8u);   // rc=1
    bool rejected = false, other = false;
    try
    {
        ChunkReader reader(f);
        reader.next();              // must throw: cap, not the parent bound
    }
    catch (BadFileException&) { rejected = true; }
    catch (...)               { other = true; }
    f->Release();
    CHECK(rejected && !other,
          "normal chunk over kMaxAloChunkBytes rejected as BadFile (parent bound alone would accept)");
}

int main()
{
    std::printf("test_alo_roundtrip\n");

    // =====================================================================
    // (a) ROUND-TRIP FIDELITY
    // =====================================================================
    {
        ParticleSystem ps;
        buildOne(ps);
        Bytes image = serialize(ps);
        CHECK(!image.empty(), "write() produced a non-empty .alo image");

        bool other = false;
        ParticleSystem* rp = NULL;
        bool ok = loads(image, &rp, other);
        CHECK(ok && !other && rp, "round-trip: serialized system reloads cleanly");
        if (rp)
        {
            CHECK(rp->getName() == "RoundTripSys", "round-trip: system name survives");
            CHECK(rp->getEmitters().size() == 1, "round-trip: emitter count survives");
            if (rp->getEmitters().size() == 1)
            {
                const Emitter* e = rp->getEmitters()[0];
                CHECK(e->name == "Em0",            "round-trip: emitter name survives");
                CHECK(e->colorTexture == "FX\\C.TGA", "round-trip: colorTexture survives");
                CHECK(e->normalTexture == "FX\\N.TGA", "round-trip: normalTexture survives");
                CHECK(e->nTriangles == 0x11111112ul, "round-trip: nTriangles survives (read +1 of stored)");
                CHECK(e->lifetime == 2.5f,          "round-trip: lifetime survives");
                CHECK(e->blendMode == (unsigned long)ParticleSystem::BLEND_SCANLINES, "round-trip: blendMode survives");
            }
            delete rp;
        }
    }

    {
        ParticleSystem ps;
        buildOne(ps);
        LinkExemptFlags flags = GetDefaultLinkExemptFlags();
        flags.lifetime = true;
        ps.setLinkExemptFlags(42u, flags);

        Bytes image = serialize(ps);
        bool other = false;
        ParticleSystem* rp = NULL;
        bool ok = loads(image, &rp, other);
        CHECK(ok && !other && rp, "round-trip: custom link-exempt chunk reloads cleanly");
        if (rp)
        {
            const LinkExemptFlags& loaded = rp->getLinkExemptFlags(42u);
            CHECK(loaded.lifetime, "round-trip: custom link-exempt flag survives");
            CHECK(loaded.colorTexture, "round-trip: default exempt flags are preserved with custom entry");
            delete rp;
        }
    }

    // --- link-exempt (0x0003) hardening: a corrupt packed size/count must be
    // rejected, not drive an unbounded read or allocation (release-audit).
    {
        ParticleSystem ps;
        buildOne(ps);
        LinkExemptFlags flags = GetDefaultLinkExemptFlags();
        flags.lifetime = true;
        const uint32_t gid = 0x00ABCDEFu;   // distinctive -> unique byte signature
        ps.setLinkExemptFlags(gid, flags);
        const Bytes image = serialize(ps);

        // Each entry stores groupId then flagsBytes (both little-endian u32); the
        // entry count precedes entry 0. Locate the pair by its unique signature.
        Bytes sig(8);
        putU32(sig, 0, gid);
        putU32(sig, 4, (uint32_t)sizeof(LinkExemptFlags));
        long at = findUnique(image, sig);
        CHECK(at >= 4, "hardening: located link-exempt (groupId,flagsBytes) pair");
        if (at >= 4)
        {
            // (i) flagsBytes far larger than the chunk must be rejected.
            Bytes big = image;
            putU32(big, (size_t)at + 4, 0x7FFFFFFFu);
            expectBadFile(big, "hardening: oversized link-exempt flagsBytes rejected");

            // (ii) entry count far larger than the chunk must be rejected.
            Bytes many = image;
            putU32(many, (size_t)at - 4, 0x7FFFFFFFu);
            expectBadFile(many, "hardening: oversized link-exempt count rejected");
        }
    }

    // =====================================================================
    // (b) MALFORMED CORPUS
    // =====================================================================

    // --- b1: wrong root chunk -> WrongFileException (derives BadFileException).
    {
        // A minimal chunk whose type is NOT 0x0900. Container header: 8 bytes,
        // type=0x1234, size = 0x80000000 (container flag, 0 payload).
        Bytes img;
        auto put32 = [&](uint32_t v){ for(int i=0;i<4;i++) img.push_back((unsigned char)((v>>(8*i))&0xFF)); };
        put32(0x1234);
        put32(0x80000000u);
        expectBadFile(img, "wrong root chunk (not 0x0900) -> BadFileException");
    }

    // --- b1b: child chunk size exceeds parent remaining -> BadFileException.
    {
        Bytes img;
        auto put32 = [&](uint32_t v){ for(int i=0;i<4;i++) img.push_back((unsigned char)((v>>(8*i))&0xFF)); };
        put32(0x0900);
        put32(0x80000008u);           // root container has exactly one child header
        put32(0x1000);
        put32(0x80000100u);           // child claims 256 bytes beyond parent end
        bool other = false;
        bool threw = chunkWalkThrowsBadFile(img, other);
        CHECK(threw && !other, "child chunk larger than parent remaining -> BadFileException");
    }

    // For b2..b5 we patch a valid serialized image so it stays structurally
    // sound up to the one corrupted field.
    ParticleSystem base; buildOne(base);
    const Bytes good = serialize(base);

    // --- b2: string chunk with no terminator -> readString() throws BadFileException.
    // The system name chunk (0x0000) holds "RoundTripSys\0". Truncating the NUL
    // (here: overwrite the terminator byte with a non-NUL) makes the last byte
    // non-'\0' -> ChunkReader::readString throws.
    {
        Bytes img = good;
        Bytes needle = { 'R','o','u','n','d','T','r','i','p','S','y','s', 0 };
        long at = findUnique(img, needle);
        CHECK(at >= 0, "b2: located the system-name string payload");
        if (at >= 0)
        {
            img[at + needle.size() - 1] = (unsigned char)'X';   // clobber the NUL terminator
            expectBadFile(img, "string chunk with no terminator -> BadFileException");
        }
    }

    // --- b3: A2 nTriangles = 0xFFFFFFFF must CLAMP to 1 (loads, not throws).
    // Stored value is 0x11111111 inside mini-chunk 0x05 (header bytes 05 04).
    {
        Bytes img = good;
        Bytes needle = { 0x05, 0x04, 0x11, 0x11, 0x11, 0x11 };   // mini hdr + stored value
        long at = findUnique(img, needle);
        CHECK(at >= 0, "b3: located the nTriangles (0x05) mini-chunk");
        if (at >= 0)
        {
            putU32(img, (size_t)at + 2, 0xFFFFFFFFul);   // crafted raw 0xFFFFFFFF
            bool other = false; ParticleSystem* rp = NULL;
            bool ok = loads(img, &rp, other);
            CHECK(ok && !other && rp, "A2: nTriangles=0xFFFFFFFF still LOADS (clamp, no throw)");
            if (rp)
            {
                CHECK(rp->getEmitters().size() == 1 &&
                      rp->getEmitters()[0]->nTriangles == 1,
                      "A2: nTriangles clamped to 1 (no +1 wrap to 0)");
                delete rp;
            }
        }
    }

    // --- b4: A4 interpolation = 0x7FFFFFFF must CLAMP to IT_LINEAR (loads).
    // The scale track (index 4) interpolation is written IT_SMOOTH (1) in a
    // mini-chunk 0x04 (header bytes 04 04). Only this track is IT_SMOOTH (all
    // others are IT_LINEAR/IT_STEP by default), so 04 04 01 00 00 00 is unique.
    {
        Bytes img = good;
        Bytes needle = { 0x04, 0x04, 0x01, 0x00, 0x00, 0x00 };   // mini hdr 0x04 + IT_SMOOTH
        long at = findUnique(img, needle);
        CHECK(at >= 0, "b4: located the scale-track interpolation (0x04=IT_SMOOTH) mini-chunk");
        if (at >= 0)
        {
            putU32(img, (size_t)at + 2, 0x7FFFFFFFul);   // crafted out-of-range enum
            bool other = false; ParticleSystem* rp = NULL;
            bool ok = loads(img, &rp, other);
            CHECK(ok && !other && rp, "A4: interpolation=0x7FFFFFFF still LOADS (clamp, no throw)");
            if (rp && rp->getEmitters().size() == 1)
            {
                Track::InterpolationType got =
                    rp->getEmitters()[0]->trackContents[ParticleSystem::TRACK_SCALE].interpolation;
                CHECK(got == Track::IT_LINEAR, "A4: out-of-range interpolation clamped to IT_LINEAR");
            }
            delete rp;
        }
    }

    // --- b5: A3 Group.type = 0xDEADBEEF (>= NUM_GROUP_TYPES) -> BadFileException.
    // groups[GROUP_SPEED].type was set to GT_CYLINDER (4); it is blitted raw as
    // the first 4 bytes of the 0x1101 group payload. 04 00 00 00 also matches the
    // interpolation IT_LINEAR mini-chunks, so anchor on the *group* by including
    // the bytes that follow (the group's float fields are all 0 by default ->
    // 04 00 00 00 00 00 00 00 ... ). We search for the cylinder type (4) followed
    // by 28 zero bytes (minX..valZ defaults) which is unique to the group blit.
    {
        Bytes img = good;
        Bytes needle;
        needle.push_back(0x04); needle.push_back(0x00); needle.push_back(0x00); needle.push_back(0x00);
        for (int i = 0; i < 28; ++i) needle.push_back(0x00);   // 7 trailing zero floats
        long at = findUnique(img, needle);
        CHECK(at >= 0, "b5: located the speed-group raw blit (type=GT_CYLINDER)");
        if (at >= 0)
        {
            putU32(img, (size_t)at, 0xDEADBEEFul);   // out-of-range group type
            expectBadFile(img, "A3: Group.type=0xDEADBEEF (>= NUM_GROUP_TYPES) -> BadFileException");
        }
    }

    testNormalChunkSizeCap();

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
