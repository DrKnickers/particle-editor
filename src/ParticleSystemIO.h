#ifndef PARTICLE_SYSTEM_IO_H
#define PARTICLE_SYSTEM_IO_H

// Pure-IO helpers for reading / writing ParticleSystem to/from `.alo`
// files on disk. Factored out of `DoOpenFile` / `DoSaveFile` /
// `ImportEmitters_LoadFile` in src/main.cpp so that the new-UI host
// (HostWindow + BridgeDispatcher) can call them without touching the
// APPLICATION_INFO* legacy plumbing.
//
// Both helpers swallow `wexception` from the loader/writer machinery
// internally and report success/failure via the return value — callers
// that need to surface an error message can capture it via the optional
// `errorOut` parameter (UTF-8 narrow string for cross-channel use). The
// host translates that return into its own bridge-level error reporting.

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "utils.h"  // WideToAnsi (DeriveParticleSystemName)

class ParticleSystem;

// Derive the on-disk internal system name (chunk 0x0000) from a target
// `.alo` path, exactly like the legacy editor's DoSaveFile did on EVERY
// save: basename, extension stripped, lowercased, narrowed with '_' as
// the replacement for unrepresentable characters. The game engine
// registers particle systems under this internal name, so a file whose
// name doesn't match its filename can fail to resolve in-game (bug
// report: save in this editor -> effect missing until re-saved by the
// legacy editor, which re-stamped the name). Callers pass the final
// destination path (not the .tmp sibling SaveParticleSystem writes to).
inline std::string DeriveParticleSystemName(const std::wstring& path)
{
    std::wstring name = path;
    size_t pos = name.find_last_of(L"\\/");
    if (pos != std::wstring::npos) name = name.substr(pos + 1);
    pos = name.find_last_of(L'.');
    if (pos != std::wstring::npos) name = name.substr(0, pos);
    std::transform(name.begin(), name.end(), name.begin(), towlower);
    return WideToAnsi(name, "_");
}

// Read `path` into a fresh ParticleSystem. Returns nullptr on failure.
// If `errorOut` is non-null, it receives a UTF-8 description of the
// failure (best-effort; empty when the system returned non-null).
std::unique_ptr<ParticleSystem> LoadParticleSystem(const std::wstring& path,
                                                   std::string* errorOut = nullptr);

// Write `system` to `path` using PhysicalFile. Returns false on
// failure. The caller is expected to have populated the system's
// internal name via `setName(...)` if it wants the on-disk
// representation to carry a particular identifier; this helper
// performs no name-derivation logic itself. User-facing save paths
// must stamp `setName(DeriveParticleSystemName(path))` first (legacy
// DoSaveFile parity — see the helper's comment above). `errorOut`
// mirrors LoadParticleSystem's contract.
bool SaveParticleSystem(ParticleSystem* system, const std::wstring& path,
                        std::string* errorOut = nullptr);

#endif // PARTICLE_SYSTEM_IO_H
