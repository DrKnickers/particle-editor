// dump_bones.cpp -- dump an .alo's 0x200 skeleton + 0x600 connections: per bone
// the name, parent, billboardMode (0x205 vs 0x206) and the local 4x3 matrix; per
// connection the mesh-ordinal -> bone-index binding; plus the mesh list. Pure
// (AloModel + files; no engine / D3D) -> fast + CI-able.
//
// Built to diagnose the vanilla space-dome "white hairline": it revealed the
// `sun_billboard` sub-mesh rides a billboard bone (billboardMode=7) the skydome
// renderer was ignoring, so the flat additive sun quad drew un-billboarded at the
// dome centre and collapsed to a 1px line edge-on. Reusable for any "skydome /
// dome sub-mesh is misplaced / not facing camera" report.
//
//   usage: dump_bones.exe <path.alo>

#include "AloModel.h"
#include "files.h"
#include "exceptions.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: dump_bones <path.alo>\n"); return 2; }
    std::string p(argv[1]); std::wstring w(p.begin(), p.end());
    IFile* f = nullptr;
    try { f = new PhysicalFile(w, PhysicalFile::READ); } catch (...) { printf("open fail\n"); return 1; }
    AloModel m;
    try { m = LoadAloModel(f); f->Release(); } catch (...) { printf("parse fail\n"); return 1; }

    printf("=== %s ===\nmeshes=%zu bones=%zu connections=%zu\n",
           argv[1], m.meshes.size(), m.bones.size(), m.connections.size());
    for (size_t i = 0; i < m.meshes.size(); ++i)
        printf("  mesh[%zu] name=%-16s subs=%zu\n", i, m.meshes[i].name.c_str(), m.meshes[i].subMeshes.size());
    for (size_t i = 0; i < m.bones.size(); ++i)
    {
        const AloBone& b = m.bones[i];
        printf("  bone[%zu] name=%-16s parent=%d billboardMode=%u  T=(%.3f,%.3f,%.3f)\n",
               i, b.name.c_str(), (int)b.parentIndex, b.billboardMode,
               b.matrix[3], b.matrix[7], b.matrix[11]);
    }
    for (size_t i = 0; i < m.connections.size(); ++i)
        printf("  conn[%zu] mesh(objectIndex)=%u -> bone(boneIndex)=%u\n",
               i, m.connections[i].objectIndex, m.connections[i].boneIndex);
    return 0;
}
