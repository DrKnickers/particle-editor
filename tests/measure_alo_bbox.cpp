// Throwaway dev-box measurement: load a real .alo and print the vertex AABB
// (object-space) over every sub-mesh, to corroborate the unit scale.
// Not part of CI; not committed as a permanent test. Position floats live at
// offset 0 of the 144-byte MASTER_VERTEX stride (AloModel.h:113).
//
//   build_test_alo_model.bat-style invocation; see the inline note below.
//   usage: measure_alo_bbox.exe <path-to.alo>

#include "AloModel.h"
#include "files.h"
#include "exceptions.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <sstream>

typedef std::array<float, 3> V3;

// Collect every .alo vertex position (pos@0, 144B stride) of the named mesh.
static std::vector<V3> aloMeshVerts(const AloModel& m, const std::string& name)
{
    std::vector<V3> out;
    for (const AloMesh& me : m.meshes)
    {
        if (me.name != name) continue;
        for (const AloSubMesh& sm : me.subMeshes)
        {
            const unsigned char* vb = sm.rawVertexBytes.data();
            size_t n = sm.rawVertexBytes.size() / kAloVertexStride;
            for (size_t v = 0; v < n; ++v)
            {
                const float* p = reinterpret_cast<const float*>(vb + v * kAloVertexStride);
                out.push_back({ p[0], p[1], p[2] });
            }
        }
    }
    return out;
}

// Read the .max vert dump (dump_max_verts.ms output) for the named mesh.
static std::vector<V3> maxMeshVerts(const std::string& path, const std::string& name)
{
    std::vector<V3> out;
    std::ifstream f(path);
    std::string line;
    bool in = false;
    long remaining = 0;
    while (std::getline(f, line))
    {
        std::istringstream ss(line);
        std::string tok; ss >> tok;
        if (tok == "MESH")
        {
            std::string nm; ss >> nm;
            std::string vtok; long n = 0; ss >> vtok >> n;   // "verts N"
            in = (nm == name);
            remaining = in ? n : 0;
            continue;
        }
        if (in && remaining > 0)
        {
            std::istringstream vs(line);
            float x, y, z;
            if (vs >> x >> y >> z) { out.push_back({ x, y, z }); --remaining; }
        }
    }
    return out;
}

// Directed nearest-neighbour residual: for each vert in `a`, min distance to any
// vert in `b`. Reports max and RMS. If export is 1:1, every .alo vert coincides
// with a .max vert -> residual ~ float epsilon. A scale would blow it up.
static void residual(const std::vector<V3>& a, const std::vector<V3>& b,
                     double& maxD, double& rms)
{
    maxD = 0.0; double sumSq = 0.0;
    for (const V3& pa : a)
    {
        double best = DBL_MAX;
        for (const V3& pb : b)
        {
            double dx = pa[0]-pb[0], dy = pa[1]-pb[1], dz = pa[2]-pb[2];
            double d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < best) best = d2;
        }
        double d = std::sqrt(best);
        if (d > maxD) maxD = d;
        sumSq += best;
    }
    rms = a.empty() ? 0.0 : std::sqrt(sumSq / a.size());
}

static int compareMode(const AloModel& m, const char* meshName, const char* maxFile, double scale)
{
    std::vector<V3> alo = aloMeshVerts(m, meshName);
    std::vector<V3> mx  = maxMeshVerts(maxFile, meshName);
    std::printf("=== per-vertex compare: mesh \"%s\" ===\n", meshName);
    if (scale != 1.0)
    {
        std::printf("  [negative control] scaling .max verts by %g before compare\n", scale);
        for (V3& v : mx) { v[0] = (float)(v[0]*scale); v[1] = (float)(v[1]*scale); v[2] = (float)(v[2]*scale); }
    }
    std::printf("  .alo verts: %zu   .max verts: %zu\n", alo.size(), mx.size());
    if (alo.empty() || mx.empty()) { std::printf("  MISSING on one side\n"); return 1; }
    double maxAB, rmsAB, maxBA, rmsBA;
    residual(alo, mx, maxAB, rmsAB);
    residual(mx, alo, maxBA, rmsBA);
    std::printf("  .alo -> .max : max NN dist = %.6g   RMS = %.6g\n", maxAB, rmsAB);
    std::printf("  .max -> .alo : max NN dist = %.6g   RMS = %.6g\n", maxBA, rmsBA);
    // Scale sanity: ratio of coordinate magnitudes (mean |pos|) both sides.
    auto meanMag = [](const std::vector<V3>& v){ double s=0; for (auto&p:v) s+=std::sqrt((double)p[0]*p[0]+p[1]*p[1]+p[2]*p[2]); return v.empty()?0.0:s/v.size(); };
    std::printf("  mean |pos|  .alo = %.6g   .max = %.6g   ratio = %.6g\n",
        meanMag(alo), meanMag(mx), meanMag(mx)>0 ? meanMag(alo)/meanMag(mx) : 0.0);
    return 0;
}

static bool loadAloFile(const char* path, AloModel& out)
{
    std::string p(path); std::wstring w(p.begin(), p.end());
    IFile* f = nullptr;
    try { f = new PhysicalFile(w, PhysicalFile::READ); } catch (...) { return false; }
    try { out = LoadAloModel(f); f->Release(); return true; }
    catch (...) { f->Release(); return false; }
}

