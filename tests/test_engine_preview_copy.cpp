// Checks the production clone-and-save boundary with a real particle document.
// No engine, recents or bridge mutation is involved in writing this separate copy.
#include "host/EnginePreviewCopy.h"
#include "host/AcceleratorBridge.h"
#include "host/BridgeDispatcher.h"
#include "host/BridgeDispatchShared.h"
#include "host/BridgeRequestContext.h"
#include "host/LayoutBroker.h"
#include "simulation/ParticleSystemInstance.h"
#include "gamedata/managers.h"
#include <cstdio>
#include <filesystem>

// No instances are registered in this data-model test, so destruction has no renderer work.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

// Renderer-free link seams: this test requests no camera matching or snapshots.
const Engine::Camera& Engine::GetCamera() const
{
    static const Camera camera = {};
    return camera;
}

namespace host
{
bool BridgeRequestContext::RequireEngine(const char*)
{
    return true;
}
void BridgeDispatcher::EmitEngineStateChanged() {}
void BridgeDispatcher::StopEngineLive(const std::string&) {}

struct EnginePreviewTestAccess
{
    static std::unique_ptr<BridgeDispatcher> Make(LayoutBroker& layout, AcceleratorBridge& accelerators)
    {
        // The existing window-free constructor initializes the same member layout.
        return std::unique_ptr<BridgeDispatcher>(
            new BridgeDispatcher(layout, accelerators, BridgeDispatcher::SpawnerConfigTestTag{}));
    }
    static nlohmann::json Dispatch(BridgeDispatcher& dispatcher, const std::string& kind, const nlohmann::json& params)
    {
        const std::string    id = "preview-test";
        BridgeRequestContext context{dispatcher, id, kind, params, nlohmann::json::object()};
        dispatcher.TryDispatchEnginePreview(context);
        return context.res;
    }
    static void Configure(BridgeDispatcher&                dispatcher,
                          std::unique_ptr<ParticleSystem>& document,
                          const std::wstring&              documentPath,
                          const std::wstring&              previewPath,
                          IFileManager&                    files,
                          UndoStack&                       undo)
    {
        dispatcher.m_pParticleSystem      = &document;
        dispatcher.m_currentFilePath      = documentPath;
        dispatcher.m_dirty                = true;
        dispatcher.m_undo                 = &undo;
        dispatcher.m_fileManager          = &files;
        dispatcher.m_enginePreviewAllowed = true;
        dispatcher.m_enginePreviewStatus  = {
            {"state", "ready"}, {"effectPath", WideToUtf8(previewPath)}, {"internalName", "preview_effect"}};
        dispatcher.m_enginePreviewPipe = L"\\\\.\\pipe\\alamo-preview-" + std::to_wstring(GetCurrentProcessId());
    }
    static bool HasOriginalBookkeeping(const BridgeDispatcher& dispatcher, const std::wstring& path, size_t depth)
    {
        return dispatcher.m_currentFilePath == path && dispatcher.m_dirty && dispatcher.m_undo->Depth() == depth;
    }
};
} // namespace host

namespace
{
void Require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

class PreviewFileManager : public IFileManager
{
public:
    explicit PreviewFileManager(const std::wstring& gameRoot) : m_gameRoots{gameRoot} {}

    IFile*                           getFile(const std::string&) override { return nullptr; }
    const std::vector<std::wstring>& GetBasepaths() const override { return m_gameRoots; }

private:
    std::vector<std::wstring> m_gameRoots;
};

void TestDispatcherCopy(const std::filesystem::path& root)
{
    host::LayoutBroker                            layout;
    host::AcceleratorBridge                       accelerators;
    const std::unique_ptr<host::BridgeDispatcher> dispatcher =
        host::EnginePreviewTestAccess::Make(layout, accelerators);
    for (const std::string& kind : {"enginePreview/status", "enginePreview/still", "enginePreview/cancel",
                                    "engineHost/setup/status", "engineHost/setup/start", "engineHost/setup/remove"})
    {
        const nlohmann::json response =
            host::EnginePreviewTestAccess::Dispatch(*dispatcher, kind, nlohmann::json::object());
        Require(response["ok"] == false && response["error"] == "the engine preview is not available in this run",
                "all preview kinds refuse automated runs");
    }
    const std::wstring              path        = (root / "user_effect.alo").wstring();
    const std::wstring              previewPath = (root / "preview_effect.alo").wstring();
    std::unique_ptr<ParticleSystem> document    = std::make_unique<ParticleSystem>();
    document->setName("user_effect");
    document->addRootEmitter();
    UndoStack undo;
    undo.Capture(*document, 0, 0);
    const size_t            depth  = undo.Depth();
    const std::vector<char> before = UndoStack::Serialize(*document);
    PreviewFileManager      files((root / "game").wstring());
    host::EnginePreviewTestAccess::Configure(*dispatcher, document, path,
                                             (root / "game" / "preview_effect.alo").wstring(), files, undo);
    const nlohmann::json refused = host::EnginePreviewTestAccess::Dispatch(
        *dispatcher, "enginePreview/still", {{"timeSeconds", 0}, {"matchCamera", false}});
    Require(refused["ok"] == false && refused["error"] == "The preview copy must be outside the game folders.",
            "real handler passes configured game roots to the preview writer");
    host::EnginePreviewTestAccess::Configure(*dispatcher, document, path, previewPath, files, undo);
    const nlohmann::json response = host::EnginePreviewTestAccess::Dispatch(
        *dispatcher, "enginePreview/still", {{"timeSeconds", 0}, {"matchCamera", false}});
    Require(response["ok"] == true && response["data"]["status"] == "pending", response.dump().c_str());
    Require(host::EnginePreviewTestAccess::HasOriginalBookkeeping(*dispatcher, path, depth),
            "real handler preserves document path, dirty flag and undo depth");
    Require(document->getName() == "user_effect" && UndoStack::Serialize(*document) == before,
            "real handler preserves live document and name");
    dispatcher->ShutdownEnginePreviewWorker();
}
} // namespace

