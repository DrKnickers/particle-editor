// dump_skydome_uv.cpp -- characterize a skydome .alo's geometry + AUTHORED UVs
// to decide whether its longitude seam needs a wholesale UV remap (#247's
// SphericalReUV, which throws away authored UVs) or just a targeted de-wrap of
// the closure-bridge triangles. Pure: AloModel + ChunkReader + files; no engine,
// no D3D, no GPU -> deterministic, fast, CI-able.
//
//   usage: dump_skydome_uv.exe <path.alo>
//
// Per textured sub-mesh it reports:
//   - counts, format, shader
//   - radius min/mean/max (is it a sphere?), centroid, bbox
//   - authored U / V range + spans (tile counts)
//   - inferred pole axis (|corr| of V with each axis) + whether U tracks the
//     longitude atan2 of the other two axes  -> does the asset's mapping match
//     SphericalReUV's hardcoded pole=Z, u=atan2(y,x) assumption?
//   - triangle U-span histogram + # of "wrap-bridge" triangles (span > half the
//     total U range) -> is the seam a single closure meridian or scattered?
//   - position-duplication: coincident-position groups carrying >1 distinct U
//     (intentional seam/pole duplication) vs single-U (welded interior)
//   - the #247 "shared-edge U-mismatch %": position-shared edges whose U
//     disagrees across the triangles sharing them -> distinguishes a clean
//     sphere + one seam from genuinely incoherent UVs.

#include "AloModel.h"
#include "files.h"
#include "exceptions.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <algorithm>

namespace
{
    struct Vtx { float p[3]; float n[3]; float u, v; };

    bool textured(const std::string& fmt)
    {
        return fmt.find("NU2") != std::string::npos;   // alD3dVertNU2 / alD3dVertNU2C
    }

    double corr(const std::vector<double>& a, const std::vector<double>& b)
    {
        const size_t n = a.size();
        if (n == 0) return 0.0;
        double ma = 0, mb = 0;
        for (size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= n; mb /= n;
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const double da = a[i] - ma, db = b[i] - mb;
            sab += da * db; saa += da * da; sbb += db * db;
        }
        const double den = std::sqrt(saa * sbb);
        return den > 1e-12 ? sab / den : 0.0;
    }

    typedef std::array<long long, 3> PKey;
    PKey posKey(const float p[3], double scale)
    {
        return { (long long)std::llround((double)p[0] * scale),
                 (long long)std::llround((double)p[1] * scale),
                 (long long)std::llround((double)p[2] * scale) };
    }

