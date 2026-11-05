#ifndef EMITTERDRAWORDER_H
#define EMITTERDRAWORDER_H

#include <vector>
#include <utility>
#include <algorithm>
#include <cstddef>

// Pure draw-order decision for a ParticleSystemInstance render pass: filter the
// live emitter instances by heat pass, then stable-sort them by their source
// emitter's authored rank (its position in the system's emitter list).
//
// ─── The engine's draw order ─────────────────────────────────────────────────
// The game draws a system's emitters in authored rank order, whatever the
// parent/child tree: a child ranked after its parent draws on top of it, and a
// child ranked before its parent draws under it. OBSERVED in retail FoC and in
// the Terrain Editor's traced draws (#949, 2026-11-05) for during-life children
// with alpha and additive blends on either side. This replaces #609's
// "parent on top of its children" rule, which in-game tests did not bear out.
// On-death children never appeared in those tests, so they are assumed to
// follow the same rank rule.

// `emitters` lists (rank, isHeat) for every live emitter instance in SPAWN
// order — the order they sit in ParticleSystemInstance::m_emitters. Returns the
// indices into `emitters` matching `wantHeat`, STABLE-sorted ascending by rank —
// so lower ranks draw first (further back), and the stable sort keeps spawn
// order among the instances of one emitter.
inline std::vector<std::size_t> ComputeEmitterDrawOrder(const std::vector<std::pair<std::size_t, bool>>& emitters,
                                                        bool                                             wantHeat)
{
    std::vector<std::size_t> order;
    order.reserve(emitters.size());
    for (std::size_t i = 0; i < emitters.size(); ++i)
        if (emitters[i].second == wantHeat) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&emitters](std::size_t a, std::size_t b) { return emitters[a].first < emitters[b].first; });
    return order;
}

#endif