int main()
{
    const std::filesystem::path root =
        std::filesystem::current_path() / (".engine-preview-copy-" + std::to_string(GetCurrentProcessId()));
    const char* stage = "create scratch directories";
    try
    {
        Require(std::filesystem::create_directory(root), "create unique scratch directory");
        const std::filesystem::path install = root / "editor";
        const std::filesystem::path windows = root / "windows";
        const std::filesystem::path game    = root / "game";
        std::filesystem::create_directory(install);
        std::filesystem::create_directory(windows);
        std::filesystem::create_directory(game);
        const std::wstring documentPath = (root / "user_effect.alo").wstring();
        ParticleSystem     document;
        document.addRootEmitter();
        stage = "derive document name";
        document.setName(DeriveParticleSystemName(documentPath));
        UndoStack undo;
        stage = "capture undo snapshot";
        undo.Capture(document, 0, 0);
        const size_t            depth       = undo.Depth();
        const std::vector<char> before      = UndoStack::Serialize(document);
        const std::wstring      previewPath = (root / "preview_effect.alo").wstring();
        std::string             error;
        stage = "write cloned preview copy";
        const bool savedPreview =
            host::WriteEnginePreviewCopy(document, previewPath, "PREVIEW_EFFECT", install.wstring(), windows.wstring(),
                                         documentPath, {game.wstring()}, error);
        if (!savedPreview) throw std::runtime_error(error);
        Require(documentPath == (root / "user_effect.alo").wstring(), "document path unchanged");
        Require(document.getName() == "user_effect" && undo.Depth() == depth, "name and undo depth unchanged");
        Require(UndoStack::Serialize(document) == before, "entire live document unchanged");
        const std::unique_ptr<ParticleSystem> preview = LoadParticleSystem(previewPath, &error);
        Require(preview && preview->getName() == "preview_effect", "preview file has host name stamp");
        Require(SaveParticleSystem(&document, documentPath, &error), "normal save still writes user's path");
        const std::unique_ptr<ParticleSystem> saved = LoadParticleSystem(documentPath, &error);
        Require(saved && saved->getName() == "user_effect", "normal save retains user's name");
        Require(!host::WriteEnginePreviewCopy(document, documentPath, "user_effect", install.wstring(),
                                              windows.wstring(), documentPath, {game.wstring()}, error),
                "refuse to overwrite the open document");
        Require(!host::WriteEnginePreviewCopy(document, (install / "preview_effect.alo").wstring(), "preview_effect",
                                              install.wstring(), windows.wstring(), documentPath, {game.wstring()},
                                              error),
                "refuse install directory");
        const std::wstring gameFile = (game / "preview_effect.alo").wstring();
        Require(SaveParticleSystem(&document, gameFile, &error), "create existing game effect fixture");
        const std::wstring aliasedGameRoot = (install / ".." / "game").wstring();
        Require(!host::WriteEnginePreviewCopy(document, gameFile, "preview_effect", install.wstring(),
                                              windows.wstring(), documentPath, {aliasedGameRoot}, error),
                "refuse game directory after root resolution");
        Require(error == "The preview copy must be outside the game folders.",
                "game refusal explains the protected root");
        const std::unique_ptr<ParticleSystem> gameEffect = LoadParticleSystem(gameFile, &error);
        Require(gameEffect && gameEffect->getName() == "user_effect", "refused preview preserves existing game file");
        stage = "verify dispatcher copy";
        TestDispatcherCopy(root);
        std::filesystem::remove(gameFile);
        std::filesystem::remove(game);
        std::filesystem::remove(previewPath);
        std::filesystem::remove(documentPath);
        std::filesystem::remove(install);
        std::filesystem::remove(windows);
        std::filesystem::remove(root);
        std::puts("engine preview copy: ALL PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::printf("FAIL (%s): %s\n", stage, error.what());
        return 1;
    }
}
