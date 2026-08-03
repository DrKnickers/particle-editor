#pragma once
#include <cmath>
// Pure camera-facing alpha for the rotation rings. Raw float[3]; engine
// passes &vec.x. Near-camera half -> 1.0, far half -> backAlpha, smoothstep band.
// The whole ring is still DRAWN (and thus still grabbable) -- this only fades it.
namespace ringfade {
inline float FacingAlpha(const float ringPoint[3], const float ringCenter[3],
                         const float camPos[3], float backAlpha) {
    // Outward normal at this ring point (from center).
    const float off[3]={ ringPoint[0]-ringCenter[0], ringPoint[1]-ringCenter[1], ringPoint[2]-ringCenter[2] };
    const float ol=std::sqrt(off[0]*off[0]+off[1]*off[1]+off[2]*off[2]);
    // Camera direction from center (not from the point -- keeps the metric linear
    // around the ring so the result is monotone and left-right symmetric).
    const float toc[3]={ camPos[0]-ringCenter[0], camPos[1]-ringCenter[1], camPos[2]-ringCenter[2] };
    const float cl=std::sqrt(toc[0]*toc[0]+toc[1]*toc[1]+toc[2]*toc[2]);
    if (ol<1e-6f || cl<1e-6f) return 1.0f;
    float f=(off[0]*toc[0]+off[1]*toc[1]+off[2]*toc[2])/(ol*cl); // [-1,1], +1 = faces cam
    // Clamp near half to 1.0; smoothstep transition spans only the far half.
    // This keeps the near semicircle uniformly opaque and monotone for any
    // sweep that stays in the near hemisphere, while still fading the far pole.
    float fc = f < 0.0f ? f : 0.0f;   // map [0,+1] -> 0 (already "full alpha")
    float t = fc + 1.0f;               // [0,1]: 0=far pole, 1=equator-or-nearer
    t = t*t*(3.0f-2.0f*t);            // smoothstep
    return backAlpha + (1.0f-backAlpha)*t;
}
} // namespace ringfade
