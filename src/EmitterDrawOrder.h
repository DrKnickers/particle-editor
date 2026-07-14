#ifndef EMITTERDRAWORDER_H
#define EMITTERDRAWORDER_H

#include <vector>
#include <utility>
#include <algorithm>
#include <cstddef>

// Pure draw-order decision for a ParticleSystemInstance render pass (#574, #609).
//
// Two pure pieces cooperate:
//
//   1. ComputeAuthoredDrawKeys — turns the SMALL authored emitter tree into a
//      per-emitter "draw key" (a post-order sequence number). Runs once per
//      render over the authored emitters (dozens), NOT the live instances
//      (which can number in the thousands for spawn-per-particle systems), so
//      it never allocates in proportion to particle count.
//
//   2. ComputeEmitterDrawOrder — the unchanged sort primitive: filter live
//      instances by heat pass, then STABLE-sort ascending by their draw key.
//
// ─── The engine's draw order (#609) ──────────────────────────────────────────
// The game paints a PARENT emitter ON TOP OF its children — i.e. a child draws
// BEHIND its parent — and orders siblings/roots by authored list position
// (OBSERVED in-game; #609 corrected the earlier #574 premise, which sorted
// purely by rank and so drew a later-ranked child on top of its parent). The
// post-order walk models exactly this: within a family every descendant is
// emitted before the node itself (so the parent's key is greater → drawn last →
// on top), and siblings (and roots) are visited in ascending authored rank (so
// a higher-ranked sibling still draws on top of a lower-ranked one, preserving
// the #574 sibling fix). Rank still resolves order between UNRELATED emitters;
// tree depth resolves it between an ancestor and its descendant.
//
// `parentRank[r]` is the authored POSITION of emitter r's parent (its slot in
// the ParticleSystem's emitter list), or SIZE_MAX when r is a root (parent ==
// NULL). Emitters are addressed by position in [0, n) — position, not the
// mutable Emitter::index mirror, so a bridge index-patch can't desync the keys
// (#609). Returns `key`, indexed by position: `key[r]` is r's post-order slot
// (lower = drawn earlier = further back). Defensive against a malformed forest
// (a parent position out of range, self-referential, or on a cycle): such nodes
// are treated as roots / swept in position order at the end, so every position
// always receives a distinct key and std::sort stays on a valid
// strict-weak-ordering key.
inline std::vector<std::size_t> ComputeAuthoredDrawKeys(
    const std::vector<std::size_t>& parentRank)
{
    const std::size_t n = parentRank.size();
    const std::size_t NO_PARENT = static_cast<std::size_t>(-1);

    // Children lists, keyed by parent rank. We push child ranks in ascending r,
    // so each children[p] is already sorted ascending — the sibling order.
    std::vector<std::vector<std::size_t>> children(n);
    std::vector<std::size_t> roots;
    roots.reserve(n);
    for (std::size_t r = 0; r < n; ++r)
    {
        const std::size_t p = parentRank[r];
        if (p < n && p != r) children[p].push_back(r);
        else                 roots.push_back(r); // root or malformed → root
    }

    std::vector<std::size_t> key(n, 0);
    std::vector<char> assigned(n, 0);
    std::size_t seq = 0;

    // Iterative post-order DFS (children before parent) so a deep spawn chain
    // can't overflow the call stack. `stack` holds (node, next-child-index).
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    for (const std::size_t root : roots)
    {
        stack.push_back({ root, 0 });
        while (!stack.empty())
        {
            auto& top = stack.back();
            const std::vector<std::size_t>& kids = children[top.first];
            if (top.second < kids.size())
            {
                stack.push_back({ kids[top.second++], 0 });
            }
            else
            {
                key[top.first] = seq++;
                assigned[top.first] = 1;
                stack.pop_back();
            }
        }
    }

    // Defensive sweep: any rank orphaned by a cycle (never reached from a root)
    // still gets a key, in rank order, after every reachable node.
    for (std::size_t r = 0; r < n; ++r)
        if (!assigned[r]) key[r] = seq++;

    return key;
}

// `emitters` lists (drawKey, isHeat) for every live emitter instance in SPAWN
// order — the order they sit in ParticleSystemInstance::m_emitters. `drawKey` is
// the authored emitter's post-order key from ComputeAuthoredDrawKeys (shared by
// every live instance of the same emitter). Returns the indices into `emitters`
// matching `wantHeat`, STABLE-sorted ascending by drawKey — so instances draw
// back-to-front per the authored tree, and the stable sort preserves spawn
// order among the (equal-key) instances of one emitter.
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
