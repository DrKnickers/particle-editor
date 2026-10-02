// Regression test for emitter state that a save + reload (and therefore every
// undo/redo, which round-trips through the same writer and reader) used to drop
// (2026-10 audit).
//
//  (a) Property 0x11. readProperties stored it in `unknown11` but writeProperties
//      never wrote it, so the first save or undo lost it. No shipped particle
//      file carries 0x11, so the image here is our own serialization with a
//      0x11 mini-chunk byte-patched in right after 0x0F (no game bytes in the
//      repo). It must load, survive an emitter copy and a clipboard copy, and
//      re-save byte-identical; a file without 0x11 must not gain one.
//  (b) Explicit channel unlock. The reader re-locks a colour channel whose keys
//      match an earlier channel's, so unlocking a channel without editing it
//      came back locked after save + reload or undo. The editor-only 0x0101
//      chunk records those channels; it is written only when one exists.
//  (c) Emitter::write is logically const: the lifetime group it derives from
//      lifetime + randomLifetimePerc is normalized on a copy, not on the emitter
//      (undo snapshots const_cast into it).
//
// See the test_alo_editor_state entry in tests/native-tests.json.

#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include "ChunkFile.h"
#include "files.h"
#include "exceptions.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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
using Track   = ParticleSystem::Emitter::Track;
typedef std::vector<unsigned char> Bytes;

static Bytes serialize(ParticleSystem& ps)
{
    MemoryFile* f = new MemoryFile();
    ps.write(f);
    f->seek(0);
    Bytes out(f->size());
    if (!out.empty()) f->read(out.data(), (unsigned long)out.size());
    f->Release();
    return out;
}

// Returns NULL when the image doesn't load.
static ParticleSystem* load(const Bytes& image)
{
    MemoryFile* f = new MemoryFile();
    if (!image.empty()) f->write(image.data(), (unsigned long)image.size());
    f->seek(0);
    ParticleSystem* ps = NULL;
    try { ps = new ParticleSystem(f); }
    catch (...) { ps = NULL; }
    f->Release();
    return ps;
}

static size_t find(const Bytes& hay, const Bytes& needle)
{
    for (size_t i = 0; i + needle.size() <= hay.size(); i++)
        if (std::memcmp(&hay[i], needle.data(), needle.size()) == 0) return i;
    return (size_t)-1;
}

static Bytes floatBytes(float f)
{
    Bytes b(sizeof(float));
    std::memcpy(b.data(), &f, sizeof(float));
    return b;
}

// Insert `extra` at offset `at`, growing the size of every chunk whose body
// contains `at` (headers: u32 type, u32 size with the container bit on top).
static void growChunks(Bytes& image, size_t start, size_t end, size_t at, uint32_t by)
{
    size_t off = start;
    while (off + 8 <= end)
    {
        uint32_t size;
        std::memcpy(&size, &image[off + 4], 4);
        const size_t body    = off + 8;
        const size_t bodyEnd = body + (size & 0x7FFFFFFFu);
        if (body <= at && at < bodyEnd)
        {
            if (size & 0x80000000u) growChunks(image, body, bodyEnd, at, by);
            size += by;
            std::memcpy(&image[off + 4], &size, 4);
        }
        off = bodyEnd;
    }
}

static Bytes insertAt(Bytes image, size_t at, const Bytes& extra)
{
    growChunks(image, 0, image.size(), at, (uint32_t)extra.size());
    image.insert(image.begin() + at, extra.begin(), extra.end());
    return image;
}

static void setLastKey(Track& track, float value)
{
    Track::KeyMap::iterator last = --track.keys.end();
    track.keys.erase(last);
    track.keys.insert(Track::Key(100.0f, value));
}

