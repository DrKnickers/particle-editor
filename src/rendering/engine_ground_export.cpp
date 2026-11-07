// Exports the loaded ground for the live engine host on the device-owning UI thread.
// Owns only its export files; never changes textures, shaders or rendering state.
#include "engine.h"
#include "EngineGroundExport.h"
#include "EngineGroundExportCleanup.h"
#include "common/ResourceLimits.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <shlobj.h>
#include <stdexcept>

namespace
{
constexpr size_t kRetainedGroundExports = 6;
// Exports are deleted at shutdown; older ones were left by a crash and no host can still read them.
constexpr std::chrono::hours kAbandonedGroundExportAgeHours{24};

void RemoveGroundExport(const std::wstring& path)
{
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) OutputDebugStringA("[live-view] ERROR cannot remove an old ground export\n");
}

std::filesystem::path CreateGroundExportStem(std::uint64_t& exportCounter)
{
    wchar_t localAppData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT,
                                localAppData)))
        throw std::runtime_error("Cannot find the ground export folder.");
    const std::filesystem::path folder = std::filesystem::path(localAppData) / L"AloParticleEditor" / L"engine-ground";
    std::filesystem::create_directories(folder);
    const std::filesystem::file_time_type cutoff =
        std::filesystem::file_time_type::clock::now() - kAbandonedGroundExportAgeHours;
    // Best effort: cleanup failure leaves old exports for the next attempt.
    const std::error_code cleanupError = engine_ground::RemoveAbandonedGroundExports(folder, cutoff);
    if (cleanupError) OutputDebugStringA("[live-view] ERROR cannot clean abandoned ground exports\n");
    const std::wstring prefix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    // Separate processes and same-tick edits get distinct names.
    while (true)
    {
        const std::filesystem::path stem     = folder / (prefix + L"-" + std::to_wstring(++exportCounter));
        const bool                  isUnused = !std::filesystem::exists(stem.wstring() + L"-color.dds") &&
                                               !std::filesystem::exists(stem.wstring() + L"-normal.dds");
        if (isUnused) return stem;
    }
}

bool ExportGroundNormal(IFileManager&                fileManager,
                        int                          slot,
                        const std::wstring&          customColor,
                        const std::filesystem::path& destination)
{
    if (slot == Engine::kGroundSolidColorSlot) return false;
    if (!customColor.empty())
    {
        std::filesystem::path sibling(customColor);
        sibling.replace_filename(sibling.stem().wstring() + L"_B.dds");
        if (!std::filesystem::exists(sibling)) return false;
        std::filesystem::copy_file(sibling, destination);
        return true;
    }
    const char* leaf = engine_ground::NormalLeafForSlot(slot);
    if (!leaf) return false;
    IFile* file = fileManager.getFile(std::string("DATA\\ART\\TEXTURES\\") + leaf);
    if (!file) file = fileManager.getFile(leaf);
    if (!file) return false;
    const std::vector<unsigned char> bytes = ReadAndReleaseCapped(file, kMaxTextureAssetBytes);
    std::ofstream                    output(destination, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("Cannot write the ground normal export.");
    return true;
}

} // namespace

void Engine::DeleteGroundHostExports()
{
    for (const std::pair<std::wstring, std::wstring>& exported : m_groundHostExports)
    {
        RemoveGroundExport(exported.first);
        RemoveGroundExport(exported.second);
    }
    m_groundHostExports.clear();
}

void Engine::ExportGroundForHost(std::wstring&       colorPath,
                                 std::wstring&       normalPath,
                                 const std::wstring& protectedColorPath)
{
    if (DeviceCallsBlocked() || !m_pGroundTexture)
        throw std::runtime_error("The current ground texture is unavailable.");
    const std::filesystem::path stem = CreateGroundExportStem(m_groundHostExportCounter);
    colorPath                        = stem.wstring() + L"-color.dds";
    normalPath                       = stem.wstring() + L"-normal.dds";
    // Track partial exports too, so a failed save does not leave an unowned file behind.
    m_groundHostExports.emplace_back(colorPath, normalPath);
    while (m_groundHostExports.size() > kRetainedGroundExports)
    {
        auto oldest = m_groundHostExports.begin();
        if (oldest->first == protectedColorPath) ++oldest;
        RemoveGroundExport(oldest->first);
        RemoveGroundExport(oldest->second);
        m_groundHostExports.erase(oldest);
    }
    IDirect3DTexture9* scratch = CreateGroundScratchCopy();
    if (!scratch) throw std::runtime_error("Cannot decode the current ground colour for export.");
    const HRESULT saved = D3DXSaveTextureToFileW(colorPath.c_str(), D3DXIFF_DDS, scratch, nullptr);
    scratch->Release();
    if (FAILED(saved)) throw std::runtime_error("Cannot save the current ground colour as DDS.");
    if (!ExportGroundNormal(m_fileManager, m_groundTextureIndex, m_groundSlotCustomPaths[m_groundTextureIndex],
                            normalPath))
        normalPath.clear();
}
