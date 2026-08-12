// Kind handlers for the textures/* + mods/* bridge domain(s), moved out of
// DispatchInternal's ladder (Phase A dispatch split --
// tasks/2026-07-06-heavyweight-refactor-plan.md).

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include "PerfTrace.h"            // host::perf spans (palette/thumbnail, get-preview)
#include "StringConv.h"           // host::Utf8ToWide / WideToUtf8
#include "../ModManager.h"        // mods/list, mods/refresh, mods/set-layers
#include "../UI/TexturePalette.h" // textures/palette/*

#include "HostMessages.h"         // WM_APP_PREVIEW_READY (textures/get-preview)

#include <commdlg.h>              // GetOpenFileNameW (textures/browse)

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchAssets(BridgeRequestContext& ctx, const std::string& kind)
{
    // DispatchInternal-local aliases so the moved ladder blocks below stay
    // verbatim (plan #3A transforms only).
    const json&        params = ctx.params;
    const std::string& id     = ctx.id;

    // -------- mods/list, mods/refresh, mods/set-layers --------
    //
    // Three thin wrappers around ModManager. ModManager owns the
    // canonical state (mods catalog + the ordered layer stack) and the
    // side-effect chain on a stack change (FileManager content-root swap,
    // registry persist of LastLayers, palette swap to the primary layer,
    // thumbnail cache clear, engine shader/texture reload). The dispatcher's
    // job is JSON in / JSON out plus a single engine/state/changed emit on
    // `set-layers` so subscribed React components see the new activePath
    // (the primary layer) without a separate request.
    //
    // Helper closure for serialising a mods/list payload (used by both
    // mods/list and mods/refresh — same response shape).
    auto buildModsListPayload = [this]() -> json {
        json modsArr = json::array();
        json layersArr = json::array();   // flat catalog: mods (with Data\Art) + nested
        const auto& mods = m_modManager->GetMods();
        for (const auto& m : mods)
        {
            const std::string modPath = WideToUtf8(m.path);
            modsArr.push_back(json{
                {"path",       modPath},
                {"folderName", WideToUtf8(m.folderName)},
                {"nickname",   WideToUtf8(m.nickname)},
                {"isFoC",      m.isFoC},
                {"rootHasArt", m.rootHasArt},
            });
            const std::string label = m.nickname.empty() ? WideToUtf8(m.folderName)
                                                          : WideToUtf8(m.nickname);
            if (m.rootHasArt)
                layersArr.push_back(json{{"path", modPath}, {"label", label},
                                         {"isFoC", m.isFoC}, {"kind", "mod"}});
            for (const auto& n : m.nested)
                layersArr.push_back(json{{"path", WideToUtf8(n.path)},
                                         {"label", WideToUtf8(n.label)},
                                         {"parentLabel", label},
                                         {"parentPath", modPath},
                                         {"isFoC", m.isFoC}, {"kind", "nested"}});
        }
        json stackArr = json::array();
        for (const auto& p : m_modManager->GetLayerStack())
            stackArr.push_back(WideToUtf8(p));
        const std::wstring primary = m_modManager->GetPrimaryLayerPath();
        return json{
            {"mods",       modsArr},
            {"layers",     layersArr},
            {"stack",      stackArr},
            {"activePath", primary.empty() ? json(nullptr) : json(WideToUtf8(primary))},
        };
    };

    if (kind == "mods/list")
    {
        if (!m_modManager)
        {
            ctx.SendOk(json{
                {"mods", json::array()},
                {"layers", json::array()},
                {"stack", json::array()},
                {"activePath", json(nullptr)},
            });
            return true;
        }
        ctx.SendOk(buildModsListPayload());
        return true;
    }

    if (kind == "mods/refresh")
    {
        if (!m_modManager)
        {
            ctx.SendOk(json{
                {"mods", json::array()},
                {"layers", json::array()},
                {"stack", json::array()},
                {"activePath", json(nullptr)},
            });
            return true;
        }
        m_modManager->DiscoverMods();
        // ModManager keeps selectedModPath as-is on refresh; if the
        // path no longer exists on disk the React UI will see a
        // "ghost" selection until the user picks something else. This
        // matches the legacy WM_COMMAND ID_MOD_REFRESH branch in
        // main.cpp which has the same behaviour for symmetry.
        ctx.SendOk(buildModsListPayload());
        return true;
    }

    if (kind == "mods/set-layers")
    {
        // G3: intentional sendOk — handler's success path also returns
        // ctx.SendOk({ok,stack}) where ok may itself be false; caller reads
        // nested ok as the discriminator, so failure stays the same shape.
        if (!m_modManager) { ctx.SendOk(json{{"ok", false}, {"error", "ModManager not bound"}}); return true; }
        auto currentStackJson = [this]() {
            json stack = json::array();
            for (const auto& p : m_modManager->GetLayerStack())
                stack.push_back(WideToUtf8(p));
            return stack;
        };
        // A layer change mutates FileManager roots and the active texture
        // palette before it reloads shaders. Refuse at the bridge boundary
        // while D3D work is blocked so a suspect/reset window cannot leave new
        // roots paired with the previous shader set.
        if (m_engine && m_engine->DeviceCallsBlocked())
        {
            ctx.SendOk(json{
                {"ok", false},
                {"stack", currentStackJson()},
                {"error", "the rendering device is unavailable; the load order was not changed"},
            });
            return true;
        }
        std::vector<std::wstring> paths;
        auto pit = params.find("paths");
        if (pit != params.end() && pit->is_array())
            for (const auto& e : *pit)
                if (e.is_string()) paths.push_back(Utf8ToWide(e.get<std::string>()));
        // Same persistence gate every settings write uses: a --test-host run
        // must never rewrite the daily driver's LastLayers/LastMod.
        // `err` distinguishes the two ways ok can be false. Both React call
        // sites render a supplied `error` verbatim and otherwise fall back to
        // "the mod shaders failed to reload" — which is an actively misleading
        // diagnosis when the real problem was the registry write (2026-07 audit).
        std::string err;
        bool ok = m_modManager->SetLayerStack(paths, !(m_testHost && !m_settingsLive), &err);
        TexturePalette::ClearBridgeThumbCache();
        // [C3] Same lifecycle for the preview LRU: a same-named texture from
        // the new stack must not serve the old stack's pixels. The epoch bump
        // (inside PreviewCacheClear) also invalidates in-flight encodes.
        PreviewCacheClear();
        EmitEngineStateChanged();
        json resp{{"ok", ok}, {"stack", currentStackJson()}};
        if (!err.empty()) resp["error"] = err;
        ctx.SendOk(resp);
        return true;
    }


    // -------- textures/browse --------
    //
    // Host-side native file dialog for an emitter's color/bump texture.
    // Opens in the active mod's texture folder (Data\Art\Textures, with
    // fallbacks), filtered to *.tga;*.dds, and returns the chosen file's
    // basename (or "" if cancelled). The React side commits the result
    // through emitters/set-properties — same path the text input uses.
    // Like file/open, GetOpenFileNameW runs a nested message loop while
    // the JS caller awaits. Mirrors legacy LoadTexture
    // (src/UI/Emitter.cpp:83).
    if (kind == "textures/browse")
    {
        std::string slot = "color";
        if (auto sit = params.find("slot"); sit != params.end() && sit->is_string())
            slot = sit->get<std::string>();

        // Initial dir: active mod's texture folder → mod root → none
        // (dialog opens at its default if all are unavailable).
        std::wstring initialDir;
        if (m_modManager)
        {
            const std::wstring mod = m_modManager->GetPrimaryLayerPath();
            if (!mod.empty())
            {
                auto isDir = [](const std::wstring& p) -> bool {
                    DWORD a = GetFileAttributesW(p.c_str());
                    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
                };
                const std::wstring texDir = mod + L"\\Data\\Art\\Textures";
                if (isDir(texDir))   initialDir = texDir;
                else if (isDir(mod)) initialDir = mod;
            }
        }

        wchar_t buf[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize     = sizeof(ofn);
        ofn.hwndOwner       = m_hostHwnd;
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = MAX_PATH;
        ofn.lpstrFilter     = L"Texture Files (*.tga;*.dds)\0*.tga;*.dds\0All Files (*.*)\0*.*\0\0";
        ofn.lpstrTitle      = (slot == "bump") ? L"Select bump texture" : L"Select color texture";
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.Flags           = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (!GetOpenFileNameW(&ofn))
        {
            // Cancelled / dialog failure → empty filename (no-op on the
            // React side, which only commits a non-empty string).
            ctx.SendOk(json{{"filename", ""}});
            return true;
        }

        // Store only the basename — matches the colorTexture /
        // normalTexture field convention (legacy strips path via strrchr).
        const std::wstring full = buf;
        const size_t slash = full.find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
        ctx.SendOk(json{{"filename", WideToUtf8(base)}});
        return true;
    }


    // -------- textures/palette/* --------
    //
    // Expose the per-mod frequently-used texture palette
    // (TexturePalette::Store, already repointed at the active mod by
    // ModManager::SelectMod) to the new UI.
    if (kind == "textures/palette/list")
    {
        std::string slot = "color";
        if (auto it = params.find("slot"); it != params.end() && it->is_string())
            slot = it->get<std::string>();
        const TexturePalette::SlotMask mask =
            (slot == "bump") ? TexturePalette::SLOT_BUMP : TexturePalette::SLOT_COLOR;

        auto& store = TexturePalette::Store::Instance();
        store.SetActiveFilter(mask);  // slot-aware default (persists per-mod)

        auto toArray = [](const std::vector<TexturePalette::Entry>& entries) {
            json arr = json::array();
            for (const TexturePalette::Entry& e : entries)
                arr.push_back(json{
                    {"filename", WideToUtf8(e.filename)},
                    {"pinned",   e.isPinned},
                    {"slotMask", (int)e.slotMask},
                });
            return arr;
        };

        ctx.SendOk(json{
            {"hasMod",  store.HasActiveMod()},
            {"filter",  slot},
            {"pins",    toArray(store.Pins(mask))},
            {"recents", toArray(store.Recents(mask))},
        });
        return true;
    }

    if (kind == "textures/palette/thumbnail")
    {
        std::string filename;
        if (auto it = params.find("filename"); it != params.end() && it->is_string())
            filename = it->get<std::string>();

        std::unique_ptr<host::perf::Span> span;
        if (host::perf::Enabled())
            span = std::make_unique<host::perf::Span>("bridge.texture_thumbnail", nlohmann::json{
                {"filename", filename}
            });
        IDirect3DDevice9* dev = m_engine ? m_engine->GetDevice() : nullptr;
        const TexturePalette::ThumbnailResult t = TexturePalette::GetThumbnail(
            Utf8ToWide(filename), m_fileManager, dev);
        const char* status =
            t.status == TexturePalette::ThumbStatus::Ok      ? "ok"      :
            t.status == TexturePalette::ThumbStatus::Missing ? "missing" : "broken";
        ctx.SendOk(json{
            {"dataUri", t.dataUri.empty() ? json(nullptr) : json(t.dataUri)},
            {"status",  status},
        });
        if (span) span->End(status);
        return true;
    }

    if (kind == "textures/get-preview")
    {
        std::string filename;
        if (auto it = params.find("filename"); it != params.end() && it->is_string())
            filename = it->get<std::string>();
        // Default true: force every pixel fully opaque (alpha=255) so ADDITIVE
        // atlas frames (RGB content, alpha ~0) are visible. The picker passes
        // false to honor the texture's real alpha.
        bool flattenAlpha = true;
        if (auto it = params.find("flattenAlpha"); it != params.end() && it->is_boolean())
            flattenAlpha = it->get<bool>();

        std::unique_ptr<host::perf::Span> span;
        if (host::perf::Enabled())
            span = std::make_unique<host::perf::Span>("bridge.texture_preview", nlohmann::json{
                {"filename", filename},
                {"flattenAlpha", flattenAlpha}
            });

        // [C3] LRU hit: full result, synchronously — repeats cost nothing.
        const std::string cacheKey = filename + (flattenAlpha ? "|1" : "|0");
        if (auto hit = m_previewLruIdx.find(cacheKey); hit != m_previewLruIdx.end())
        {
            // Move to MRU.
            m_previewLru.splice(m_previewLru.begin(), m_previewLru, hit->second);
            const PreviewCacheEntry& e = hit->second->second;
            if (e.status == "ok")
                ctx.SendOk(json{{"status", "ok"}, {"dataUri", e.dataUri},
                            {"srcW", e.srcW}, {"srcH", e.srcH}});
            else
                ctx.SendOk(json{{"status", e.status}});
            if (span) span->End(e.status + "-cached");
            return true;
        }

        // In-flight dedupe: the encode is already queued; the caller waits
        // for the same preview-ready event the first requester armed.
        if (m_previewInFlight.count(cacheKey))
        {
            ctx.SendOk(json{{"status", "pending"}});
            if (span) span->End("pending-dup");
            return true;
        }

        // Miss: the DEVICE-BOUND half runs here (UI thread) — file/MEG read,
        // D3DX decode to SCRATCH, flatten, one packed copy. The measured-heavy
        // PNG encode + base64 goes to the worker; the response is `pending`
        // and `textures/preview-ready` fires when the dataUri is cached.
        IDirect3DDevice9* dev = m_engine ? m_engine->GetDevice() : nullptr;
        TexturePalette::PreviewPixels px = TexturePalette::DecodeTexturePreviewBgra(
            Utf8ToWide(filename), m_fileManager, dev, 1024, flattenAlpha);
        if (px.status != "ok")
        {
            // Terminal failures answer (and cache) immediately — no encode.
            PreviewCachePut(cacheKey, PreviewCacheEntry{px.status, "", px.srcW, px.srcH});
            ctx.SendOk(json{{"status", px.status}});
            if (span) span->End(px.status);
            return true;
        }

        if (!m_previewWorker)
            m_previewWorker = std::make_unique<host::PreviewEncodeWorker>(
                m_hostHwnd, WM_APP_PREVIEW_READY);
        host::PreviewEncodeWorker::Job job;
        job.key = cacheKey;
        job.filename = Utf8ToWide(filename);
        job.flattenAlpha = flattenAlpha;
        job.srcW = px.srcW; job.srcH = px.srcH;
        job.outW = px.outW; job.outH = px.outH;
        job.bgra = std::move(px.bgra);
        job.epoch = m_previewEpoch;
        m_previewInFlight.insert(cacheKey);
        m_previewWorker->Enqueue(std::move(job));
        ctx.SendOk(json{{"status", "pending"}});
        if (span) span->End("pending");
        return true;
    }

    if (kind == "textures/palette/toggle-pin")
    {
        std::string filename;
        if (auto it = params.find("filename"); it != params.end() && it->is_string())
            filename = it->get<std::string>();

        const std::wstring wf = Utf8ToWide(filename);
        auto& store = TexturePalette::Store::Instance();
        if (!store.TogglePin(wf))
        {
            // The only user-reachable failure (entry exists + mod active) is
            // the pins-full cap; no-mod/malformed never happen from the UI.
            // G3: intentional sendOk — not an error, a "nothing changed" cap
            // result the UI handles as normal (carries reason, not error);
            // success path also returns ctx.SendOk({ok:true,pinned}).
            ctx.SendOk(json{{"ok", false}, {"reason", "pins-full"}});
            return true;
        }
        // Report the new pinned state (an entry can be pinned for either
        // slot, so check both filters).
        bool pinned = false;
        for (const TexturePalette::Entry& e : store.Pins(TexturePalette::SLOT_COLOR))
            if (e.filename == wf) { pinned = true; break; }
        if (!pinned)
            for (const TexturePalette::Entry& e : store.Pins(TexturePalette::SLOT_BUMP))
                if (e.filename == wf) { pinned = true; break; }
        ctx.SendOk(json{{"ok", true}, {"pinned", pinned}});
        return true;
    }

    if (kind == "textures/palette/touch-recent")
    {
        std::string filename;
        std::string slot = "color";
        if (auto it = params.find("filename"); it != params.end() && it->is_string())
            filename = it->get<std::string>();
        if (auto it = params.find("slot"); it != params.end() && it->is_string())
            slot = it->get<std::string>();
        const TexturePalette::SlotMask mask =
            (slot == "bump") ? TexturePalette::SLOT_BUMP : TexturePalette::SLOT_COLOR;

        TexturePalette::Store::Instance().TouchRecent(Utf8ToWide(filename), mask);
        ctx.SendOk(json{{"ok", true}});
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
