#include "sha256.h"
#include <windows.h>
#include <wincrypt.h>

std::string Sha256Hex(const void* data, size_t size)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    BYTE digest[32] = {};
    DWORD digestSize = sizeof(digest);
    bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    // CryptHashData takes a DWORD length; feed larger buffers in pieces.
    const BYTE* bytes = static_cast<const BYTE*>(data);
    for (size_t done = 0; ok && done < size;)
    {
        const DWORD piece = static_cast<DWORD>(size - done > 0x40000000 ? 0x40000000 : size - done);
        ok = CryptHashData(hash, bytes + done, piece, 0) != FALSE;
        done += piece;
    }
    ok = ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) && digestSize == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) return "";
    const char* hex = "0123456789abcdef";
    std::string result;
    for (BYTE byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
