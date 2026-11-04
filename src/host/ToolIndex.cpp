// `--tool index`. See ToolIndex.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <memory>
#include <set>
#include "ToolIndex.h"
#include "ToolIO.h"
#include "ContentStack.h"
#include "effect/ParticleSystem.h"
#include "gamedata/managers.h"
#include "common/exceptions.h"
#include "common/files.h"
#include "common/ResourceLimits.h"
#include "version.h"

namespace host
{
namespace
{
using nlohmann::json;

// A texture resolved for its identity alone: the index hashes the winner's
// bytes and never decodes them, so a file D3DX would reject still resolves.
struct Hashed : RefCounted
{
};

class HashOnlyDecoder : public IAssetDecoder<Hashed>
{
public:
    Hashed* Decode(const std::vector<unsigned char>&) override { return new Hashed(); }
    // No embedded fallback: an unresolved texture is reported, not substituted.
    Hashed* CreateFallback(std::vector<unsigned char>&, std::string&) override { return nullptr; }
};

const char kModels[] = "Data\\Art\\Models\\";

// A particle system's first chunk; a mesh .alo starts with another.
const uint32_t kParticleSystemChunk = 0x0900;

bool IsFolder(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The model's bytes when it is a particle system, empty for a mesh. Releases
// the file; throws IOException when it cannot be read.
std::vector<unsigned char> ReadIfParticleSystem(IFile* file)
{
    try
    {
        uint32_t type = 0;
        if (file->read(&type, sizeof(type)) != sizeof(type) || type != kParticleSystemChunk)
        {
            file->Release();
            return {};
        }
        file->seek(0);
    }
    catch (...)
    {
        file->Release();
        throw;
    }
    return ReadAndReleaseCapped(file, kMaxAloModelBytes);
}
} // namespace

int RunIndex(const IndexStack& stack, json* records, std::string* error)
{
    std::vector<std::wstring> roots;
    for (std::wstring root : stack.gameRoots)
    {
        if (!IsFolder(root))
        {
            *error = "a game root is not a folder: " + WideToUtf8(root);
            return kExitUsage;
        }
        if (root.back() != L'\\' && root.back() != L'/') root += L'\\';
        roots.push_back(root);
    }
    for (const std::wstring& layer : stack.layers)
        if (!IsFolder(layer))
        {
            *error = "a layer is not a folder: " + WideToUtf8(layer);
            return kExitUsage;
        }
    const std::string stackHash = ContentStackHash(stack.layers);
    if (stackHash.empty()) return kExitInternal;

    std::unique_ptr<FileManager> files;
    try
    {
        files.reset(new FileManager(roots));
    }
    catch (IOException&)
    {
        *error = "no game root lists a MEG in Data\\MegaFiles.xml";
        return kExitSource;
    }
    catch (ParseException&)
    {
        *error = "a game root's Data\\MegaFiles.xml does not parse";
        return kExitSource;
    }
    files->SetLayers(stack.layers);

    AssetCache<Hashed> textures("texture", files.get(), kTextureBasePath, kTextureRetryExtension,
                                kMaxTextureAssetBytes);
    HashOnlyDecoder    decoder;
    size_t             lookups = 0, resolved = 0;
    auto               texture = [&](const std::string& name)
    {
        Hashed* hashed = textures.Get(decoder, name);
        SAFE_RELEASE(hashed);
        lookups++;
        const std::string  identity = AssetIdentity(name);
        const AssetRecord* record   = textures.Ledger().Find("texture", identity);
        json               value    = {{"request", name},
                                       {"canonicalName", identity},
                                       {"outcome", "unresolved"},
                                       {"source", nullptr},
                                       {"sha256", nullptr}};
        if (record != NULL && record->outcome == AssetRecord::Outcome::Resolved)
        {
            value["outcome"] = "resolved";
            value["source"]  = WinnerJson(record->winner);
            value["sha256"]  = record->sha256;
            resolved++;
        }
        return value;
    };

    // The normal map an emitter gets when its file names none: a file with no
    // normal-map chunk loads the default, and the editor loads it.
    const ParticleSystem::Emitter  defaults;
    json                           systems = json::array(), failures = json::array(), entries = json::array();
    const std::vector<std::string> models = files->listFiles(kModels, ".ALO");
    size_t                         meshes = 0;
    for (const std::string& name : models)
    {
        AssetWinner winner;
        IFile*      file = files->getFileWithWinner(name, &winner);
        if (file == NULL)
        {
            failures.push_back({{"file", name}, {"reason", "listed but could not be opened"}});
            continue;
        }
        std::vector<unsigned char> raw;
        try
        {
            raw = ReadIfParticleSystem(file);
        }
        catch (IOException&)
        {
            failures.push_back({{"file", name}, {"reason", "could not be read"}});
            continue;
        }
        if (raw.empty())
        {
            meshes++;
            continue;
        }
        const std::string bytes(raw.begin(), raw.end());
        const std::string hash = Sha256Hex(bytes);
        if (hash.empty()) return kExitInternal;
        std::string                     reason;
        std::unique_ptr<ParticleSystem> system = ParseSource(bytes, &reason);
        if (!system)
        {
            failures.push_back({{"file", name}, {"source", WinnerJson(winner)}, {"sha256", hash}, {"reason", reason}});
            continue;
        }
        const std::vector<ParticleSystem::Emitter*>& emitters = system->getEmitters();
        systems.push_back({{"file", name},
                           {"source", WinnerJson(winner)},
                           {"sha256", hash},
                           {"internalName", system->getName()},
                           {"emitters", emitters.size()}});
        for (size_t i = 0; i < emitters.size(); i++)
        {
            const ParticleSystem::Emitter& emitter = *emitters[i];
            std::set<float>                cells;
            for (const auto& key : emitter.tracks[ParticleSystem::TRACK_INDEX]->keys) cells.insert(key.value);
            // Only the normal map's chunk is optional: a colour name is always the file's own.
            const struct
            {
                const char*        slot;
                const std::string& name;
                const std::string* loaderDefault;
            } slots[] = {{"color", emitter.colorTexture, nullptr},
                         {"normal", emitter.normalTexture, &defaults.normalTexture}};
            for (const auto& slot : slots)
            {
                if (slot.name.empty()) continue;
                json value = texture(slot.name);
                value["loaderDefault"] =
                    slot.loaderDefault != nullptr && AssetIdentity(slot.name) == AssetIdentity(*slot.loaderDefault);
                entries.push_back({{"effect", name},
                                   {"emitter", {{"index", i}, {"name", emitter.name}}},
                                   {"slot", slot.slot},
                                   {"texture", value},
                                   {"blendMode", emitter.blendMode},
                                   {"isWorldOriented", emitter.isWorldOriented},
                                   {"textureSize", emitter.textureSize},
                                   {"indexValues", cells}});
            }
        }
    }

    const std::string& unverifiable = textures.Ledger().Unverifiable();
    // The chain's first step reads each name as given, relative to the working
    // directory (as the editor's does), so a stray file there can win.
    wchar_t cwd[MAX_PATH] = L"";
    GetCurrentDirectoryW(MAX_PATH, cwd);
    *records = {
        {"schema", 1},
        {"verb", "index"},
        {"tool", PE_VERSION_STR},
        {"content",
         {{"gameRoots", ContentPaths(stack.gameRoots)},
          {"layers", ContentPaths(stack.layers)},
          {"sha256", stackHash},
          {"workingDirectory", ContentPaths({cwd})[0]}}},
        {"summary",
         {{"models", models.size()},
          {"systems", systems.size()},
          {"meshes", meshes},
          {"failed", failures.size()},
          {"records", entries.size()},
          {"textures",
           {{"lookups", lookups},
            {"resolved", resolved},
            {"unresolved", lookups - resolved},
            {"loads", textures.Ledger().Snapshot().size()}}},
          {"unverifiable", unverifiable.empty() ? json(nullptr) : json(unverifiable)}}},
        {"systems", systems},
        {"failures", failures},
        {"records", entries},
    };
    fprintf(stderr,
            "[vfx-index] models=%zu systems=%zu meshes=%zu failed=%zu records=%zu texture-lookups=%zu resolved=%zu "
            "loads=%zu\n",
            models.size(), systems.size(), meshes, failures.size(), entries.size(), lookups, resolved,
            textures.Ledger().Snapshot().size());
    if (!unverifiable.empty()) fprintf(stderr, "[vfx-index] unverifiable: %s\n", unverifiable.c_str());
    return kExitOk;
}
} // namespace host
