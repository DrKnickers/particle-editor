#ifndef PARTICLECOMPACTION_H
#define PARTICLECOMPACTION_H

#include <cassert>
#include <cstddef>
#include <set>
#include <utility>
#include <vector>

template <typename Primitive, typename ParticlePtr>
size_t CompactKilledParticleSlots(std::vector<Primitive>& primitives,
                                  std::vector<ParticlePtr>& particleIndex,
                                  const std::set<size_t>& kills)
{
    if (kills.empty())
    {
        return 0;
    }

    assert(primitives.size() == particleIndex.size());

    const size_t originalSize = primitives.size();
    size_t write = *kills.begin();
    std::set<size_t>::const_iterator kill = kills.begin();

    for (size_t read = write; read < originalSize; ++read)
    {
        while (kill != kills.end() && *kill < read)
        {
            ++kill;
        }

        if (kill != kills.end() && *kill == read)
        {
            ++kill;
            continue;
        }

        if (write != read)
        {
            primitives[write] = std::move(primitives[read]);
            particleIndex[write] = particleIndex[read];
        }
        particleIndex[write]->m_indicesIndex = write;
        ++write;
    }

    primitives.resize(write);
    particleIndex.resize(write);
    return originalSize - write;
}

#endif
