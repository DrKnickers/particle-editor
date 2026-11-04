#ifndef MEGAFILES_H
#define MEGAFILES_H

#include <vector>
#include "common/types.h"
#include "common/files.h"

class MegaFile
{
    struct FileInfo
    {
        uint32_t crc;
        uint32_t index;
        uint32_t size;
        uint32_t start;
        uint32_t nameIndex;
    };

    IFile*                   file;
    std::wstring             path;
    std::vector<FileInfo>    files;
    std::vector<std::string> filenames;

public:
    // A member's identity in this archive, for the asset ledger.
    struct Member
    {
        std::string   name;
        unsigned long start, size;
    };

    // member, when given, receives the found member's name and byte range.
    IFile*              getFile(std::string path, Member* member = NULL) const;
    const std::wstring& getPath() const { return path; }
    // Every member's name, as stored in the archive.
    const std::vector<std::string>& getFilenames() const { return filenames; }

    // path names the archive on disk, for the asset ledger; empty when unknown.
    MegaFile(IFile* file, const std::wstring& path = L"");
    ~MegaFile();
    MegaFile(const MegaFile&)            = delete;
    MegaFile& operator=(const MegaFile&) = delete;
};

#endif