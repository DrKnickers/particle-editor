// dump_bounds.cpp -- compute the object-space AABB of an .alo's DRAWN geometry,
// replicating ReferenceObjectMesh::Load's bone placement + bounds exactly (skip
// 0x402-hidden meshes; place each rigid mesh by its 0x602-connected bone's
// accumulated object-space matrix; skinned -> identity bind pose). Pure
// (AloModel + files; no engine / D3D) -> fast + CI-able.
//
// Built to diagnose the reference-object "feet float above the ground" report:
// it prints the lowest drawn vertex Z (m_boundMin.z), so we can tell whether the
// model's feet sit at object-Z 0 (=> the float is an engine placement bug) or
// well above 0 (=> the geometry rests above origin and auto-grounding is the fix).
//
//   usage: dump_bounds.exe <path.alo> [scaleFactor]

#include "AloModel.h"
#include "files.h"
#include "exceptions.h"
#include "MuzzleFlashFilter.h"   // same muzzle-flash hide rule the engine's Load uses

#include <d3dx9.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    // --- verbatim from ReferenceObjectMesh.cpp (keep in lockstep) ---
    D3DXMATRIX boneLocalMatrix(const float m[12])
    {
        D3DXMATRIX r;
        r._11 = m[0]; r._12 = m[4]; r._13 = m[8];  r._14 = 0.0f;
        r._21 = m[1]; r._22 = m[5]; r._23 = m[9];  r._24 = 0.0f;
        r._31 = m[2]; r._32 = m[6]; r._33 = m[10]; r._34 = 0.0f;
        r._41 = m[3]; r._42 = m[7]; r._43 = m[11]; r._44 = 1.0f;
        return r;
    }
    void computeBoneObjectMatrices(const std::vector<AloBone>& bones, std::vector<D3DXMATRIX>& out)
    {
        const size_t n = bones.size();
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            D3DXMATRIX local = boneLocalMatrix(bones[i].matrix);
            const uint32_t p = bones[i].parentIndex;
            if (p < i) D3DXMatrixMultiply(&out[i], &local, &out[p]);
            else       out[i] = local;
        }
    }
    bool isRSkinFormat(const std::string& name) { return name.rfind("alD3dVertRSkin", 0) == 0; }
    std::string toLower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: dump_bounds <path.alo> [scaleFactor]\n"); return 2; }
    const float scale = (argc >= 3) ? (float)atof(argv[2]) : 1.0f;

    std::string p(argv[1]); std::wstring w(p.begin(), p.end());
    IFile* f = nullptr;
    try { f = new PhysicalFile(w, PhysicalFile::READ); } catch (...) { printf("open fail\n"); return 1; }
    AloModel m;
    try { m = LoadAloModel(f); f->Release(); } catch (...) { printf("parse fail\n"); return 1; }

    std::vector<D3DXMATRIX> boneObj;
    computeBoneObjectMatrices(m.bones, boneObj);

    bool has = false;
    D3DXVECTOR3 lo(0, 0, 0), hi(0, 0, 0);
    auto accum = [&](const D3DXVECTOR3& placed) {
        if (!has) { lo = hi = placed; has = true; return; }
        if (placed.x < lo.x) lo.x = placed.x; if (placed.x > hi.x) hi.x = placed.x;
        if (placed.y < lo.y) lo.y = placed.y; if (placed.y > hi.y) hi.y = placed.y;
        if (placed.z < lo.z) lo.z = placed.z; if (placed.z > hi.z) hi.z = placed.z;
    };

    int dropped = 0;
    for (size_t meshIdx = 0; meshIdx < m.meshes.size(); ++meshIdx)
    {
        const AloMesh& mesh = m.meshes[meshIdx];
        if (mesh.hidden)
        {
            if (toLower(mesh.name).find("muzzle") != std::string::npos)
                printf("  (already 0x402-hidden) mesh[%zu] %s\n", meshIdx, mesh.name.c_str());
            continue;
        }

        D3DXMATRIX placement; D3DXMatrixIdentity(&placement);
        std::string connBoneLower;
        for (const AloConnection& c : m.connections)
            if (c.objectIndex == meshIdx)
            {
                if (c.boneIndex < boneObj.size())   placement     = boneObj[c.boneIndex];
                if (c.boneIndex < m.bones.size())   connBoneLower = toLower(m.bones[c.boneIndex].name);
                break;
            }

        // Same muzzle-flash hide the engine's ReferenceObjectMesh::Load applies.
        if (IsMuzzleFlashMesh(toLower(mesh.name), connBoneLower))
        {
            printf("  DROP(muzzle) mesh[%zu] %-18s bone=%s\n", meshIdx, mesh.name.c_str(), connBoneLower.c_str());
            ++dropped;
            continue;
        }

        for (const AloSubMesh& sm : mesh.subMeshes)
        {
            if (sm.rawVertexBytes.empty() || sm.vertexCount == 0 || sm.primitiveCount == 0) continue;
            const AloRenderClass rc = AloClassifyShader(sm.shaderName);
            if (rc == ALO_RC_OCCLUDED || rc == ALO_RC_HEAT || rc == ALO_RC_SHADOW) continue;
            const bool skinned = isRSkinFormat(sm.vertexFormatName);
            if (AloIsSkinnedVertexFormat(sm.vertexFormatName) && !skinned) continue;

            D3DXMATRIX place;
            if (skinned) D3DXMatrixIdentity(&place);
            else         place = placement;
            for (uint32_t v = 0; v < sm.vertexCount; ++v)
            {
                const float* pos = reinterpret_cast<const float*>(
                    sm.rawVertexBytes.data() + (size_t)v * kAloVertexStride);  // pos@0
                D3DXVECTOR3 in(pos[0], pos[1], pos[2]), out;
                D3DXVec3TransformCoord(&out, &in, &place);
                accum(out);
            }
        }
    }

    if (!has) { printf("no drawn geometry\n"); return 1; }
    printf("=== %s  (scale=%.3f) ===\n", argv[1], scale);
    printf("muzzle-flash meshes dropped: %d\n", dropped);
    printf("object-space AABB:\n");
    printf("  min = (%.3f, %.3f, %.3f)\n", lo.x, lo.y, lo.z);
    printf("  max = (%.3f, %.3f, %.3f)\n", hi.x, hi.y, hi.z);
    printf("  size= (%.3f, %.3f, %.3f)\n", hi.x - lo.x, hi.y - lo.y, hi.z - lo.z);
    printf("scaled (x%.3f) lowest vertex Z = %.3f  (this is how far feet sit ABOVE the model origin)\n",
           scale, lo.z * scale);
    printf("=> to rest feet on ground at z=G, translate model by (G - %.3f) in Z\n", lo.z * scale);
    return 0;
}