// [M2] Compare the named mesh's vertices between TWO .alo files (e.g. author copy
// vs the MEG-extracted shipped asset) — provenance check for export-1:1 / result 2.
static int compareAloMode(const AloModel& a, const char* meshName, const char* otherAlo)
{
    AloModel b;
    if (!loadAloFile(otherAlo, b)) { std::printf("cannot load %s\n", otherAlo); return 2; }
    std::vector<V3> va = aloMeshVerts(a, meshName);
    std::vector<V3> vb = aloMeshVerts(b, meshName);
    std::printf("=== .alo <-> .alo compare: mesh \"%s\" ===\n", meshName);
    std::printf("  A verts: %zu   B (%s) verts: %zu\n", va.size(), otherAlo, vb.size());
    if (va.empty() || vb.empty()) { std::printf("  MISSING on one side\n"); return 1; }
    double mAB, rAB, mBA, rBA;
    residual(va, vb, mAB, rAB);
    residual(vb, va, mBA, rBA);
    std::printf("  A -> B : max NN = %.6g   RMS = %.6g\n", mAB, rAB);
    std::printf("  B -> A : max NN = %.6g   RMS = %.6g\n", mBA, rBA);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: measure_alo_bbox <path.alo> [--compare <mesh> <maxverts.txt> [scale]] [--compare-alo <mesh> <other.alo>]\n"); return 2; }

    std::string p(argv[1]);
    std::wstring wpath(p.begin(), p.end());
    IFile* f = nullptr;
    try { f = new PhysicalFile(wpath, PhysicalFile::READ); }
    catch (...) { std::printf("cannot open %s\n", argv[1]); return 2; }

    AloModel m;
    try { m = LoadAloModel(f); }
    catch (wexception& e) { f->Release(); std::wprintf(L"parse failed: %s\n", e.what()); return 1; }
    catch (...) { f->Release(); std::printf("parse failed (unknown)\n"); return 1; }
    f->Release();

    if (argc >= 5 && std::strcmp(argv[2], "--compare") == 0)
    {
        double scale = (argc >= 6) ? std::atof(argv[5]) : 1.0;
        return compareMode(m, argv[3], argv[4], scale);
    }
    if (argc >= 5 && std::strcmp(argv[2], "--compare-alo") == 0)
        return compareAloMode(m, argv[3], argv[4]);

    float gmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float gmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    size_t totalVerts = 0;

    std::printf("meshes: %zu\n", m.meshes.size());
    for (size_t mi = 0; mi < m.meshes.size(); ++mi)
    {
        const AloMesh& me = m.meshes[mi];
        float lmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float lmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        size_t meshVerts = 0;
        bool meshRigid = true;   // a-priori rigid iff no skinned submesh
        for (size_t si = 0; si < me.subMeshes.size(); ++si)
        {
            const AloSubMesh& sm = me.subMeshes[si];
            const bool skinned = AloIsSkinnedVertexFormat(sm.vertexFormatName);
            if (skinned) meshRigid = false;
            std::printf("    sub[%zu] fmt=%s shader=%s skinned=%d\n",
                si, sm.vertexFormatName.c_str(), sm.shaderName.c_str(), skinned ? 1 : 0);
            const unsigned char* vb = sm.rawVertexBytes.data();
            size_t n = sm.rawVertexBytes.size() / kAloVertexStride;
            for (size_t v = 0; v < n; ++v)
            {
                const float* pos = reinterpret_cast<const float*>(vb + v * kAloVertexStride);
                for (int a = 0; a < 3; ++a)
                {
                    if (pos[a] < lmin[a]) lmin[a] = pos[a];
                    if (pos[a] > lmax[a]) lmax[a] = pos[a];
                    if (pos[a] < gmin[a]) gmin[a] = pos[a];
                    if (pos[a] > gmax[a]) gmax[a] = pos[a];
                }
            }
            meshVerts += n;
        }
        totalVerts += meshVerts;
        std::printf("  mesh[%zu] \"%s\" hidden=%d collision=%d verts=%zu\n",
            mi, me.name.c_str(), me.hidden ? 1 : 0, me.collision ? 1 : 0, meshVerts);
        if (meshVerts)
            std::printf("    bbox  min=(%.3f, %.3f, %.3f)  max=(%.3f, %.3f, %.3f)  extent=(%.3f, %.3f, %.3f)\n",
                lmin[0], lmin[1], lmin[2], lmax[0], lmax[1], lmax[2],
                lmax[0] - lmin[0], lmax[1] - lmin[1], lmax[2] - lmin[2]);
    }

    std::printf("\n=== bones (%zu) ===\n", m.bones.size());
    for (size_t bi = 0; bi < m.bones.size(); ++bi)
    {
        const AloBone& b = m.bones[bi];
        // column-major float[12]; translation = matrix[3],[7],[11]
        std::printf("  bone[%zu] \"%s\" parent=%u T=(%.3f, %.3f, %.3f)\n",
            bi, b.name.c_str(), b.parentIndex, b.matrix[3], b.matrix[7], b.matrix[11]);
    }
    std::printf("=== connections (%zu)  [objectIndex -> boneIndex] ===\n", m.connections.size());
    for (size_t ci = 0; ci < m.connections.size(); ++ci)
        std::printf("  obj %u -> bone %u\n", m.connections[ci].objectIndex, m.connections[ci].boneIndex);

    std::printf("\nTOTAL verts=%zu\n", totalVerts);
    if (totalVerts)
        std::printf("OVERALL bbox  min=(%.3f, %.3f, %.3f)  max=(%.3f, %.3f, %.3f)\n"
                    "              extent X=%.3f  Y=%.3f  Z=%.3f\n",
            gmin[0], gmin[1], gmin[2], gmax[0], gmax[1], gmax[2],
            gmax[0] - gmin[0], gmax[1] - gmin[1], gmax[2] - gmin[2]);
    return 0;
}