    void analyze(const std::string& meshName, int subIdx, const AloSubMesh& sm)
    {
        const size_t n = sm.rawVertexBytes.size() / kAloVertexStride;
        if (n == 0) return;

        std::vector<Vtx> V(n);
        float rmin = 1e30f, rmax = 0, rsum = 0, c[3] = { 0,0,0 };
        float bbmin[3] = { 1e30f,1e30f,1e30f }, bbmax[3] = { -1e30f,-1e30f,-1e30f };
        float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
        for (size_t i = 0; i < n; ++i)
        {
            const unsigned char* r = sm.rawVertexBytes.data() + i * kAloVertexStride;
            std::memcpy(V[i].p, r, 12);
            std::memcpy(V[i].n, r + 12, 12);   // NORMAL float3 @12
            std::memcpy(&V[i].u, r + 24, 4);
            std::memcpy(&V[i].v, r + 28, 4);
            const float rad = std::sqrt(V[i].p[0]*V[i].p[0] + V[i].p[1]*V[i].p[1] + V[i].p[2]*V[i].p[2]);
            rmin = (std::min)(rmin, rad); rmax = (std::max)(rmax, rad); rsum += rad;
            for (int a = 0; a < 3; ++a) { c[a] += V[i].p[a]; bbmin[a] = (std::min)(bbmin[a], V[i].p[a]); bbmax[a] = (std::max)(bbmax[a], V[i].p[a]); }
            umin = (std::min)(umin, V[i].u); umax = (std::max)(umax, V[i].u);
            vmin = (std::min)(vmin, V[i].v); vmax = (std::max)(vmax, V[i].v);
        }
        for (int a = 0; a < 3; ++a) c[a] /= (float)n;
        const float rmean = rsum / (float)n;
        const float uspan = umax - umin;

        std::printf("  sub[%d] mesh=\"%s\" fmt=%s shader=%s\n",
                    subIdx, meshName.c_str(), sm.vertexFormatName.c_str(), sm.shaderName.c_str());
        std::printf("    verts=%zu tris=%u\n", n, sm.primitiveCount);
        std::printf("    radius min/mean/max = %.3f / %.3f / %.3f   sphere-spread=%.4f (0=perfect sphere)\n",
                    rmin, rmean, rmax, rmax > 0 ? (rmax - rmin) / rmax : 0.0f);
        std::printf("    centroid=(%.3f,%.3f,%.3f)\n", c[0], c[1], c[2]);
        std::printf("    bbox min=(%.2f,%.2f,%.2f) max=(%.2f,%.2f,%.2f)\n",
                    bbmin[0],bbmin[1],bbmin[2], bbmax[0],bbmax[1],bbmax[2]);
        std::printf("    AUTHORED U=[%.4f,%.4f] span=%.4f (~%.0f tiles)   V=[%.4f,%.4f] span=%.4f\n",
                    umin, umax, uspan, std::round(uspan), vmin, vmax, vmax - vmin);

        // ---- pole-axis inference: |corr| of authored V with each normalized axis ----
        std::vector<double> Vv(n);
        std::vector<double> axis[3] = { std::vector<double>(n), std::vector<double>(n), std::vector<double>(n) };
        std::vector<double> Uu(n);
        for (size_t i = 0; i < n; ++i)
        {
            const float rad = std::sqrt(V[i].p[0]*V[i].p[0] + V[i].p[1]*V[i].p[1] + V[i].p[2]*V[i].p[2]);
            const double inv = rad > 1e-6f ? 1.0 / rad : 0.0;
            Vv[i] = V[i].v; Uu[i] = V[i].u;
            for (int a = 0; a < 3; ++a) axis[a][i] = V[i].p[a] * inv;
        }
        const char* axn[3] = { "X", "Y", "Z" };
        double cV[3] = { std::fabs(corr(Vv, axis[0])), std::fabs(corr(Vv, axis[1])), std::fabs(corr(Vv, axis[2])) };
        int pole = 0; for (int a = 1; a < 3; ++a) if (cV[a] > cV[pole]) pole = a;
        std::printf("    |corr(V,axis)| X=%.3f Y=%.3f Z=%.3f  -> inferred POLE axis = %s\n",
                    cV[0], cV[1], cV[2], axn[pole]);
        // longitude: U vs atan2 of the two non-pole axes
        const int q = (pole + 1) % 3, s = (pole + 2) % 3;
        std::vector<double> lon(n);
        for (size_t i = 0; i < n; ++i) lon[i] = std::atan2((double)V[i].p[s], (double)V[i].p[q]);
        std::printf("    |corr(U, atan2(%s,%s))| = %.3f  (high => U IS the longitude; SphericalReUV-compatible)\n",
                    axn[s], axn[q], std::fabs(corr(Uu, lon)));
        std::printf("    (SphericalReUV ASSUMES pole=Z, u=atan2(Y,X). Mismatch here => the remap distorts this dome.)\n");

        // ---- triangles: U-span histogram + wrap-bridge count ----
        const size_t tris = sm.indexBytes.size() / 2 / 3;
        std::vector<uint16_t> idx(sm.indexBytes.size() / 2);
        if (!idx.empty()) std::memcpy(idx.data(), sm.indexBytes.data(), sm.indexBytes.size());
        const float wrapThresh = uspan * 0.5f;
        size_t wrapTris = 0;
        size_t hist[6] = {0,0,0,0,0,0};   // span buckets: <0.05span,<0.1,<0.25,<0.5,<0.9,>=0.9 (of uspan)
        for (size_t t = 0; t < tris; ++t)
        {
            const uint16_t a = idx[t*3], b = idx[t*3+1], cc = idx[t*3+2];
            if (a >= n || b >= n || cc >= n) continue;
            const float lo = (std::min)(V[a].u, (std::min)(V[b].u, V[cc].u));
            const float hi = (std::max)(V[a].u, (std::max)(V[b].u, V[cc].u));
            const float sp = hi - lo;
            if (sp > wrapThresh) ++wrapTris;
            const float frac = uspan > 1e-6f ? sp / uspan : 0.0f;
            if (frac < 0.05f) hist[0]++; else if (frac < 0.10f) hist[1]++;
            else if (frac < 0.25f) hist[2]++; else if (frac < 0.50f) hist[3]++;
            else if (frac < 0.90f) hist[4]++; else hist[5]++;
        }
        std::printf("    tri U-span histogram (frac of total U span): <5%%=%zu <10%%=%zu <25%%=%zu <50%%=%zu <90%%=%zu >=90%%=%zu\n",
                    hist[0],hist[1],hist[2],hist[3],hist[4],hist[5]);
        std::printf("    WRAP-BRIDGE triangles (span > 50%% of U) = %zu of %zu  (a single closure ring is a clean seam)\n",
                    wrapTris, tris);

        // ---- position duplication: coincident positions carrying multiple U ----
        const double scale = rmax > 0 ? 2000.0 / rmax : 1.0;   // ~5e-4 * radius resolution
        std::map<PKey, std::vector<float>> byPos;
        for (size_t i = 0; i < n; ++i) byPos[posKey(V[i].p, scale)].push_back(V[i].u);
        size_t multiU = 0, maxDistinct = 0;
        // Seam-jump snappability: at each multi-U position, the gap between its
        // distinct U values IS the local seam jump. If those gaps are a near-INTEGER
        // number of tiles, a WRAP-sampled tiling texture is already continuous there
        // (invisible); a NON-integer gap is the visible seam. "Snappable" = the gap's
        // fractional part is consistent, so nudging the seam column to the nearest
        // integer gap stitches it with a LOCAL edit (no remap).
        size_t jumps = 0, nearInt = 0;
        double fracSum = 0.0; std::vector<double> fracs;
        for (auto& kv : byPos)
        {
            std::set<long long> du;
            for (float u : kv.second) du.insert((long long)std::llround(u * 1000.0));
            if (du.size() > 1) ++multiU;
            maxDistinct = (std::max)(maxDistinct, du.size());
            if (du.size() >= 2)
            {
                std::vector<double> us; for (long long q : du) us.push_back(q / 1000.0);
                for (size_t a = 0; a + 1 < us.size(); ++a)
                {
                    const double gap = us[a+1] - us[a];
                    const double frac = std::fabs(gap - std::round(gap));   // distance to nearest integer
                    ++jumps; fracSum += frac; fracs.push_back(frac);
                    if (frac < 0.08) ++nearInt;
                }
            }
        }
        std::printf("    distinct positions=%zu  with >1 authored U=%zu (%.1f%%)  max distinct-U at one position=%zu\n",
                    byPos.size(), multiU, byPos.empty()?0.0:100.0*multiU/byPos.size(), maxDistinct);
        std::sort(fracs.begin(), fracs.end());
        const double medFrac = fracs.empty() ? 0.0 : fracs[fracs.size()/2];
        std::printf("    SEAM JUMPS: %zu U-gaps at multi-U positions; near-integer (snappable, |frac|<0.08)=%zu (%.0f%%); mean|frac|=%.3f median|frac|=%.3f\n",
                    jumps, nearInt, jumps?100.0*nearInt/jumps:0.0, jumps?fracSum/jumps:0.0, medFrac);
        std::printf("    (frac~0 => texture already continuous (no seam); frac clustered ~const => one phase error, LOCALLY snappable; frac scattered uniform => not locally fixable.)\n");

        // ---- NORMAL coherence at coincident positions (MeshGloss specular seam test) ----
        // If the seam-column verts carry SPLIT normals, MeshGloss's specular/SH lighting
        // spikes along the crease -> a bright line that a NORMAL WELD fixes (no UV remap).
        // Also report whether normals are radial (a clean sphere normal) or arbitrary.
        std::map<PKey, std::vector<uint32_t>> posGroups;
        for (uint32_t i = 0; i < (uint32_t)n; ++i) posGroups[posKey(V[i].p, scale)].push_back(i);
        size_t multiVtx = 0, creased = 0; double maxAngle = 0.0;
        size_t multiUcreased = 0;
        double radialDotSum = 0.0;
        for (uint32_t i = 0; i < (uint32_t)n; ++i)
        {
            const float rr = std::sqrt(V[i].p[0]*V[i].p[0]+V[i].p[1]*V[i].p[1]+V[i].p[2]*V[i].p[2]);
            const float nn = std::sqrt(V[i].n[0]*V[i].n[0]+V[i].n[1]*V[i].n[1]+V[i].n[2]*V[i].n[2]);
            if (rr > 1e-6f && nn > 1e-6f)
                radialDotSum += std::fabs((V[i].p[0]*V[i].n[0]+V[i].p[1]*V[i].n[1]+V[i].p[2]*V[i].n[2])/(rr*nn));
        }
        for (auto& kv : posGroups)
        {
            const std::vector<uint32_t>& g = kv.second;
            if (g.size() < 2) continue;
            ++multiVtx;
            double gMax = 0.0;
            for (size_t a = 0; a < g.size(); ++a)
                for (size_t b = a + 1; b < g.size(); ++b)
                {
                    const float* na = V[g[a]].n; const float* nb = V[g[b]].n;
                    const double la = std::sqrt((double)na[0]*na[0]+na[1]*na[1]+na[2]*na[2]);
                    const double lb = std::sqrt((double)nb[0]*nb[0]+nb[1]*nb[1]+nb[2]*nb[2]);
                    if (la < 1e-9 || lb < 1e-9) continue;
                    double d = (na[0]*nb[0]+na[1]*nb[1]+na[2]*nb[2])/(la*lb);
                    d = d > 1.0 ? 1.0 : (d < -1.0 ? -1.0 : d);
                    const double ang = std::acos(d) * 180.0 / 3.14159265358979;
                    gMax = (std::max)(gMax, ang);
                }
            if (gMax > 5.0) ++creased;
            maxAngle = (std::max)(maxAngle, gMax);
            // is this also a multi-U (seam) position?
            std::set<long long> du; for (uint32_t vi : g) du.insert((long long)std::llround(V[vi].u * 1000.0));
            if (du.size() > 1 && gMax > 5.0) ++multiUcreased;
        }
        std::printf("    NORMALS: radial-alignment=%.3f (1=clean sphere normals)   coincident-pos groups=%zu; creased (>5deg)=%zu; maxcrease=%.1fdeg\n",
                    n ? radialDotSum / n : 0.0, multiVtx, creased, maxAngle);
        std::printf("    seam(multi-U) positions that are ALSO normal-creased = %zu  (>0 => MeshGloss specular seam => NORMAL WELD fixes it, no UV remap)\n", multiUcreased);

        // ---- #247 metric: shared position-edge U-mismatch ----
        // Key each undirected edge by its two position keys; record the U at each
        // endpoint. An edge shared by 2+ triangles whose U disagrees at a shared
        // position endpoint is a "mismatch". A clean sphere mismatches ONLY along
        // the one closure meridian; "scattered everywhere" == incoherent UVs.
        struct ERec { std::vector<float> uA, uB; };   // U seen at endpoint A / B
        std::map<std::pair<PKey,PKey>, ERec> edges;
        for (size_t t = 0; t < tris; ++t)
        {
            const uint16_t vtri[3] = { idx[t*3], idx[t*3+1], idx[t*3+2] };
            for (int e = 0; e < 3; ++e)
            {
                const uint16_t i0 = vtri[e], i1 = vtri[(e+1)%3];
                if (i0 >= n || i1 >= n) continue;
                PKey k0 = posKey(V[i0].p, scale), k1 = posKey(V[i1].p, scale);
                float ua = V[i0].u, ub = V[i1].u;
                std::pair<PKey,PKey> key;
                if (k1 < k0) { std::swap(k0, k1); std::swap(ua, ub); }
                key = { k0, k1 };
                ERec& r = edges[key];
                r.uA.push_back(ua); r.uB.push_back(ub);
            }
        }
        size_t sharedEdges = 0, mismatched = 0;
        auto spread = [](const std::vector<float>& u) {
            if (u.size() < 2) return 0.0f;
            float lo = u[0], hi = u[0];
            for (float x : u) { lo = (std::min)(lo, x); hi = (std::max)(hi, x); }
            return hi - lo;
        };
        for (auto& kv : edges)
        {
            if (kv.second.uA.size() < 2) continue;   // not shared (boundary edge)
            ++sharedEdges;
            if (spread(kv.second.uA) > 1e-3f || spread(kv.second.uB) > 1e-3f) ++mismatched;
        }
        std::printf("    #247 metric: shared position-edges=%zu  U-mismatched=%zu (%.1f%%)\n",
                    sharedEdges, mismatched, sharedEdges ? 100.0*mismatched/sharedEdges : 0.0);
        std::printf("    (clean sphere: mismatch%% ~ the one seam ring (small). ~31%% scattered = genuinely incoherent.)\n\n");
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: dump_skydome_uv <path.alo>\n"); return 2; }

    std::string p(argv[1]);
    std::wstring w(p.begin(), p.end());
    IFile* f = nullptr;
    try { f = new PhysicalFile(w, PhysicalFile::READ); }
    catch (...) { std::printf("cannot open %s\n", argv[1]); return 2; }

    AloModel m;
    try { m = LoadAloModel(f); f->Release(); }
    catch (wexception& e) { f->Release(); std::wprintf(L"parse failed: %s\n", e.what()); return 1; }
    catch (...) { f->Release(); std::printf("parse failed (unknown)\n"); return 1; }

    std::printf("=== %s ===\nmeshes=%zu\n\n", argv[1], m.meshes.size());
    int textSubs = 0;
    for (const AloMesh& me : m.meshes)
        for (size_t si = 0; si < me.subMeshes.size(); ++si)
            if (textured(me.subMeshes[si].vertexFormatName) && !me.subMeshes[si].rawVertexBytes.empty())
            { analyze(me.name, (int)si, me.subMeshes[si]); ++textSubs; }

    if (textSubs == 0) std::printf("(no textured sub-meshes)\n");
    return 0;
}
