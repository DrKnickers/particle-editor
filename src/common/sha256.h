#ifndef COMMON_SHA256_H
#define COMMON_SHA256_H

#include <cstddef>
#include <string>

// SHA-256 of a byte buffer as 64 lowercase hex digits, or "" when CryptoAPI
// fails. Hashes the bytes a caller already holds, never a re-read of a path.
std::string Sha256Hex(const void* data, size_t size);

#endif
