// Deterministic oracle for persistent ParticleSystemInstance identities.
//
// The first half drives the exact stateful table used by
// Engine::ResolveInstance through clear + same-address reuse. The second half
// pins the production binding so a pointer-only bypass in Engine itself cannot
// leave this device-free test falsely green.

#include "InstanceBorrow.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

int g_failed = 0;

void Check(bool condition, const char* label)
{
    if (condition)
    {
        std::printf("  ok: %s\n", label);
        return;
    }
    ++g_failed;
    std::printf("  FAIL: %s\n", label);
}

std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool Contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

std::size_t Count(const std::string& text, const char* needle)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    const std::size_t length = std::char_traits<char>::length(needle);
    while ((pos = text.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += length;
    }
    return count;
}

}  // namespace

int main()
{
    std::printf("test_instance_borrow\n");

    int storageA = 0;
    int storageB = 0;
    auto* ptrA = reinterpret_cast<ParticleSystemInstance*>(&storageA);
    auto* ptrB = reinterpret_cast<ParticleSystemInstance*>(&storageB);

    ParticleSystemInstanceBorrowTable table;
    table.Register(ptrA, 41);
    const ParticleSystemInstanceHandle staleA = table.Make(ptrA);
    Check(staleA.ptr == ptrA && staleA.token == 41,
          "registered live {A,41} makes the exact persistent handle");
    Check(table.Resolve(staleA) == ptrA,
          "registered live {A,41} resolves before clear");

    table.Clear();
    table.Register(ptrA, 42);  // deterministic allocator-address reuse
    const ParticleSystemInstanceHandle liveA = table.Make(ptrA);
    Check(table.Resolve(staleA) == nullptr,
          "stale {A,41} is rejected against reused-address live {A,42}");
    Check(table.Resolve(liveA) == ptrA,
          "current {A,42} resolves after same-address reuse");
    Check(table.Resolve({ ptrB, 42 }) == nullptr,
          "wrong pointer {B,42} is rejected against live {A,42}");
    Check(table.Resolve({ ptrA, 0 }) == nullptr,
          "reserved token zero is rejected");
    Check(table.Resolve({}) == nullptr,
          "empty handle is rejected");

    table.Register(ptrB, 43);
    const ParticleSystemInstanceHandle liveB = table.Make(ptrB);
    Check(table.Resolve(liveA) == ptrA,
          "registering unrelated {B,43} preserves current {A,42}");
    table.Unregister(ptrA, 42);
    Check(table.Resolve(liveA) == nullptr,
          "natural-removal unregister invalidates the live handle");
    Check(table.Resolve(liveB) == ptrB,
          "unregistering {A,42} preserves unrelated live {B,43}");

    ParticleSystemInstanceBorrowTable sequence;
    const std::uint64_t token1 = sequence.AllocateToken();
    sequence.Clear();
    const std::uint64_t token2 = sequence.AllocateToken();
    Check(token1 == 1 && token2 == 2,
          "Clear preserves the monotonic token sequence");

    ParticleSystemInstanceHandle reset { ptrA, 42 };
    reset.Reset();
    Check(!reset && reset.ptr == nullptr && reset.token == 0,
          "Reset clears both persistent identity fields");

    std::printf("production binding\n");
    const std::filesystem::path root = std::filesystem::current_path();
    const std::string engineHeader =
        ReadSource(root / "src" / "engine.h");
    const std::string engineSource =
        ReadSource(root / "src" / "engine.cpp");
    const std::string renderSource =
        ReadSource(root / "src" / "engine_render.cpp");
    const std::string instanceHeader =
        ReadSource(root / "src" / "ParticleSystemInstance.h");
    const std::string instanceSource =
        ReadSource(root / "src" / "ParticleSystemInstance.cpp");
    const std::string hostHeader =
        ReadSource(root / "src" / "host" / "BridgeDispatcher.h");
    const std::string hostSource =
        ReadSource(root / "src" / "host" / "HostWindow.cpp");
    const std::string recordSource =
        ReadSource(root / "src" / "host" / "BridgeDispatch_Spawner.cpp");

    Check(!engineHeader.empty() && !engineSource.empty()
              && !renderSource.empty() && !instanceHeader.empty()
              && !instanceSource.empty() && !hostHeader.empty()
              && !hostSource.empty() && !recordSource.empty(),
          "production borrow sources are readable");
    Check(Contains(engineSource,
                   "return m_instanceBorrows.Resolve(handle);"),
          "Engine::ResolveInstance delegates to the tested token table");
    Check(Contains(engineSource,
                   "m_instanceBorrows.Register(stored, instanceToken);"),
          "Engine::SpawnParticleSystem registers the stored pointer and token");
    Check(Contains(engineSource,
                   "*this, system, parent, instanceToken);"),
          "Engine passes the allocated token into ParticleSystemInstance");
    Check(Contains(instanceSource,
                   "m_instanceToken(instanceToken)"),
          "ParticleSystemInstance stores the exact constructor token");
    Check(Count(engineSource,
                "ParticleSystemInstance* instance = ResolveInstance(handle);")
              == 2,
          "both handle Kill and Detach resolve before acting");
    const std::size_t clearRegistry =
        engineSource.find("m_instanceBorrows.Clear();");
    const std::size_t clearInstances =
        engineSource.find("m_instances.clear();", clearRegistry);
    Check(clearRegistry != std::string::npos
              && clearInstances != std::string::npos
              && clearRegistry < clearInstances,
          "Engine::Clear invalidates handles before destroying instances");
    const std::size_t unregister =
        renderSource.find("m_instanceBorrows.Unregister(");
    const std::size_t erase =
        renderSource.find("m_instances.erase(it);", unregister);
    Check(unregister != std::string::npos
              && erase != std::string::npos
              && unregister < erase,
          "natural instance removal unregisters before erase");
    Check(Contains(renderSource,
                   "it->get(), (*it)->GetInstanceToken());"),
          "natural removal unregisters the instance's exact stored token");
    Check(Contains(instanceHeader, "const std::uint64_t")
              && Contains(instanceHeader, "m_instanceToken;"),
          "ParticleSystemInstance stores an immutable token");
    Check(Contains(engineHeader,
                   "ParticleSystemInstanceBorrowTable m_instanceBorrows;"),
          "Engine owns the tested live-identity table");
    Check(Contains(hostHeader,
                   "ParticleSystemInstanceHandle     m_recordPreviewAttached;")
              && Contains(hostSource,
                          "ParticleSystemInstanceHandle m_attachedParticleSystem;"),
          "both and only persistent preview slots use tokenized handles");
    Check(Count(hostSource,
                "m_attachedParticleSystem = engine->MakeInstanceHandle(")
              == 2,
          "both HostWindow Shift spawn sites store Engine-made handles");
    Check(Contains(hostSource,
                   "engine->ResolveInstance(m_attachedParticleSystem);"),
          "HostWindow stale-borrow helper uses Engine token resolution");
    Check(Contains(recordSource,
                   "m_recordPreviewAttached = m_engine->MakeInstanceHandle("),
          "record preview stores an Engine-made handle");
    Check(Contains(recordSource,
                   "m_engine->ResolveInstance(m_recordPreviewAttached)"),
          "record preview stale-borrow preflight uses Engine token resolution");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n",
                g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
