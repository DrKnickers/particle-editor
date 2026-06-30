// dump_refbounds.cpp -- drive the EDITOR'S OWN ReferenceObjectMesh::Load on a loose
// .alo and print the object-space AABB it computes (placement + skinned-bind-pose
// aware -- the EXACT geometry the reference-object renderer draws). Unlike a raw
// per-vertex AABB tool, this runs the same bone-placement + RSkin handling the live
// render uses, so its min/max Z is the height the model actually draws at.
//
// Built to root-cause the "reference object floats above the ground" report: it
// answers whether the editor itself places a land unit's feet at ~0 (model is fine,
// look elsewhere) or lifts them (a model-processing offset, here in the editor's space).
//
//   usage: dump_refbounds.exe <path.alo>
//
// Pure CPU: ReferenceObjectMesh::Load needs no D3D device (Resolve/CreateBuffers do).

#include "ReferenceObjectMesh.h"
#include "managers.h"   // IFileManager
#include "files.h"

#include <cstdio>
#include <string>

// Minimal IFileManager: open whatever physical path Load asks for. We feed Load the
// full physical path directly, so getFile just wraps it in a PhysicalFile.
class LoosePathManager : public IFileManager
{
public:
    IFile* getFile(const std::string& path) override
    {
        std::wstring w(path.begin(), path.end());
        try { return new PhysicalFile(w, PhysicalFile::READ); }
        catch (...) { return nullptr; }
    }
};

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: dump_refbounds <path.alo>\n"); return 2; }

    LoosePathManager fm;
    ReferenceObjectMesh mesh;
    // Pass the physical path straight through (our getFile opens it verbatim).
    if (!mesh.Load(fm, argv[1], {}))
    {
        printf("Load FAILED for %s\n", argv[1]);
        return 1;
    }

    D3DXVECTOR3 mn, mx;
    bool hasBox = mesh.GetBoundingBox(mn, mx);

    printf("=== %s ===\n", argv[1]);
    printf("kept sub-meshes: %zu   skippedSkinned=%d\n",
           mesh.SubMeshes().size(), mesh.SkippedSkinned() ? 1 : 0);

    int rigid = 0, skinned = 0;
    float minPlacementZ = 1e30f, maxPlacementZ = -1e30f;
    printf("per-sub-mesh (placement-applied Z, exactly as bounds accumulate):\n");
    int si = 0;
    for (const RefSubMeshGpu& s : mesh.SubMeshes())
    {
        if (s.skinned) ++skinned; else ++rigid;
        const float pz = s.placement._43;   // translation Z of the placement matrix
        if (pz < minPlacementZ) minPlacementZ = pz;
        if (pz > maxPlacementZ) maxPlacementZ = pz;

        // This sub-mesh's own object-space min/max Z, placement-applied (skinned ->
        // identity placement, same as ReferenceObjectMesh's bounds accumulation).
        float zmin = 1e30f, zmax = -1e30f;
        const unsigned char* vb = s.vertexBytes.data();
        for (uint32_t v = 0; v < s.vertexCount; ++v)
        {
            const float* p = reinterpret_cast<const float*>(vb + (size_t)v * s.stride);
            D3DXVECTOR3 pos(p[0], p[1], p[2]), placed;
            D3DXVec3TransformCoord(&placed, &pos, &s.placement);
            if (placed.z < zmin) zmin = placed.z;
            if (placed.z > zmax) zmax = placed.z;
        }
        printf("  [%d] %-7s rc=%d placZ=%8.3f  Zrange=[%8.3f .. %8.3f]  shader=%s\n",
               si++, s.skinned ? "SKIN" : "rigid", (int)s.renderClass, pz,
               zmin, zmax, s.shaderName.c_str());
    }
    printf("rigid=%d skinned=%d   placement Z range=[%.3f .. %.3f]\n",
           rigid, skinned, minPlacementZ, maxPlacementZ);

    if (hasBox)
    {
        printf("object-space AABB:\n");
        printf("  min = (%.4f, %.4f, %.4f)\n", mn.x, mn.y, mn.z);
        printf("  max = (%.4f, %.4f, %.4f)\n", mx.x, mx.y, mx.z);
        printf("  >>> FEET (min Z) = %.4f   TOP (max Z) = %.4f   height = %.4f\n",
               mn.z, mx.z, mx.z - mn.z);
    }
    else
    {
        printf("no bounds (empty kept geometry)\n");
    }
    return 0;
}
