#pragma once
#include <cmath>
#include <vector>
// Pure dashing + corner-bracket geometry for the selection box. Raw
// float[3]; engine passes &vec.x. Tested == shipped.
namespace selboxstyle {
struct Seg { float a[3]; float b[3]; };
inline float sb_dist(const float a[3], const float b[3]) {
    const float dx=b[0]-a[0], dy=b[1]-a[1], dz=b[2]-a[2]; return std::sqrt(dx*dx+dy*dy+dz*dz);
}
// Split [a,b] into dashes of `dashLen` separated by `gapLen`. First dash at a; a
// trailing partial dash is clipped to b (never overshoots). No-op on degenerate input.
inline void DashedSegments(const float a[3], const float b[3], float dashLen, float gapLen,
                           std::vector<Seg>& out) {
    const float L=sb_dist(a,b);
    if (L<1e-6f || dashLen<=0.0f) return;
    const float dir[3]={ (b[0]-a[0])/L, (b[1]-a[1])/L, (b[2]-a[2])/L };
    const float stride = dashLen + (gapLen>0.0f?gapLen:0.0f);
    for (float pos=0.0f; pos<L; pos+=stride) {
        float end=pos+dashLen; if (end>L) end=L;
        Seg s;
        for (int i=0;i<3;++i){ s.a[i]=a[i]+dir[i]*pos; s.b[i]=a[i]+dir[i]*end; }
        out.push_back(s);
    }
}
// 3 short segments from corner `c` toward neighbours n0,n1,n2, length `segLen`
// (clamped to each edge). The bright corner brackets of the selection box.
inline void CornerBracketSegs(const float c[3], const float n0[3], const float n1[3],
                              const float n2[3], float segLen, std::vector<Seg>& out) {
    const float* ns[3]={ n0,n1,n2 };
    for (int k=0;k<3;++k) {
        const float L=sb_dist(c,ns[k]); if (L<1e-6f) continue;
        const float t=(segLen<L?segLen:L);
        Seg s;
        for (int i=0;i<3;++i){ s.a[i]=c[i]; s.b[i]=c[i]+(ns[k][i]-c[i])*(t/L); }
        out.push_back(s);
    }
}
} // namespace selboxstyle
