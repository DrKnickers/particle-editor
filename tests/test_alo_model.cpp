// Unit tests for the static-mesh .alo decoder (src/AloModel.cpp).
//
// Builds synthetic .alo byte streams in memory (via the documented chunk wire
// format) and feeds them through LoadAloModel -- no game assets are committed
// or required. Covers happy paths, multi-submesh / multi-mesh, tolerant
// skipping of non-mesh root chunks, the legacy 0x10005 skip, and the malformed
// cases the parser must reject. Standalone console exe; see
// tests/build_test_alo_model.bat.

#include "AloModel.h"
#include "ReferenceObjectMesh.h"   // [refmesh] shadow-bucket routing test
#include "managers.h"              // IFileManager (StubFileManager below)
#include "files.h"
#include "exceptions.h"

#include <array>
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
// Build a geometry chunk with explicit float3 positions for each vertex (all in
// the 144B MASTER_VERTEX stride). positions.size() must equal nVerts; other
// fields (normal, uv, …) are zeroed. Used by the AABB-exclusion test to place
// shadow-volume verts at a known far distance so we can assert they don't widen
// the visible bounding box.
static Bytes geometryWithPositions(const std::vector<std::array<float,3>>& positions,
                                   uint32_t prims, const std::string& fmt)
{
    const uint32_t n = (uint32_t)positions.size();
    Bytes vdata; vdata.resize((size_t)n * 144, 0);
    for (uint32_t i = 0; i < n; ++i)
        std::memcpy(&vdata[(size_t)i * 144], positions[i].data(), 12);   // pos @0

    std::vector<Bytes> kids;
    kids.push_back(countsChunk(n, prims));
    Bytes f; cstr(f, fmt); kids.push_back(leaf(0x10002, f));
    kids.push_back(leaf(0x10007, vdata));
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
// 0x402 MESH-INFO with the hidden/collision flags set: subMeshCount(4) +
// bbox_min[3](12) + bbox_max[3](12) + unused(4) + isHidden(4)@32 + isCollision(4)@36,
// padded to the writer's fixed 128 B. `payloadBytes` lets a test truncate the
// chunk below the 40-B minimum to exercise the tolerant-defaults path.
static Bytes meshInfoChunk(uint32_t subCount, bool hidden, bool collision, size_t payloadBytes = 128) {
    Bytes p;
    u32le(p, subCount);
    for (int i = 0; i < 6; ++i) f32le(p, 0.0f);   // bbox min + max
    u32le(p, 0);                                  // unused @28
    u32le(p, hidden ? 1u : 0u);                   // isHidden @32
    u32le(p, collision ? 1u : 0u);                // isCollision @36
    p.resize(payloadBytes, 0);                    // pad (or, if < 40, truncate)
    return leaf(0x0402, p);
}
static Bytes meshFlagged(const std::string& name,
                         const std::vector<std::pair<Bytes, Bytes>>& subs,
                         bool hidden, bool collision, size_t infoBytes = 128) {
    std::vector<Bytes> kids;
    Bytes nm; cstr(nm, name); kids.push_back(leaf(0x0401, nm));
    kids.push_back(meshInfoChunk((uint32_t)subs.size(), hidden, collision, infoBytes));
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

// real-skeleton + connection builders (vs the empty stubs above).
// Bone data: 0x205 = parentIndex(4)+visible(4)+matrix[12](48) = 56 B;
//            0x206 = + billboardMode(4) before the matrix = 60 B.
static Bytes boneData(uint32_t parent, uint32_t visible, bool useBillboard, uint32_t billboard, const float m[12]) {
    Bytes p; u32le(p, parent); u32le(p, visible);
    if (useBillboard) u32le(p, billboard);
    for (int i = 0; i < 12; ++i) f32le(p, m[i]);
    return leaf(useBillboard ? 0x0206 : 0x0205, p);
}
static Bytes bone(const std::string& name, uint32_t parent, uint32_t visible,
                  bool useBillboard, uint32_t billboard, const float m[12]) {
    Bytes nm; cstr(nm, name);
    return container(0x0202, { leaf(0x0203, nm), boneData(parent, visible, useBillboard, billboard, m) });
}
static Bytes skeleton(const std::vector<Bytes>& bones) {
    Bytes info; u32le(info, (uint32_t)bones.size()); info.resize(128, 0);
    std::vector<Bytes> kids; kids.push_back(leaf(0x0201, info));
    for (const auto& b : bones) kids.push_back(b);
    return container(0x0200, kids);
}
static Bytes connectionObj(uint32_t objIdx, uint32_t boneIdx) {
    Bytes p, v;
    u32le(v, objIdx);  mini(p, 2, v); v.clear();
    u32le(v, boneIdx); mini(p, 3, v);
    return leaf(0x0602, p);
}
static Bytes connections(const std::vector<Bytes>& objs) {
    Bytes counts, v, v2;
    u32le(v,  (uint32_t)objs.size()); mini(counts, 1, v);   // nConnections
    u32le(v2, 0);                     mini(counts, 4, v2);  // nProxies
    std::vector<Bytes> kids; kids.push_back(leaf(0x0601, counts));
    for (const auto& o : objs) kids.push_back(o);
    return container(0x0600, kids);
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

// [refmesh] Minimal IFileManager returning an in-memory IFile over a fixed byte
// blob, for the device-free ReferenceObjectMesh::Load path (Load calls only
// getFile). Each getFile hands back a FRESH MemoryFile at refcount 1 so Load's
// single Release() frees it.
class StubFileManager : public IFileManager {
public:
    explicit StubFileManager(Bytes bytes) : m_bytes(std::move(bytes)) {}
    IFile* getFile(const std::string& /*path*/) override {
        MemoryFile* mf = new MemoryFile();
        if (!m_bytes.empty()) mf->write(m_bytes.data(), (unsigned long)m_bytes.size());
        mf->seek(0);
        return mf;
    }
private:
    Bytes m_bytes;
};

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
        std::printf("  mesh[%zu] \"%s\" submeshes=%zu hidden=%d collision=%d\n",
            mi, me.name.c_str(), me.subMeshes.size(), me.hidden ? 1 : 0, me.collision ? 1 : 0);
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
    std::printf("bones: %zu\n", m.bones.size());
    for (size_t bi = 0; bi < m.bones.size(); ++bi) {
        const AloBone& b = m.bones[bi];
        // translation row = (col0[3], col1[3], col2[3]) = matrix[3], [7], [11].
        std::printf("  bone[%zu] \"%s\" parent=%u vis=%d bb=%u T=(%g,%g,%g)\n",
            bi, b.name.c_str(), b.parentIndex, b.visible ? 1 : 0, b.billboardMode,
            b.matrix[3], b.matrix[7], b.matrix[11]);
    }
    std::printf("connections: %zu\n", m.connections.size());
    for (size_t ci = 0; ci < m.connections.size(); ++ci)
        std::printf("  conn[%zu] obj=%u -> bone=%u\n", ci, m.connections[ci].objectIndex, m.connections[ci].boneIndex);
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

    // ---- 0x402 mesh-info: hidden + collision flags --------------------------
    std::printf("[mesh-info-0x402]\n");
    {
        auto sub = []() -> std::pair<Bytes, Bytes> {
            return { material("MeshCollision.fx", {}), geometry(3, 1, "alD3dVertN") };
        };
        // default (all-zero 0x402 via mesh()) -> visible, non-collision.
        {
            AloModel m = parseBytes(assemble({ mesh("vis", { sub() }) }));
            CHECK(m.meshes.size() == 1 && !m.meshes[0].hidden && !m.meshes[0].collision,
                  "all-zero 0x402 -> visible, non-collision");
        }
        // isHidden = 1.
        {
            AloModel m = parseBytes(assemble({ meshFlagged("hid", { sub() }, true, false) }));
            CHECK(m.meshes.size() == 1 && m.meshes[0].hidden, "isHidden=1 decodes hidden");
            CHECK(!m.meshes[0].collision, "isHidden=1 leaves collision false");
        }
        // isCollision = 1 (and visible).
        {
            AloModel m = parseBytes(assemble({ meshFlagged("col", { sub() }, false, true) }));
            CHECK(m.meshes.size() == 1 && m.meshes[0].collision && !m.meshes[0].hidden,
                  "isCollision=1 decodes collision, stays visible");
        }
        // both flags set.
        {
            AloModel m = parseBytes(assemble({ meshFlagged("both", { sub() }, true, true) }));
            CHECK(m.meshes[0].hidden && m.meshes[0].collision, "both flags decode");
        }
        // short 0x402 (< 40 B) -> tolerant defaults (visible), no throw.
        {
            AloModel m = parseBytes(assemble({ meshFlagged("short", { sub() }, true, true, 16) }));
            CHECK(m.meshes.size() == 1 && !m.meshes[0].hidden && !m.meshes[0].collision,
                  "short 0x402 keeps visible defaults (tolerant)");
        }
    }

    // ---- AloClassifyShader: phase/blend buckets (FoC corpus) ----------------
    std::printf("[classify-shader]\n");
    {
        CHECK(AloClassifyShader("MeshGloss.fx")        == ALO_RC_OPAQUE,   "MeshGloss -> opaque");
        CHECK(AloClassifyShader("MeshBumpColorize.fx") == ALO_RC_OPAQUE,   "MeshBumpColorize -> opaque");
        CHECK(AloClassifyShader("MeshCollision.fx")    == ALO_RC_OPAQUE,   "MeshCollision -> opaque (blue)");
        CHECK(AloClassifyShader("MeshAdditive.fx")     == ALO_RC_ADDITIVE, "MeshAdditive -> additive");
        CHECK(AloClassifyShader("MeshAdditiveVColor.fx")== ALO_RC_ADDITIVE,"MeshAdditiveVColor -> additive");
        CHECK(AloClassifyShader("MeshShield.fx")       == ALO_RC_ADDITIVE, "MeshShield -> additive (name mismatch)");
        CHECK(AloClassifyShader("MeshAlpha.fx")        == ALO_RC_ALPHA,    "MeshAlpha -> alpha");
        CHECK(AloClassifyShader("MeshAlphaGloss.fx")   == ALO_RC_ALPHA,    "MeshAlphaGloss -> alpha");
        CHECK(AloClassifyShader("MeshHeat.fx")         == ALO_RC_HEAT,     "MeshHeat -> heat");
        CHECK(AloClassifyShader("MeshShadowVolume.fx") == ALO_RC_SHADOW,   "MeshShadowVolume -> shadow");
        CHECK(AloClassifyShader("MeshOccludedUnit.fx") == ALO_RC_OCCLUDED, "MeshOccludedUnit -> occluded");
        CHECK(AloClassifyShader("MESHSHIELD.FX")       == ALO_RC_ADDITIVE, "case-insensitive");
        // AloIsNonVisibleShader: shadow/occluded/heat true; collision/opaque/blend false.
        CHECK(AloIsNonVisibleShader("MeshShadowVolume.fx"), "shadow is non-visible");
        CHECK(AloIsNonVisibleShader("MeshHeat.fx"),         "heat is non-visible (v1-deferred)");
        CHECK(!AloIsNonVisibleShader("MeshCollision.fx"),   "collision is NOT non-visible (drawn blue)");
        CHECK(!AloIsNonVisibleShader("MeshShield.fx"),      "shield is visible (transparent)");
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

    // ---- skeleton (0x200) + connections (0x600) decode ---------------
    std::printf("[lt7-skeleton]\n");
    {
        float rootM[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };   // identity
        // Base bone: a 90deg rotation + a z translation, stored COLUMN-MAJOR:
        //   col0 @ [0..3] = (0,1,0,0), col1 @ [4..7] = (-1,0,0,0), col2 @ [8..11] = (0,0,1,6.5)
        float baseM[12] = { 0,1,0,0,  -1,0,0,0,  0,0,1,6.5f };
        Bytes skel = skeleton({
            bone("Root", 0xFFFFFFFFu, 1, /*billboard*/true,  7, rootM),   // 0x206 path
            bone("Base", 0,           1, /*billboard*/false, 0, baseM),   // 0x205 path
        });
        Bytes conn = connections({ connectionObj(/*objIdx*/0, /*boneIdx*/1) });  // mesh 0 -> bone 1
        Bytes mat  = material("MeshBumpColorize.fx", {});
        Bytes geo  = geometry(4, 2, "alD3dVertNU2U3U3");
        AloModel m = parseBytes(assemble({ skel, mesh("turret", { { mat, geo } }), conn }));

        CHECK(m.bones.size() == 2, "2 bones decoded");
        CHECK(m.bones.size() == 2 && m.bones[0].name == "Root", "bone0 name=Root");
        CHECK(m.bones.size() == 2 && m.bones[0].parentIndex == 0xFFFFFFFFu, "bone0 root sentinel stored verbatim");
        CHECK(m.bones.size() == 2 && m.bones[0].billboardMode == 7, "bone0 billboardMode from 0x206");
        CHECK(m.bones.size() == 2 && m.bones[1].name == "Base", "bone1 name=Base");
        CHECK(m.bones.size() == 2 && m.bones[1].parentIndex == 0, "bone1 parentIndex=0 (Root)");
        CHECK(m.bones.size() == 2 && m.bones[1].billboardMode == 0, "bone1 billboardMode=0 (0x205, no billboard)");
        CHECK(m.bones.size() == 2 && m.bones[1].matrix[0] == 0.0f && m.bones[1].matrix[1] == 1.0f, "bone1 matrix col0 verbatim (column-major)");
        CHECK(m.bones.size() == 2 && m.bones[1].matrix[11] == 6.5f, "bone1 matrix translation z @ [11] verbatim");
        CHECK(m.connections.size() == 1, "1 connection decoded");
        CHECK(m.connections.size() == 1 && m.connections[0].objectIndex == 0, "connection objectIndex=0");
        CHECK(m.connections.size() == 1 && m.connections[0].boneIndex == 1, "connection boneIndex=1 (Base)");
        CHECK(m.meshes.size() == 1, "mesh still decoded alongside skeleton/connections");
    }
    std::printf("[lt7-tolerant]\n");
    {
        // (a) No skeleton / connections at all -> empty vectors, mesh still loads.
        AloModel m = parseBytes(assemble({ mesh("plain", { { material("MeshGloss.fx", {}), geometry(4, 2, "alD3dVertNU2") } }) }));
        CHECK(m.bones.empty() && m.connections.empty(), "no skeleton/connections -> empty vectors");
        CHECK(m.meshes.size() == 1, "mesh loads without a skeleton");

        // (b) A bone-data chunk of an UNEXPECTED size must be TOLERATED (keep
        // identity defaults), never throw -- these chunks loaded before game-object support.
        Bytes nm; cstr(nm, "Weird");
        Bytes badData; u32le(badData, 5); badData.resize(40, 0);   // 40 B: neither 56 nor 60
        Bytes badSkel = container(0x0200, { container(0x0202, { leaf(0x0203, nm), leaf(0x0206, badData) }) });
        bool threw = false; AloModel m2;
        try { m2 = parseBytes(assemble({ badSkel, mesh("t", { { material("X.fx", {}), geometry(4, 2, "alD3dVertN") } }) })); }
        catch (...) { threw = true; }
        CHECK(!threw, "wrong-size bone data tolerated (no throw)");
        CHECK(!threw && m2.bones.size() == 1 && m2.bones[0].name == "Weird", "tolerant bone still recorded (name)");
        CHECK(!threw && m2.bones.size() == 1 && m2.bones[0].parentIndex == 0 && m2.bones[0].matrix[0] == 1.0f, "tolerant bone keeps identity defaults");
    }

    // ---- [refmesh] ReferenceObjectMesh::Load shadow-volume routing ----------
    // A shadow sub-mesh (MeshShadowVolume.fx -> ALO_RC_SHADOW) must be KEPT in a
    // SEPARATE ShadowSubMeshes() bucket (for a later stencil-shadow pass), NOT in
    // the visible SubMeshes() list (it would draw as a solid hull).
    std::printf("[refmesh-shadow-routing]\n");
    {
        std::pair<Bytes, Bytes> visible = { material("MeshGloss.fx", { texParam("BaseTexture", "hull.dds") }),
                                            geometry(8, 4, "alD3dVertN") };
        std::pair<Bytes, Bytes> shadow  = { material("MeshShadowVolume.fx", {}),
                                            geometry(6, 2, "alD3dVertN") };
        Bytes file = assemble({ mesh("unit", { visible, shadow }) });

        StubFileManager fm(file);
        ReferenceObjectMesh rom;
        bool ok = rom.Load(fm, "unit.alo");
        CHECK(ok, "Load returns true (has visible geometry)");
        CHECK(rom.SubMeshes().size() == 1, "1 visible sub-mesh (shadow excluded from visible list)");
        CHECK(rom.ShadowSubMeshes().size() == 1, "1 shadow sub-mesh kept in separate bucket");
        CHECK(rom.ShadowSubMeshes().size() == 1 &&
              rom.ShadowSubMeshes()[0].shaderName == "MeshShadowVolume.fx",
              "shadow bucket holds the MeshShadowVolume.fx sub-mesh");
    }
    // A model with NO shadow sub-meshes yields an EMPTY shadow bucket.
    std::printf("[refmesh-no-shadow]\n");
    {
        Bytes file = assemble({ mesh("plain", { { material("MeshGloss.fx", { texParam("BaseTexture", "hull.dds") }),
                                                  geometry(8, 4, "alD3dVertN") } }) });
        StubFileManager fm(file);
        ReferenceObjectMesh rom;
        bool ok = rom.Load(fm, "plain.alo");
        CHECK(ok, "Load returns true (no-shadow model)");
        CHECK(rom.SubMeshes().size() == 1, "1 visible sub-mesh");
        CHECK(rom.ShadowSubMeshes().empty(), "no-shadow model -> empty shadow bucket");
    }

    // ---- [refmesh-clear-reuse] Clear()-on-reuse drains the shadow bucket ---------
    // Loading a shadow-bearing model into a ReferenceObjectMesh then reloading a
    // shadow-free model into the SAME instance must yield an empty shadow bucket.
    // Guards stale-bucket regression on object switch (Load calls Clear internally).
    std::printf("[refmesh-clear-reuse]\n");
    {
        // First load: model WITH a MeshShadowVolume.fx sub-mesh.
        std::pair<Bytes, Bytes> vis1   = { material("MeshGloss.fx", {}), geometry(4, 2, "alD3dVertN") };
        std::pair<Bytes, Bytes> shad1  = { material("MeshShadowVolume.fx", {}), geometry(3, 1, "alD3dVertN") };
        Bytes file1 = assemble({ mesh("with_shadow", { vis1, shad1 }) });

        StubFileManager fm1(file1);
        ReferenceObjectMesh rom;
        rom.Load(fm1, "with_shadow.alo");
        CHECK(rom.ShadowSubMeshes().size() == 1, "clear-reuse: first load populates shadow bucket");

        // Second load: a DIFFERENT model with NO shadow sub-mesh into the same instance.
        Bytes file2 = assemble({ mesh("no_shadow", { { material("MeshGloss.fx", {}), geometry(6, 2, "alD3dVertN") } }) });
        StubFileManager fm2(file2);
        bool ok2 = rom.Load(fm2, "no_shadow.alo");

        CHECK(ok2, "clear-reuse: second load returns true (has visible geometry)");
        CHECK(rom.ShadowSubMeshes().empty(), "clear-reuse: shadow bucket drained after reloading shadow-free model");
        CHECK(rom.SubMeshes().size() == 1, "clear-reuse: visible bucket reflects new (shadow-free) model");
    }

    // ---- [refmesh-rskin-shadow] RSkinShadowVolume.fx routes to shadow bucket ----
    // RSkinShadowVolume.fx is classified ALO_RC_SHADOW by AloClassifyShader, so it
    // must land in ShadowSubMeshes() (not SubMeshes(), not dropped). The vertex format
    // "alD3dVertRSkinN" makes isRSkinFormat() true, so gpu.skinned == true as well.
    std::printf("[refmesh-rskin-shadow]\n");
    {
        std::pair<Bytes, Bytes> vis    = { material("MeshGloss.fx", {}), geometry(4, 2, "alD3dVertN") };
        // RSkin shadow: shader = RSkinShadowVolume.fx, format = alD3dVertRSkinN
        // (1-bone rigid-skinning prefix -> isRSkinFormat true -> skinned flag set).
        std::pair<Bytes, Bytes> rskin  = { material("RSkinShadowVolume.fx", {}),
                                           geometry(3, 1, "alD3dVertRSkinN") };
        Bytes file = assemble({ mesh("rskin_shad", { vis, rskin }) });

        StubFileManager fm(file);
        ReferenceObjectMesh rom;
        bool ok = rom.Load(fm, "rskin_shad.alo");

        CHECK(ok, "rskin-shadow: Load returns true (visible sub-mesh present)");
        CHECK(rom.SubMeshes().size() == 1, "rskin-shadow: RSkinShadowVolume.fx NOT in visible list");
        CHECK(rom.ShadowSubMeshes().size() == 1, "rskin-shadow: RSkinShadowVolume.fx lands in shadow bucket");
        CHECK(rom.ShadowSubMeshes().size() == 1 &&
              rom.ShadowSubMeshes()[0].shaderName == "RSkinShadowVolume.fx",
              "rskin-shadow: shader name preserved in shadow bucket");
        // The alD3dVertRSkinN format triggers isRSkinFormat -> skinned flag set.
        // Asserting skinned==true verifies the RSkin path (not just shader-name routing).
        CHECK(rom.ShadowSubMeshes().size() == 1 &&
              rom.ShadowSubMeshes()[0].skinned == true,
              "rskin-shadow: shadow sub-mesh has skinned==true (alD3dVertRSkin* format)");
    }

    // ---- [refmesh-aabb-excludes-shadow] AABB excludes shadow-volume geometry ---
    // GetBoundingBox must be computed over VISIBLE geometry only; a shadow hull
    // whose verts extend FAR beyond the visible mesh must NOT widen the AABB.
    // Vertex positions are controllable via geometryWithPositions, so this test runs.
    std::printf("[refmesh-aabb-excludes-shadow]\n");
    {
        // Visible sub-mesh: 3 vertices tightly clustered within [-1,1]^3.
        std::vector<std::array<float,3>> visPos = {
            {{  0.5f,  0.5f,  0.5f }},
            {{ -0.5f,  0.0f,  0.0f }},
            {{  0.0f, -0.5f,  0.5f }},
        };
        // Shadow sub-mesh: 3 vertices placed FAR outside -- at ±100 on every axis.
        // If the AABB loop accidentally includes shadow verts, the bounds blow up.
        std::vector<std::array<float,3>> shadPos = {
            {{ 100.0f,  100.0f,  100.0f }},
            {{-100.0f, -100.0f, -100.0f }},
            {{ 100.0f, -100.0f,  100.0f }},
        };
        std::pair<Bytes, Bytes> vis  = { material("MeshGloss.fx", {}),
                                         geometryWithPositions(visPos,  1, "alD3dVertN") };
        std::pair<Bytes, Bytes> shad = { material("MeshShadowVolume.fx", {}),
                                         geometryWithPositions(shadPos, 1, "alD3dVertN") };
        Bytes file = assemble({ mesh("aabb_test", { vis, shad }) });

        StubFileManager fm(file);
        ReferenceObjectMesh rom;
        bool ok = rom.Load(fm, "aabb_test.alo");
        CHECK(ok, "aabb-excludes-shadow: Load returns true");

        D3DXVECTOR3 mn, mx;
        bool hasBounds = rom.GetBoundingBox(mn, mx);
        CHECK(hasBounds, "aabb-excludes-shadow: GetBoundingBox returns true");
        // The AABB must reflect the VISIBLE extent only (no bone transform -> identity).
        // Expected min = (-0.5, -0.5, 0.0), max = (0.5, 0.5, 0.5) (within epsilon).
        const float eps = 1e-5f;
        CHECK(hasBounds && mn.x > -1.0f && mn.y > -1.0f && mn.z > -1.0f,
              "aabb-excludes-shadow: min not blown out by shadow verts (all > -1)");
        CHECK(hasBounds && mx.x <  1.0f && mx.y <  1.0f && mx.z <  1.0f,
              "aabb-excludes-shadow: max not blown out by shadow verts (all < 1)");
        CHECK(hasBounds && std::abs(mn.x - (-0.5f)) < eps && std::abs(mn.y - (-0.5f)) < eps,
              "aabb-excludes-shadow: min.xy == visible extent min (-0.5, -0.5)");
        CHECK(hasBounds && std::abs(mx.x -   0.5f)  < eps && std::abs(mx.y -  0.5f) < eps,
              "aabb-excludes-shadow: max.xy == visible extent max (0.5, 0.5)");
        (void)eps;
    }

    std::printf("\n=== AloModel: %s ===\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
