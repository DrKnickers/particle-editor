// Saves a validated, stamped clone for the engine host. The live document and
// editor bookkeeping are read-only inputs; this does not perform pipe I/O.
#ifndef HOST_ENGINE_PREVIEW_COPY_H
#define HOST_ENGINE_PREVIEW_COPY_H

#include "EnginePreviewPipe.h"
#include "SavePathConfine.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemIO.h"
#include "effect/UndoStack.h"
#include <filesystem>
#include <memory>

namespace host
{
inline bool WriteEnginePreviewCopy(const ParticleSystem& document,
                                   const std::wstring&   path,
                                   const std::string&    internalName,
                                   const std::wstring&   installDirectory,
                                   const std::wstring&   windowsDirectory,
                                   const std::wstring&   documentPath,
                                   std::string&          error)
{
    error = engine_preview::CheckPreviewPath(WideToUtf8(path), internalName, WideToUtf8(installDirectory),
                                             WideToUtf8(windowsDirectory));
    if (!error.empty()) return false;
    const std::filesystem::path destination(path);
    const std::wstring          parent  = clip::ResolveFinalDirPath(destination.parent_path().wstring());
    const std::wstring          install = clip::ResolveFinalDirPath(installDirectory);
    const std::wstring          windows = clip::ResolveFinalDirPath(windowsDirectory);
    if (parent.empty() || install.empty() || windows.empty())
    {
        error = "The preview directories must exist and be resolvable.";
        return false;
    }
    const std::wstring resolved = (std::filesystem::path(parent) / destination.filename()).wstring();
    error =
        engine_preview::CheckPreviewPath(WideToUtf8(resolved), internalName, WideToUtf8(install), WideToUtf8(windows));
    if (!error.empty()) return false;
    if (!documentPath.empty())
    {
        const std::filesystem::path current(documentPath);
        const std::wstring          currentParent = clip::ResolveFinalDirPath(current.parent_path().wstring());
        const std::wstring currentResolved = (std::filesystem::path(currentParent) / current.filename()).wstring();
        if (engine_preview::FoldPath(WideToUtf8(resolved)) == engine_preview::FoldPath(WideToUtf8(currentResolved)))
        {
            error = "The preview copy must have a different path from the open document.";
            return false;
        }
    }
    std::unique_ptr<ParticleSystem> copy;
    try
    {
        copy.reset(UndoStack::Deserialize(UndoStack::Serialize(document)));
    }
    catch (const std::exception& failure)
    {
        error                    = "Cannot clone the document for engine preview.";
        const std::string reason = failure.what();
        if (!reason.empty()) error += " " + reason;
        return false;
    }
    if (!copy)
    {
        error = "Cannot clone the document for engine preview.";
        return false;
    }
    copy->setName(DeriveParticleSystemName(resolved));
    if (engine_preview::FoldPath(copy->getName()) != engine_preview::FoldPath(internalName))
    {
        error = "The preview copy name does not match the host's internal name.";
        return false;
    }
    return SaveParticleSystem(copy.get(), resolved, &error);
}
} // namespace host
#endif
