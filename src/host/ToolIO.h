#ifndef TOOL_IO_H
#define TOOL_IO_H

// File and document helpers shared by the `--tool` verbs (ToolMain.cpp,
// ToolPatch.cpp). Window-free.

#include <memory>
#include <string>

class ParticleSystem;

namespace host
{
// The exit codes, shared by every verb (see ToolMain.h).
const int kExitOk        = 0;
const int kExitUsage     = 2;
const int kExitSource    = 3;
const int kExitSelector  = 4;
const int kExitRule      = 5;
const int kExitPostcheck = 6;
const int kExitIo        = 13;
const int kExitInternal  = 14;

// Lowercase hex SHA-256 of `bytes`, or "" if CryptoAPI fails.
std::string Sha256Hex(const std::string& bytes);

// True when `a` and `b` name the same file: equal canonical paths (compared
// case-insensitively, as NTFS does) or, for files that exist, the same volume
// and file index, which also catches a hard link.
bool SameFile(const std::wstring& a, const std::wstring& b);

// Reads the whole file (at most 512 MiB). Returns false with a reason.
bool ReadFileBytes(const std::wstring& path, std::string* bytes, std::string* error);

// Parses a particle system from bytes already in memory, so a hash and the
// document describe the same content. nullptr with a reason on failure.
std::unique_ptr<ParticleSystem> ParseSource(const std::string& bytes, std::string* error);

// The bytes the writer produces for `system`, with no name stamp.
std::string SaveToBytes(ParticleSystem& system);

// Writes `bytes` to a unique sibling, then renames it over `path`. Nothing is
// left behind on failure.
bool WriteFileAtomically(const std::wstring& path, const std::string& bytes);
} // namespace host

#endif // TOOL_IO_H
