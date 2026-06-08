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
// `errorOut` parameter (UTF-8 narrow string for cross-channel use).
// Legacy paths in main.cpp keep their MessageBox UI side-effects; they
// translate the bool/nullptr return into their own dialogs.

#include <memory>
#include <string>

class ParticleSystem;

// Read `path` into a fresh ParticleSystem. Returns nullptr on failure.
// If `errorOut` is non-null, it receives a UTF-8 description of the
// failure (best-effort; empty when the system returned non-null).
std::unique_ptr<ParticleSystem> LoadParticleSystem(const std::wstring& path,
                                                   std::string* errorOut = nullptr);

// Write `system` to `path` using PhysicalFile. Returns false on
// failure. The caller is expected to have populated the system's
// internal name via `setName(...)` if it wants the on-disk
// representation to carry a particular identifier; this helper
// performs no name-derivation logic. `errorOut` mirrors
// LoadParticleSystem's contract.
bool SaveParticleSystem(ParticleSystem* system, const std::wstring& path,
                        std::string* errorOut = nullptr);

#endif // PARTICLE_SYSTEM_IO_H
