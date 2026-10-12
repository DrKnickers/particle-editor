#include "AssetCache.h"
#include "managers.h"
#include "common/exceptions.h"
#include "common/files.h"
#include "common/utils.h"

// Read a winner's bytes for the ledger. An empty or oversized file is refused
// before reading, as ReadAndReleaseCapped refuses it; a short read or I/O error
// leaves the bytes unknown, which the ledger cannot vouch for.
static void ReadWinner(IFile* file, unsigned long maxBytes, AssetCandidate& candidate, AssetLedger& ledger)
{
    const unsigned long size = file->size();
    if (size == 0 || (maxBytes != 0 && size > maxBytes))
    {
        file->Release();
        candidate.attempt.result = AssetAttempt::Result::ReadRejected;
        candidate.attempt.reportedSize = size;
        candidate.attempt.reason = size == 0 ? "empty" : "over the size cap";
        return;
    }
    try
    {
        candidate.bytes = ReadAndReleaseCapped(file, maxBytes);
    }
    catch (ReadException&)
    {
        ledger.MarkUnverifiable("short read: " + candidate.attempt.name);
        candidate.recorded = false;
        return;
    }
    catch (...)
    {
        ledger.MarkUnverifiable("read error: " + candidate.attempt.name);
        throw;
    }
    candidate.attempt.sha256 = Sha256Hex(candidate.bytes.data(), candidate.bytes.size());
    if (candidate.attempt.sha256.empty())
    {
        ledger.MarkUnverifiable("hash failed: " + candidate.attempt.name);
        candidate.recorded = false;
        candidate.bytes.clear();
        return;
    }
    candidate.read = true;
}

AssetCandidate ReadLiteralAsset(const std::string& name, unsigned long maxBytes, AssetLedger& ledger)
{
    AssetCandidate candidate;
    candidate.attempt.name = name;
    IFile* file = NULL;
    try
    {
        file = new PhysicalFile(AnsiToWide(name));
    }
    catch (FileNotFoundException&)
    {
        return candidate;
    }
    candidate.attempt.winner.kind = AssetWinner::Kind::Loose;
    candidate.attempt.winner.path = AssetPathUtf8(AnsiToWide(name));
    ReadWinner(file, maxBytes, candidate, ledger);
    return candidate;
}

AssetCandidate ReadContentAsset(IFileManager* files, const std::string& path, unsigned long maxBytes, AssetLedger& ledger)
{
    AssetCandidate candidate;
    candidate.attempt.name = path;
    IFile* file = files->getFileWithWinner(path, &candidate.attempt.winner);
    if (file == NULL)
    {
        candidate.attempt.winner = AssetWinner();
        return candidate;
    }
    ReadWinner(file, maxBytes, candidate, ledger);
    return candidate;
}