int main()
{
    // ---------------------------------------------------------------- (a)
    std::printf("[0x11] read, copied and written back only when present\n");
    {
        const float kLifetime  = 1234.5f;   // sentinel to locate the 0x0F mini-chunk
        const float kUnknown11 = 0.625f;

        ParticleSystem ps;
        Emitter* src = ps.addRootEmitter();
        src->lifetime  = kLifetime;
        // setDefaults zeroes these two, which keeps the image (and its
        // byte-identity re-save) deterministic.
        CHECK(!src->unknown2b && src->unknown49 == 0,
              "a default emitter has unknown2b / unknown49 zeroed");
        const Bytes plain = serialize(ps);

        Bytes lifetimeMini = { 0x0F, 0x04 };
        const Bytes lf = floatBytes(kLifetime);
        lifetimeMini.insert(lifetimeMini.end(), lf.begin(), lf.end());
        const size_t at = find(plain, lifetimeMini);
        CHECK(at != (size_t)-1, "0x0F mini-chunk located in the serialized image");

        Bytes mini11 = { 0x11, 0x04 };
        const Bytes uf = floatBytes(kUnknown11);
        mini11.insert(mini11.end(), uf.begin(), uf.end());
        CHECK(find(plain, mini11) == (size_t)-1, "a system that never had 0x11 doesn't write one");

        if (at != (size_t)-1)
        {
            const Bytes patched = insertAt(plain, at + lifetimeMini.size(), mini11);
            ParticleSystem* rp = load(patched);
            CHECK(rp != NULL, "image with 0x11 after 0x0F loads");
            if (rp != NULL)
            {
                Emitter* e = rp->getEmitters()[0];
                CHECK(e->has11 && e->unknown11 == kUnknown11, "0x11 value is read");

                const Bytes resaved = serialize(*rp);
                CHECK(resaved == patched, "re-save is byte-identical (0x11 written back after 0x0F)");
                for (size_t i = 0; i < resaved.size() && i < patched.size(); i++)
                {
                    if (resaved[i] != patched[i])
                    {
                        std::printf("    first difference at byte %zu of %zu/%zu\n", i, resaved.size(), patched.size());
                        for (size_t k = (i > 16 ? i - 16 : 0); k < i + 16 && k < resaved.size(); k++) std::printf("%02x%s", resaved[k], k == i ? "*" : " ");
                        std::printf("\n");
                        for (size_t k = (i > 16 ? i - 16 : 0); k < i + 16 && k < patched.size(); k++) std::printf("%02x%s", patched[k], k == i ? "*" : " ");
                        std::printf("\n");
                        break;
                    }
                }

                Emitter copied(*e);
                CHECK(copied.has11 && copied.unknown11 == kUnknown11, "emitter copy keeps 0x11");

                // Clipboard copy: Emitter::copy into a 0x0700 chunk and back.
                MemoryFile* f = new MemoryFile();
                {
                    ChunkWriter w(f);
                    w.beginChunk(0x0700);
                    e->copy(w);
                    w.endChunk();
                }
                f->seek(0);
                bool pastedOk = false;
                try
                {
                    ChunkReader r(f);
                    if (r.next() == 0x0700)
                    {
                        Emitter pasted(r);
                        pastedOk = pasted.has11 && pasted.unknown11 == kUnknown11;
                    }
                }
                catch (...) { pastedOk = false; }
                f->Release();
                CHECK(pastedOk, "clipboard copy keeps 0x11");
                delete rp;
            }
        }
    }

    // ---------------------------------------------------------------- (b)
    std::printf("[unlock] an unlocked channel stays unlocked across save + reload\n");
    {
        ParticleSystem base;
        base.addRootEmitter();   // G, B, A locked to R (the default)
        const size_t baseSize = serialize(base).size();

        // Green unlocked with keys identical to red's.
        ParticleSystem ps;
        Emitter* e = ps.addRootEmitter();
        e->tracks[1] = &e->trackContents[1];
        const Bytes image = serialize(ps);
        CHECK(image.size() == baseSize + 12, "unlocked-but-identical channel adds one 12-byte editor chunk");
        ParticleSystem* rp = load(image);
        CHECK(rp != NULL, "image with the unlock chunk loads");
        if (rp != NULL)
        {
            const Emitter* r = rp->getEmitters()[0];
            CHECK(r->tracks[1] == &r->trackContents[1], "green is still unlocked after reload");
            CHECK(r->tracks[2] == &r->trackContents[0] && r->tracks[3] == &r->trackContents[0],
                  "blue and alpha are still locked to red");
            CHECK(serialize(*rp) == image, "second save is byte-identical");
            delete rp;
        }

        // Unlocked and different from every earlier channel: the reader
        // already keeps it unlocked, so no chunk is written.
        ParticleSystem diff;
        Emitter* d = diff.addRootEmitter();
        d->tracks[1] = &d->trackContents[1];
        setLastKey(d->trackContents[1], 0.2f);
        CHECK(serialize(diff).size() == baseSize, "unlocked and different: no editor chunk");

        // Different in memory but identical once quantized to a byte on save
        // (0.5 * 255 and 0.499 * 255 both truncate to 127): still recorded.
        ParticleSystem quant;
        Emitter* q = quant.addRootEmitter();
        setLastKey(q->trackContents[0], 0.5f);
        q->tracks[2] = &q->trackContents[2];
        setLastKey(q->trackContents[2], 0.499f);
        ParticleSystem* rq = load(serialize(quant));
        CHECK(rq != NULL && rq->getEmitters()[0]->tracks[2] == &rq->getEmitters()[0]->trackContents[2],
              "a channel identical only after byte quantization stays unlocked");
        delete rq;
    }

    // ---------------------------------------------------------------- (c)
    std::printf("[const write] the lifetime group is normalized on a copy\n");
    {
        ParticleSystem ps;
        Emitter* e = ps.addRootEmitter();
        e->lifetime           = 2.0f;
        e->randomLifetimePerc = 0.25f;
        e->groups[ParticleSystem::GROUP_LIFETIME].minY = 7.0f;
        e->groups[ParticleSystem::GROUP_LIFETIME].maxY = 9.0f;
        const Bytes image = serialize(ps);
        CHECK(e->groups[ParticleSystem::GROUP_LIFETIME].minY == 7.0f &&
              e->groups[ParticleSystem::GROUP_LIFETIME].maxY == 9.0f,
              "write leaves the emitter's lifetime group untouched");
        ParticleSystem* rp = load(image);
        CHECK(rp != NULL &&
              rp->getEmitters()[0]->groups[ParticleSystem::GROUP_LIFETIME].minY == 1.5f &&
              rp->getEmitters()[0]->groups[ParticleSystem::GROUP_LIFETIME].maxY == 2.0f &&
              rp->getEmitters()[0]->groups[ParticleSystem::GROUP_LIFETIME].type == 1,
              "the written lifetime group is still the normalized one");
        delete rp;
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
