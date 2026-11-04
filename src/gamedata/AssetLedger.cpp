#include "AssetLedger.h"
#include <algorithm>
#include <windows.h>

std::string AssetPathUtf8(const std::wstring& path)
{
    std::wstring full   = path;
    const DWORD  length = GetFullPathNameW(path.c_str(), 0, NULL, NULL);
    if (length != 0)
    {
        full.assign(length, L'\0');
        const DWORD written = GetFullPathNameW(path.c_str(), length, &full[0], NULL);
        full.resize(written != 0 && written < length ? written : 0);
        if (full.empty()) full = path;
    }
    const int   bytes = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), (int)full.size(), NULL, 0, NULL, NULL);
    std::string result(bytes > 0 ? bytes : 0, '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, full.c_str(), (int)full.size(), &result[0], bytes, NULL, NULL);
    return result;
}

void AssetLedger::Record(AssetRecord record)
{
    for (auto it = m_records.rbegin(); it != m_records.rend(); ++it)
    {
        if (it->type != record.type || it->identity != record.identity) continue;
        if (it->attempts == record.attempts && it->outcome == record.outcome && it->winner == record.winner &&
            it->sha256 == record.sha256)
        {
            it->requests.insert(record.requests.begin(), record.requests.end());
            // Keep the merged load latest, so cache hits alias onto it.
            std::rotate(it.base() - 1, it.base(), m_records.end());
            return;
        }
        break;
    }
    m_records.push_back(std::move(record));
}

void AssetLedger::AddRequest(const std::string& type, const std::string& identity, const std::string& original)
{
    for (auto it = m_records.rbegin(); it != m_records.rend(); ++it)
        if (it->type == type && it->identity == identity)
        {
            it->requests.insert(original);
            return;
        }
}

const AssetRecord* AssetLedger::Find(const std::string& type, const std::string& identity) const
{
    for (auto it = m_records.rbegin(); it != m_records.rend(); ++it)
        if (it->type == type && it->identity == identity) return &*it;
    return nullptr;
}

void AssetLedger::MarkUnverifiable(const std::string& message)
{
    if (m_unverifiable.empty()) m_unverifiable = message;
}

std::vector<AssetRecord> AssetLedger::Snapshot() const
{
    std::vector<AssetRecord> result = m_records;
    std::stable_sort(result.begin(), result.end(), [](const AssetRecord& a, const AssetRecord& b)
                     { return a.type != b.type ? a.type < b.type : a.identity < b.identity; });
    return result;
}
