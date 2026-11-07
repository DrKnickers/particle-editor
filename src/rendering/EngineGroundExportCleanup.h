// Owns abandoned ground-export selection and removal for the renderer and unit tests.
// Uses only the filesystem; never touches a device or unrelated folder contents.
#ifndef RENDERING_ENGINE_GROUND_EXPORT_CLEANUP_H
#define RENDERING_ENGINE_GROUND_EXPORT_CLEANUP_H

#include <filesystem>
#include <regex>
#include <system_error>

namespace engine_ground
{
inline std::error_code RemoveAbandonedGroundExports(const std::filesystem::path&    folder,
                                                    std::filesystem::file_time_type cutoff)
{
    // CreateGroundExportStem owns only <pid>-<ticks>-<counter>-{color,normal}.dds.
    static const std::wregex                  kExportLeaf(L"[0-9]+-[0-9]+-[0-9]+-(color|normal)\\.dds");
    std::error_code                           error;
    std::error_code                           firstFileError;
    std::filesystem::directory_iterator       entry(folder, error);
    const std::filesystem::directory_iterator end;
    while (!error && entry != end)
    {
        // One file another process still holds must not stop the sweep of the rest.
        std::error_code                         fileError;
        const std::filesystem::directory_entry& file = *entry;
        if (std::regex_match(file.path().filename().wstring(), kExportLeaf) &&
            std::filesystem::is_regular_file(file.symlink_status(fileError)))
        {
            const std::filesystem::file_time_type writtenAt = file.last_write_time(fileError);
            if (!fileError && writtenAt < cutoff) std::filesystem::remove(file.path(), fileError);
        }
        if (fileError && !firstFileError) firstFileError = fileError;
        entry.increment(error);
    }
    return error ? error : firstFileError;
}
} // namespace engine_ground
#endif
