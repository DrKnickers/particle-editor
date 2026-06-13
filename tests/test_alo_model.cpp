// Unit tests for the static-mesh .alo decoder (src/AloModel.cpp).
//
// Builds synthetic .alo byte streams in memory (via the documented chunk wire
// format) and feeds them through LoadAloModel -- no game assets are committed
// or required. Covers happy paths, multi-submesh / multi-mesh, tolerant
// skipping of non-mesh root chunks, the legacy 0x10005 skip, and the malformed
// cases the parser must reject. Standalone console exe; see
// tests/build_test_alo_model.bat.

#include "AloModel.h"
#include "files.h"
#include "exceptions.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

typedef std::vector<unsigned char> Bytes;

// ---- little-endian byte writers ------------------------------------------
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

// ---- chunk builders (mirror the .alo wire format) -------------------------
// leaf:      [u32 type][u32 size (high bit CLEAR)][payload]
// container: [u32 type][u32 size | 0x80000000   ][concatenated children]
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
// mini-chunk inside a param leaf payload: [u8 type][u8 size][payload]
static void mini(Bytes& b, unsigned char id, const Bytes& payload) {
    b.push_back(id); b.push_back((unsigned char)payload.size());
    b.insert(b.end(), payload.begin(), payload.end());
}

static Bytes floatParam(const std::string& name, float v) {
    Bytes p, nm, val; cstr(nm, name); mini(p, 1, nm); f32le(val, v); mini(p, 2, val);
    return leaf(0x10103, p);
}
static Bytes float4Param(const std::string& name, float a, float b, float c, float d) {
    Bytes p, nm, val; cstr(nm, name); mini(p, 1, nm);
    f32le(val, a); f32le(val, b); f32le(val, c); f32le(val, d); mini(p, 2, val);
    return leaf(0x10106, p);
}
static Bytes texParam(const std::string& name, const std::string& fn) {
    Bytes p, nm, val; cstr(nm, name); mini(p, 1, nm); cstr(val, fn); mini(p, 2, val);
    return leaf(0x10105, p);
}

static Bytes countsChunk(uint32_t verts, uint32_t prims) {
    Bytes p; u32le(p, verts); u32le(p, prims); p.resize(128, 0);  // fixed 128B
    return leaf(0x10001, p);
}
static Bytes vertexBlob(uint32_t n, bool markV0) {
    Bytes p; p.resize((size_t)n * 144, 0);
    if (markV0 && n > 0) {
        float pos[3] = { 1.0f, 2.0f, 3.0f };       std::memcpy(&p[0],  pos, 12);
        float col[4] = { 0.25f, 0.5f, 0.75f, 1.0f }; std::memcpy(&p[80], col, 16);  // color @ kAloColorOffset
    }
    return p;
}
static Bytes indexBlob(uint32_t prims) {
    Bytes p; for (uint32_t i = 0; i < prims * 3; ++i) u16le(p, (uint16_t)(i & 3)); return p;
}

static Bytes material(const std::string& shader, const std::vector<Bytes>& params) {
    std::vector<Bytes> kids; Bytes nm; cstr(nm, shader); kids.push_back(leaf(0x10101, nm));
    for (const auto& pr : params) kids.push_back(pr);
    return container(0x10100, kids);
}
static Bytes geometry(uint32_t n, uint32_t prims, const std::string& fmt) {
    std::vector<Bytes> kids;
    kids.push_back(countsChunk(n, prims));
    Bytes f; cstr(f, fmt); kids.push_back(leaf(0x10002, f));
    kids.push_back(leaf(0x10007, vertexBlob(n, true)));
    kids.push_back(leaf(0x10004, indexBlob(prims)));
    return container(0x10000, kids);
}
static Bytes mesh(const std::string& name, const std::vector<std::pair<Bytes, Bytes>>& subs) {
    std::vector<Bytes> kids;
    Bytes nm; cstr(nm, name); kids.push_back(leaf(0x0401, nm));
    Bytes info; info.resize(128, 0); kids.push_back(leaf(0x0402, info));
    for (const auto& s : subs) { kids.push_back(s.first); kids.push_back(s.second); }
    return container(0x0400, kids);
}
static Bytes skeletonStub() {
    Bytes info; u32le(info, 1); info.resize(128, 0);
    return container(0x0200, { leaf(0x0201, info) });
}
static Bytes connectionsStub() {
    Bytes c; u32le(c, 0); return container(0x0600, { leaf(0x0601, c) });
}
static Bytes assemble(const std::vector<Bytes>& roots) {
    Bytes b; for (const auto& r : roots) b.insert(b.end(), r.begin(), r.end()); return b;
}

