#ifndef INSTANCEBORROW_H
#define INSTANCEBORROW_H

#include <algorithm>
#include <cstdint>
#include <vector>

class ParticleSystemInstance;

// Non-owning identity for an Engine-owned ParticleSystemInstance that may be
// retained across Engine::Clear(). The pointer is never dereferenced until the
// live registry finds both the same address and immutable creation token.
struct ParticleSystemInstanceHandle
{
    ParticleSystemInstance* ptr = nullptr;
    std::uint64_t token = 0;

    explicit operator bool() const
    {
        return ptr != nullptr && token != 0;
    }

    void Reset()
    {
        ptr = nullptr;
        token = 0;
    }
};

// Device-free live-identity registry used directly by Engine::ResolveInstance.
// Clear() intentionally preserves m_nextToken: a new instance allocated at an
// old address must never inherit the cleared instance's identity.
class ParticleSystemInstanceBorrowTable
{
public:
    std::uint64_t AllocateToken()
    {
        if (m_nextToken == 0) return 0;  // unreachable 64-bit exhaustion
        return m_nextToken++;
    }

    void Register(ParticleSystemInstance* ptr, std::uint64_t token)
    {
        if (ptr != nullptr && token != 0)
            m_live.push_back({ ptr, token });
    }

    void Unregister(ParticleSystemInstance* ptr, std::uint64_t token)
    {
        m_live.erase(
            std::remove_if(
                m_live.begin(), m_live.end(),
                [ptr, token](const ParticleSystemInstanceHandle& live)
                {
                    return live.ptr == ptr && live.token == token;
                }),
            m_live.end());
    }

    void Clear()
    {
        m_live.clear();
    }

    ParticleSystemInstanceHandle Make(ParticleSystemInstance* ptr) const
    {
        for (const auto& live : m_live)
            if (live.ptr == ptr) return live;
        return {};
    }

    ParticleSystemInstance* Resolve(ParticleSystemInstanceHandle handle) const
    {
        if (!handle) return nullptr;
        for (const auto& live : m_live)
        {
            if (live.ptr == handle.ptr && live.token == handle.token)
                return live.ptr;
        }
        return nullptr;
    }

private:
    std::uint64_t m_nextToken = 1;
    std::vector<ParticleSystemInstanceHandle> m_live;
};

#endif
