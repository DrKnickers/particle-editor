#ifndef ASSET_LEDGER_H
#define ASSET_LEDGER_H

#include <set>
#include <string>
#include <vector>

//
// The asset ledger: for every rendering asset the texture and shader managers
// load, which file won and the SHA-256 of the bytes it supplied. An observation
// bundle records it so a texture or shader replaced between two runs shows as a
// provenance difference rather than as drift blamed on the effect.
//
// The catalog is never cleared. Shaders load once in the engine constructor and
// are reused by pointer, and shader-annotation textures stay bound after the
// texture cache is cleared, so a per-observation reset would lose exactly the
// assets the renderer keeps using.
//

// Where an asset's bytes came from.
struct AssetWinner
{
    enum class Kind { None, Loose, Archive, Embedded };
    Kind          kind = Kind::None;
    std::string   path;        // Loose: the file. Archive: the MEG file. UTF-8.
    std::string   member;      // Archive: the member name, as the MEG stores it
    unsigned long start = 0;   // Archive: the member's byte range in the MEG
    unsigned long size  = 0;
    std::string   resourceId;  // Embedded: the executable's resource (IDB_MISSING, IDR_DEFAULT_SHADER)

    bool operator==(const AssetWinner& o) const
    {
        return kind == o.kind && path == o.path && member == o.member && start == o.start &&
            size == o.size && resourceId == o.resourceId;
    }
};

// One load the manager tried, in order. Which fields are set follows the result:
//   NotFound      no winner, no hash
//   ReadRejected  a winner whose bytes were refused before reading (empty or over
//                 the cap): its reported size and the reason, no hash
//   DecodeFailed  a winner and the hash of the bytes the decoder rejected
//   Resolved      a winner and the hash of the bytes in use
struct AssetAttempt
{
    enum class Result { NotFound, ReadRejected, DecodeFailed, Resolved };
    std::string   name;
    Result        result = Result::NotFound;
    AssetWinner   winner;
    std::string   sha256;
    unsigned long reportedSize = 0;
    std::string   reason;

    bool operator==(const AssetAttempt& o) const
    {
        return name == o.name && result == o.result && winner == o.winner && sha256 == o.sha256 &&
            reportedSize == o.reportedSize && reason == o.reason;
    }
};

// One load of one asset: every attempt, and the resource the renderer uses.
// DefaultSubstituted means every attempt failed and the embedded fallback is in
// use; Failed means even the fallback could not be created.
struct AssetRecord
{
    enum class Outcome { Resolved, DefaultSubstituted, Failed };
    std::string               type;        // "texture" or "shader"
    std::string               identity;    // the cache key: uppercased, sanitised
    std::set<std::string>     requests;    // every original name that reached it
    std::vector<AssetAttempt> attempts;
    Outcome                   outcome = Outcome::Failed;
    AssetWinner               winner;
    std::string               sha256;
};

// A winner's path for the ledger: absolute, in UTF-8.
std::string AssetPathUtf8(const std::wstring& path);

class AssetLedger
{
public:
    // Appends a load, or merges its requests into an identical earlier load of the
    // same asset (a reload after a cache clear that found the same bytes).
    void Record(AssetRecord record);

    // A cache hit: another original name reached the latest load of identity.
    void AddRequest(const std::string& type, const std::string& identity, const std::string& original);

    // The latest load of identity, or nullptr.
    const AssetRecord* Find(const std::string& type, const std::string& identity) const;

    // A short read or I/O error leaves the winner's bytes unknown: the ledger can
    // no longer vouch for what was loaded. The first message is kept.
    void MarkUnverifiable(const std::string& message);
    const std::string& Unverifiable() const { return m_unverifiable; }

    // The whole catalog, sorted by type then identity, loads in order.
    std::vector<AssetRecord> Snapshot() const;

private:
    std::vector<AssetRecord> m_records;
    std::string m_unverifiable;
};

#endif
