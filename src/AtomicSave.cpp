#include "AtomicSave.h"

#include "ParticleSystem.h"
#include "files.h"
#include "exceptions.h"
#include "host/StringConv.h"   // host::WideToUtf8 -- errors are UTF-8

#include <memory>

namespace {

void SetError(std::string* err, const char* message)
{
    if (err) *err = message;
}

// The exception's own message, or `fallback` when it has none (its string
// resource missing), so a failure is never reported with an empty reason.
void SetError(std::string* err, const wexception& e, const char* fallback)
{
    if (!err) return;
    *err = host::WideToUtf8(e.wwhat());
    if (err->empty()) *err = fallback;
}

// True when `path` loads through the production parser.
bool VerifyParticleSystemFile(const std::wstring& path)
{
    PhysicalFile* f = NULL;
    try
    {
        f = new PhysicalFile(path, PhysicalFile::READ);
        std::unique_ptr<ParticleSystem> verified(new ParticleSystem(f));
        f->Release();
        return true;
    }
    catch (...)
    {
        if (f) f->Release();
        return false;
    }
}

} // namespace

bool AtomicWriteParticleSystem(const ParticleSystem& sys, const std::wstring& dest,
                               const AtomicSaveOptions& options, std::string* err)
{
    if (err) err->clear();
    const std::wstring tmp = options.tmpPath.empty() ? dest + L".tmp" : options.tmpPath;

    PhysicalFile* f = NULL;
    try
    {
        f = new PhysicalFile(tmp, PhysicalFile::WRITE);
    }
    catch (wexception& e)
    {
        SetError(err, e, "could not open file for writing");
        return false;
    }
    catch (...)
    {
        SetError(err, "could not open file for writing");
        return false;
    }

    bool ok = true;
    try
    {
        const_cast<ParticleSystem&>(sys).write(f);
        f->Flush();
    }
    catch (wexception& e)
    {
        SetError(err, e, "write failed");
        ok = false;
    }
    catch (...)
    {
        SetError(err, "write failed");
        ok = false;
    }
    // Close BEFORE verifying, renaming or deleting. PhysicalFile opens without
    // FILE_SHARE_DELETE, so a live handle would make DeleteFileW fail and leave
    // a temp that the next write to the same path cannot reopen.
    f->Release();

    if (ok && options.verify)
    {
        if (options.beforeVerify) options.beforeVerify(tmp);
        if (!VerifyParticleSystemFile(tmp))
        {
            SetError(err, "the written file did not load back");
            ok = false;
        }
    }

    if (ok && !MoveFileExW(tmp.c_str(), dest.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        SetError(err, "could not replace destination file");
        ok = false;
    }

    if (!ok)
        DeleteFileW(tmp.c_str());   // the destination is untouched; no orphan temp
    return ok;
}
