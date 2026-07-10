#ifndef EMITTERDRAWORDER_H
#define EMITTERDRAWORDER_H

#include <vector>
#include <utility>
#include <algorithm>
#include <cstddef>

// Pure draw-order decision for a ParticleSystemInstance render pass (#574).
//
// `emitters` lists (authoredRank, isHeat) for every live emitter instance in
// SPAWN order — the order they sit in ParticleSystemInstance::m_emitters, which
// is insertion order: root emitters first (in authored-rank order), then any
// lazily-spawned children (spawnDuringLife / spawnOnDeath) appended at the tail
// as their parent particles are born or die, REGARDLESS of the child's rank.
//
// Returns the indices into `emitters` for those matching `wantHeat`, STABLE-
// sorted ascending by authoredRank. Drawing in this order makes every emitter
// honor its authored list position vs its siblings — so a lazily-spawned child
// with a low rank draws BEHIND a higher-ranked sibling that spawned later,
// instead of always drawing last (on top) purely because it was appended last.
// The stable sort preserves spawn order among the (equal-rank) instances of one
// child emitter. `authoredRank` is EmitterInstance::GetSourceRank() ==
// ParticleSystem::Emitter::index, kept in sync on add/remove/reorder.
//
// NOTE: rank ordering INTENTIONALLY overrides parent/child tree depth. The old
// spawn-order draw always painted a child on top of its parent (children are
// appended to m_emitters at spawn); pure rank order drops that invariant, so a
// child whose rank is LOWER than its parent's (reachable via reparentEmitter —
// dragging a low-index emitter onto a higher-index one) now draws BEHIND its
// parent. That is deliberate: the fix models the game as painting strictly by
// authored list rank (#574). This premise is undocumented engine behavior and
// was not independently verified in-game — if the engine instead keys draw
// order off tree depth, the reparent-inverted case would need special handling.
inline std::vector<std::size_t> ComputeEmitterDrawOrder(
    const std::vector<std::pair<std::size_t, bool>>& emitters, bool wantHeat)
{
    std::vector<std::size_t> order;
    order.reserve(emitters.size());
    for (std::size_t i = 0; i < emitters.size(); ++i)
        if (emitters[i].second == wantHeat)
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
        [&emitters](std::size_t a, std::size_t b)
        { return emitters[a].first < emitters[b].first; });
    return order;
}

#endif
