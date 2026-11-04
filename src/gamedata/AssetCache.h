#ifndef ASSET_CACHE_H
#define ASSET_CACHE_H

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "AssetLedger.h"
#include "AssetPathSafety.h"
#include "common/sha256.h"
#include "common/types.h"

class IFileManager;

// An asset's identity, the cache key: the name uppercased, then reduced to a
// safe relative name (F-PATH: strip absolute/UNC/.. before any CreateFile).
inline std::string AssetIdentity(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)toupper(c); });
    return SanitizeAssetName(name);
}

// Turns an asset's bytes into a resource, and makes the embedded fallback.
// The managers pass the D3D9 decoders; tests pass stubs, so the real lookup,
// retry, cache and fallback chain runs without a device.
template <typename T> class IAssetDecoder
{
public:
    virtual ~IAssetDecoder() {}
    // A new resource (one reference), or nullptr when the bytes are rejected.
    virtual T* Decode(const std::vector<unsigned char>& bytes) = 0;
    // The embedded fallback, its bytes and its resource id, or nullptr.
    virtual T* CreateFallback(std::vector<unsigned char>& bytes, std::string& resourceId) = 0;
};

// One candidate the chain tried: its attempt, and its bytes when they were read.
struct AssetCandidate
{
    bool                       read     = false;
    bool                       recorded = true; // false: unverifiable, kept out of the ledger
    std::vector<unsigned char> bytes;
    AssetAttempt               attempt;
};

// The file as named (relative to the working directory), and the content stack.
AssetCandidate ReadLiteralAsset(const std::string& name, unsigned long maxBytes, AssetLedger& ledger);
AssetCandidate ReadContentAsset(IFileManager*      files,
                                const std::string& path,
                                unsigned long      maxBytes,
                                AssetLedger&       ledger);

//
// The texture and shader managers' shared resolution chain: the cache first,
// then the name as given, then the content stack, then the retry extension
// (.DDS / .FXO), then the embedded fallback. Every load is recorded in the
// ledger, every cache hit adds its original name to the cached load.
//
template <typename T> class AssetCache
{
public:
    AssetCache(const char*        type,
               IFileManager*      files,
               const std::string& basePath,
               const char*        retryExtension,
               unsigned long      maxBytes)
        : m_type(type), m_files(files), m_basePath(basePath), m_retryExtension(retryExtension), m_maxBytes(maxBytes)
    {
    }

    ~AssetCache()
    {
        SAFE_RELEASE(m_default);
        Clear();
    }

    AssetCache(const AssetCache&)            = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    // +1 reference to the caller on every non-null return.
    T* Get(IAssetDecoder<T>& decoder, const std::string& original)
    {
        size_t      pos;
        std::string filename = AssetIdentity(original);

        // Cache lookup first: the literal path below never reaches Load(), so a
        // repeat request would otherwise reload and strand the new resource.
        auto cached = m_cache.find(filename);
        if (cached != m_cache.end())
        {
            m_ledger.AddRequest(m_type, filename, original);
            cached->second->AddRef();
            return cached->second;
        }

        AssetRecord record;
        record.type     = m_type;
        record.identity = filename;
        record.requests.insert(original);

        T* resource = Decode(decoder, ReadLiteralAsset(filename, m_maxBytes, m_ledger), record);
        if (resource == NULL)
        {
            // Use the part after the (back)slash, if any
            if (filename.find_first_of(":") != std::string::npos &&
                (pos = filename.find_last_of("\\/")) != std::string::npos)
                filename = filename.substr(pos + 1);
            resource = Load(decoder, filename, record);
        }
        if (resource == NULL)
        {
            std::string name = filename;
            if ((pos = filename.rfind('.')) != std::string::npos) name = name.substr(0, pos) + m_retryExtension;
            resource = Load(decoder, name, record);
            if (resource == NULL)
            {
                if (m_default == NULL)
                {
                    std::vector<unsigned char> bytes;
                    m_default       = decoder.CreateFallback(bytes, m_defaultResource);
                    m_defaultSha256 = m_default ? Sha256Hex(bytes.data(), bytes.size()) : "";
                }
                if (m_default != NULL)
                {
                    resource = m_default;
                    m_default->AddRef();
                    record.outcome           = AssetRecord::Outcome::DefaultSubstituted;
                    record.winner            = AssetWinner();
                    record.winner.kind       = AssetWinner::Kind::Embedded;
                    record.winner.resourceId = m_defaultResource;
                    record.sha256            = m_defaultSha256;
                }
            }
        }

        if (resource != NULL)
        {
            m_cache.insert(std::make_pair(filename, resource));
            resource->AddRef();
        }
        m_ledger.Record(std::move(record));
        return resource;
    }

    // The fallback resource, or nullptr before one was needed.
    T* Default() const { return m_default; }

    // Release every cached resource. The fallback and the ledger are kept.
    void Clear()
    {
        for (auto& entry : m_cache) SAFE_RELEASE(entry.second);
        m_cache.clear();
    }

    void ReleaseDefault() { SAFE_RELEASE(m_default); }

    // Every distinct resource, the fallback included: one fallback can sit
    // under several cache keys.
    std::set<T*> Unique() const
    {
        std::set<T*> unique;
        if (m_default != NULL) unique.insert(m_default);
        for (const auto& entry : m_cache)
            if (entry.second != NULL) unique.insert(entry.second);
        return unique;
    }

    const AssetLedger& Ledger() const { return m_ledger; }

private:
    T* Decode(IAssetDecoder<T>& decoder, AssetCandidate candidate, AssetRecord& record)
    {
        if (!candidate.read)
        {
            if (candidate.recorded) record.attempts.push_back(candidate.attempt);
            return NULL;
        }
        T* resource              = decoder.Decode(candidate.bytes);
        candidate.attempt.result = resource ? AssetAttempt::Result::Resolved : AssetAttempt::Result::DecodeFailed;
        record.attempts.push_back(candidate.attempt);
        if (resource != NULL)
        {
            record.outcome = AssetRecord::Outcome::Resolved;
            record.winner  = candidate.attempt.winner;
            record.sha256  = candidate.attempt.sha256;
        }
        return resource;
    }

    // No reference added: Get adds the caller's.
    T* Load(IAssetDecoder<T>& decoder, const std::string& filename, AssetRecord& record)
    {
        auto cached = m_cache.find(filename);
        if (cached != m_cache.end())
        {
            // A retried name already loaded under its own identity: its load is the winner.
            AssetAttempt attempt;
            attempt.name               = filename;
            attempt.reason             = "cached";
            const AssetRecord* earlier = m_ledger.Find(m_type, filename);
            record.outcome             = earlier ? earlier->outcome : AssetRecord::Outcome::Failed;
            if (earlier && earlier->outcome == AssetRecord::Outcome::Resolved)
            {
                attempt.result = AssetAttempt::Result::Resolved;
                attempt.winner = earlier->winner;
                attempt.sha256 = earlier->sha256;
            }
            if (earlier)
            {
                record.winner = earlier->winner;
                record.sha256 = earlier->sha256;
            }
            record.attempts.push_back(attempt);
            return cached->second;
        }
        return Decode(decoder, ReadContentAsset(m_files, m_basePath + filename, m_maxBytes, m_ledger), record);
    }

    typedef std::map<std::string, T*> Map;

    std::string   m_type;
    IFileManager* m_files;
    std::string   m_basePath;
    std::string   m_retryExtension;
    unsigned long m_maxBytes;
    Map           m_cache;
    T*            m_default = NULL;
    std::string   m_defaultResource, m_defaultSha256;
    AssetLedger   m_ledger;
};

#endif