static AloModel parseBytes(const Bytes& b) {
    MemoryFile* mf = new MemoryFile();
    if (!b.empty()) mf->write(b.data(), (unsigned long)b.size());
    mf->seek(0);
    try {
        AloModel m = LoadAloModel(mf);
        mf->Release();
        return m;
    } catch (...) {
        mf->Release();
        throw;
    }
}

static const AloShaderParam* findParam(const AloSubMesh& sm, const std::string& name) {
    for (const auto& p : sm.params) if (p.name == name) return &p;
    return nullptr;
}

// Dump mode (argv[1] = path to a real .alo): parse + print, no assertions.
// Validates the decoder against real install assets (dev-box only; not run in CI).
static int dumpRealAlo(const char* path) {
    std::wstring wpath(path, path + std::strlen(path));
    IFile* f = nullptr;
    try { f = new PhysicalFile(wpath, PhysicalFile::READ); }
    catch (...) { std::printf("cannot open %s\n", path); return 2; }
    AloModel m;
    try { m = LoadAloModel(f); }
    catch (wexception& e) { f->Release(); std::wprintf(L"parse failed: %s\n", e.what()); return 1; }
    catch (...) { f->Release(); std::printf("parse failed (unknown)\n"); return 1; }
    f->Release();
    std::printf("meshes: %zu\n", m.meshes.size());
    for (size_t mi = 0; mi < m.meshes.size(); ++mi) {
        const AloMesh& me = m.meshes[mi];
        std::printf("  mesh[%zu] \"%s\" submeshes=%zu\n", mi, me.name.c_str(), me.subMeshes.size());
        for (size_t si = 0; si < me.subMeshes.size(); ++si) {
            const AloSubMesh& sm = me.subMeshes[si];
            std::printf("    sub[%zu] shader=%s fmt=%s verts=%u prims=%u vbytes=%zu ibytes=%zu params=%zu\n",
                si, sm.shaderName.c_str(), sm.vertexFormatName.c_str(), sm.vertexCount,
                sm.primitiveCount, sm.rawVertexBytes.size(), sm.indexBytes.size(), sm.params.size());
            for (const auto& p : sm.params) {
                switch (p.kind) {
                    case AloShaderParam::TEXTURE: std::printf("        %s = \"%s\"\n", p.name.c_str(), p.tex.c_str()); break;
                    case AloShaderParam::FLOAT:   std::printf("        %s = %g\n", p.name.c_str(), p.f[0]); break;
                    case AloShaderParam::FLOAT3:  std::printf("        %s = (%g,%g,%g)\n", p.name.c_str(), p.f[0], p.f[1], p.f[2]); break;
                    case AloShaderParam::FLOAT4:  std::printf("        %s = (%g,%g,%g,%g)\n", p.name.c_str(), p.f[0], p.f[1], p.f[2], p.f[3]); break;
                    case AloShaderParam::INT:     std::printf("        %s = %d\n", p.name.c_str(), p.i); break;
                }
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return dumpRealAlo(argv[1]);

    // ---- Happy path: single submesh skydome --------------------------------
    std::printf("[happy]\n");
    {
        Bytes mat = material("Skydome.fx", {
            float4Param("Emissive", 0.5f, 0.5f, 0.5f, 0.0f),
            floatParam("CloudScrollRate", 0.0f),
            floatParam("CloudScale", 1.0f),
            texParam("BaseTexture", "W_clearbluesky.dds"),
            texParam("CloudTexture", "clouds.dds"),
        });
        Bytes geo = geometry(4, 2, "alD3dVertNU2C");
        Bytes file = assemble({ skeletonStub(), mesh("clearblue", { { mat, geo } }), connectionsStub() });

        AloModel m = parseBytes(file);
        CHECK(m.meshes.size() == 1, "1 mesh (skeleton + connections skipped)");
        CHECK(m.meshes[0].name == "clearblue", "mesh name");
        CHECK(m.meshes[0].subMeshes.size() == 1, "1 submesh");
        const AloSubMesh& sm = m.meshes[0].subMeshes[0];
        CHECK(sm.shaderName == "Skydome.fx", "shader name");
        CHECK(sm.vertexFormatName == "alD3dVertNU2C", "vertex format name");
        CHECK(sm.vertexCount == 4, "vertexCount == 4");
        CHECK(sm.primitiveCount == 2, "primitiveCount == 2");
        CHECK(sm.rawVertexBytes.size() == 4 * 144, "rawVertexBytes == 4*144");
        CHECK(sm.indexBytes.size() == 2 * 3 * 2, "indexBytes == 12");

        const AloShaderParam* bt = findParam(sm, "BaseTexture");
        CHECK(bt && bt->kind == AloShaderParam::TEXTURE && bt->tex == "W_clearbluesky.dds", "BaseTexture param");
        const AloShaderParam* ct = findParam(sm, "CloudTexture");
        CHECK(ct && ct->tex == "clouds.dds", "CloudTexture param");
        const AloShaderParam* csr = findParam(sm, "CloudScrollRate");
        CHECK(csr && csr->kind == AloShaderParam::FLOAT && csr->f[0] == 0.0f, "CloudScrollRate param");
        const AloShaderParam* cs = findParam(sm, "CloudScale");
        CHECK(cs && cs->f[0] == 1.0f, "CloudScale param");
        const AloShaderParam* em = findParam(sm, "Emissive");
        CHECK(em && em->kind == AloShaderParam::FLOAT4 && em->f[0] == 0.5f && em->f[3] == 0.0f, "Emissive float4 param");

        // Vertex color lives at kAloColorOffset (float4) -- pins the offset constant.
        float c[4]; std::memcpy(c, &sm.rawVertexBytes[kAloColorOffset], 16);
        CHECK(c[0] == 0.25f && c[1] == 0.5f && c[2] == 0.75f && c[3] == 1.0f, "vertex color @ kAloColorOffset round-trips");
        float pos[3]; std::memcpy(pos, &sm.rawVertexBytes[0], 12);
        CHECK(pos[0] == 1.0f && pos[1] == 2.0f && pos[2] == 3.0f, "vertex position @ 0 round-trips");
    }

    // ---- Multi-submesh: distinct shaders/formats per submesh ----------------
    std::printf("[multi-submesh]\n");
    {
        std::pair<Bytes, Bytes> s0 = { material("MeshGloss.fx", { texParam("BaseTexture", "stars.dds") }),
                                       geometry(8, 4, "alD3dVertN") };
        std::pair<Bytes, Bytes> s1 = { material("MeshAdditive.fx", { texParam("BaseTexture", "sun.dds") }),
                                       geometry(6, 2, "alD3dVertNU2") };
        Bytes file = assemble({ mesh("nebula", { s0, s1 }) });
        AloModel m = parseBytes(file);
        CHECK(m.meshes.size() == 1 && m.meshes[0].subMeshes.size() == 2, "2 submeshes");
        CHECK(m.meshes[0].subMeshes[0].shaderName == "MeshGloss.fx", "submesh 0 shader");
        CHECK(m.meshes[0].subMeshes[0].vertexFormatName == "alD3dVertN", "submesh 0 format");
        CHECK(m.meshes[0].subMeshes[1].shaderName == "MeshAdditive.fx", "submesh 1 shader");
        CHECK(m.meshes[0].subMeshes[1].vertexCount == 6, "submesh 1 vertexCount");
    }

    // ---- Multi-mesh: two 0x400 chunks --------------------------------------
    std::printf("[multi-mesh]\n");
    {
        Bytes m0 = mesh("a", { { material("Skydome.fx", {}), geometry(3, 1, "alD3dVertNU2C") } });
        Bytes m1 = mesh("b", { { material("Skydome.fx", {}), geometry(3, 1, "alD3dVertNU2C") } });
        AloModel m = parseBytes(assemble({ skeletonStub(), m0, m1 }));
        CHECK(m.meshes.size() == 2, "2 meshes");
        CHECK(m.meshes[0].name == "a" && m.meshes[1].name == "b", "mesh names");
    }

    // ---- Legacy 0x10005 vertex chunk: submesh skipped (empty verts) --------
    std::printf("[old-vertex]\n");
    {
        std::vector<Bytes> geoKids;
        geoKids.push_back(countsChunk(4, 2));
        Bytes f; cstr(f, "alD3dVertNU2C"); geoKids.push_back(leaf(0x10002, f));
        Bytes old; old.resize(4 * 128, 0); geoKids.push_back(leaf(0x10005, old));  // legacy 128B
        geoKids.push_back(leaf(0x10004, indexBlob(2)));
        Bytes geo = container(0x10000, geoKids);
        Bytes file = assemble({ mesh("old", { { material("Skydome.fx", {}), geo } }) });
        AloModel m = parseBytes(file);
        CHECK(m.meshes.size() == 1 && m.meshes[0].subMeshes.size() == 1, "old-vertex submesh present");
        CHECK(m.meshes[0].subMeshes[0].rawVertexBytes.empty(), "old-vertex leaves rawVertexBytes empty (skipped)");
    }

    // ---- Malformed: stride mismatch -> BadFileException --------------------
    std::printf("[malformed]\n");
    {
        std::vector<Bytes> geoKids;
        geoKids.push_back(countsChunk(4, 2));
        Bytes f; cstr(f, "alD3dVertNU2C"); geoKids.push_back(leaf(0x10002, f));
        Bytes shortVerts; shortVerts.resize(3 * 144, 0);  // claims 4, gives 3
        geoKids.push_back(leaf(0x10007, shortVerts));
        geoKids.push_back(leaf(0x10004, indexBlob(2)));
        Bytes file = assemble({ mesh("bad", { { material("Skydome.fx", {}), container(0x10000, geoKids) } }) });
        bool threw = false;
        try { parseBytes(file); } catch (BadFileException&) { threw = true; } catch (...) {}
        CHECK(threw, "vertex stride mismatch -> BadFileException");
    }
    {
        // vertexCount > 0xFFFF -> BadFileException (count check fires before alloc)
        std::vector<Bytes> geoKids;
        geoKids.push_back(countsChunk(70000, 2));
        Bytes f; cstr(f, "alD3dVertNU2C"); geoKids.push_back(leaf(0x10002, f));
        Bytes tinyVerts; tinyVerts.resize(16, 0); geoKids.push_back(leaf(0x10007, tinyVerts));
        geoKids.push_back(leaf(0x10004, indexBlob(2)));
        Bytes file = assemble({ mesh("big", { { material("Skydome.fx", {}), container(0x10000, geoKids) } }) });
        bool threw = false;
        try { parseBytes(file); } catch (BadFileException&) { threw = true; } catch (...) {}
        CHECK(threw, "vertexCount > 0xFFFF -> BadFileException");
    }
    {
        // Huge primitiveCount -> BadFileException (bound + 64-bit size check;
        // guards against 32-bit `long` overflow bypassing the check + a giant alloc).
        std::vector<Bytes> geoKids;
        geoKids.push_back(countsChunk(4, 0x80000001u));
        Bytes f; cstr(f, "alD3dVertNU2C"); geoKids.push_back(leaf(0x10002, f));
        geoKids.push_back(leaf(0x10007, vertexBlob(4, false)));  // valid verts
        geoKids.push_back(leaf(0x10004, indexBlob(1)));          // 6 bytes (1 tri)
        Bytes file = assemble({ mesh("ovf", { { material("Skydome.fx", {}), container(0x10000, geoKids) } }) });
        bool threw = false;
        try { parseBytes(file); } catch (BadFileException&) { threw = true; } catch (...) {}
        CHECK(threw, "huge primitiveCount -> BadFileException");
    }
    {
        // No mesh chunk -> WrongFileException
        bool wrong = false;
        try { parseBytes(assemble({ skeletonStub(), connectionsStub() })); }
        catch (WrongFileException&) { wrong = true; } catch (...) {}
        CHECK(wrong, "no mesh chunk -> WrongFileException");
    }
    {
        // Truncated mid-vertex-blob -> ReadException
        Bytes mat = material("Skydome.fx", { texParam("BaseTexture", "x.dds") });
        Bytes geo = geometry(4, 2, "alD3dVertNU2C");
        Bytes file = assemble({ mesh("trunc", { { mat, geo } }) });
        file.resize(file.size() - 200);  // chop the tail
        bool threw = false;
        try { parseBytes(file); } catch (ReadException&) { threw = true; } catch (...) {}
        CHECK(threw, "truncated file -> ReadException");
    }

    std::printf("\n=== AloModel: %s ===\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
