#ifndef MANAGERS_H
#define MANAGERS_H

#include <string>
#include <map>
#include <vector>
#include "Effect.h"
#include "MegaFiles.h"
#include "AssetCache.h"

//
// Manager interfaces
//
class IFileManager
{
public:
	virtual IFile* getFile(const std::string& path) = 0;

	// getFile, also naming the file that won (a loose path or an archive member)
	// for the asset ledger. Default: no identity, so simple mocks still work.
	virtual IFile* getFileWithWinner(const std::string& path, AssetWinner* winner)
	{
		if (winner != NULL) *winner = AssetWinner();
		return getFile(path);
	}

	// Hot-swap the priority basepath for active-mod loose-file lookups.
	// Default is a no-op so simple test mocks don't have to implement
	// it; FileManager overrides with the real basepath swap.
	virtual void SetModPath(const std::wstring& /*path*/) {}

	// Select an ORDERED stack of submod folders under the active mod. Only the
	// selected folders are searched — a mod's shared core folder is an ordinary
	// selectable entry, never added implicitly.
	// Order is precedence, highest first (front of the vector wins a shared name).
	// Empty clears the stack. Default no-op for simple mocks; FileManager rebuilds
	// its content roots.
	virtual void SetSubmods(const std::vector<std::wstring>& /*names*/) {}

	// Snapshot the active roots so a background thread can construct an
	// ISOLATED FileManager over the same content (own MEG handles -> no seek-race)
	// and rebuild the game-object catalog off the UI thread. Default empties so
	// simple mocks compile; FileManager overrides with the real roots.
	virtual const std::vector<std::wstring>& GetBasepaths() const
	{ static const std::vector<std::wstring> kEmpty; return kEmpty; }
	virtual const std::wstring& GetModPath() const
	{ static const std::wstring kEmpty; return kEmpty; }
	virtual const std::vector<std::wstring>& GetSubmods() const
	{ static const std::vector<std::wstring> kEmpty; return kEmpty; }

	// Set the loose-file content roots directly from an ORDERED layer stack
	// (absolute paths, front = highest precedence). The stack-aware successor to
	// SetModPath()+SetSubmods(): ModManager::SetLayerStack drives this. Default no-op
	// so simple test mocks don't have to implement it; FileManager overrides.
	virtual void SetLayers(const std::vector<std::wstring>& /*absoluteLayers*/) {}

	// The active loose-file content roots in precedence order (front wins),
	// slash-terminated. The stack-aware successor to GetModPath()+GetSubmods() for
	// snapshotting/replicating the active content. Default empty for simple mocks.
	virtual const std::vector<std::wstring>& GetContentRoots() const
	{ static const std::vector<std::wstring> kEmpty; return kEmpty; }
};

class ITextureManager
{
public:
	virtual IDirect3DTexture9* getTexture(IDirect3DDevice9* pDevice, std::string name) = 0;
	virtual void Clear() = 0;
	// Drop every cached resource before a device reset. Distinct from
	// Clear(): Clear preserves the missing-texture placeholder so
	// hot-reload doesn't flash it; OnLostDevice releases everything
	// because under D3D9Ex the D3DX helpers create D3DPOOL_DEFAULT
	// textures that must be released before IDirect3DDevice9::Reset.
	virtual void OnLostDevice() = 0;

	// The emitter path: getTexture, also attesting the requested name as resolved
	// or unresolved (the placeholder, or nothing). Shader annotations keep calling
	// getTexture. Defaults: no attestation, for simple mocks.
	virtual IDirect3DTexture9* getEmitterTexture(IDirect3DDevice9* pDevice, std::string name) { return getTexture(pDevice, name); }
	virtual void ResetTextureAttestation() {}
	// Sorted; clears the attestation.
	virtual void TakeTextureAttestation(std::vector<std::string>& resolved, std::vector<std::string>& unresolved)
	{
		resolved.clear();
		unresolved.clear();
	}
	virtual const AssetLedger* GetAssetLedger() const { return NULL; }
};

class IShaderManager
{
public:
	virtual Effect* getShader(IDirect3DDevice9* pDevice, std::string name) = 0;
	virtual void Clear() = 0;
	virtual const AssetLedger* GetAssetLedger() const { return NULL; }
	// Fan out D3DX effect lifecycle calls to every unique cached Effect.
	// A fallback Effect can occupy several cache keys, so implementations must
	// deduplicate by object identity before invoking either callback.
	virtual void OnLostDevice() = 0;
	virtual void OnResetDevice() = 0;
};

//
// Texture and shader managers. Both resolve an asset name through an
// IFileManager under a base path and fall back to a built-in placeholder
// resource; the definitions live in managers.cpp.
//
class TextureManager : public ITextureManager
{
	AssetCache<IDirect3DTexture9> cache;
	// Emitter-requested names by provenance; a name is in exactly one set.
	std::set<std::string>         resolved, unresolved;

public:
	IDirect3DTexture9* getTexture(IDirect3DDevice9* pDevice, std::string filename);
	IDirect3DTexture9* getEmitterTexture(IDirect3DDevice9* pDevice, std::string filename) override;
	void ResetTextureAttestation() override;
	void TakeTextureAttestation(std::vector<std::string>& resolved, std::vector<std::string>& unresolved) override;
	const AssetLedger* GetAssetLedger() const override { return &cache.Ledger(); }

	void Clear();

	// Drop every cached resource (including the missing-
	// texture placeholder) for the device-reset path. Under D3D9Ex,
	// D3DXCreateTextureFromFileInMemory and D3DXCreateTextureFromResource
	// silently use D3DPOOL_DEFAULT — those handles are stale after
	// IDirect3DDevice9::Reset, so all of them must go. getTexture()
	// lazy-reloads on next call.
	void OnLostDevice() override;

	TextureManager(IFileManager* fileManager, const std::string& basePath);
};

// The texture manager's lookup: the folder under each content root, and the
// extension a name that is not found retries with.
static const char kTextureBasePath[]       = "Data\\Art\\Textures\\";
static const char kTextureRetryExtension[] = ".DDS";

class ShaderManager : public IShaderManager
{
	AssetCache<Effect> cache;

public:
	Effect* getShader(IDirect3DDevice9* pDevice, std::string filename);
	const AssetLedger* GetAssetLedger() const override { return &cache.Ledger(); }

	void Clear();

	void OnLostDevice() override;

	void OnResetDevice() override;

	ShaderManager(IFileManager* fileManager, const std::string& basePath);
};

//
// File Manager
//
class FileManager : public IFileManager
{
	std::vector<std::wstring> basepaths;
	std::vector<MegaFile*>    megafiles;
	std::wstring              modpath;     // the active mod root (or empty = Unmodded)
	// Ordered stack of selected submod folder names under `modpath` (e.g.
	// {"Alpha","Bravo"}), highest precedence first. A shared core folder is just one of these
	// names now (placed where the user ordered it), not a separate layer. Empty = none.
	std::vector<std::wstring> submods;
	// Loose-file search roots for the active mod, in PRECEDENCE order (front
	// wins): the selected submod stack (the core folder among them where chosen)
	// first, then the mod root LAST -- the game ranks the mod root lowest; see
	// BuildModContentRoots. Rebuilt by SetModPath/SetSubmods; searched before the base
	// paths. First match wins (the engine replaces a file by precedence, never merges).
	std::vector<std::wstring> modContentRoots;

	// Populate modContentRoots from `modpath`: the root + each selected submod
	// (in order, those with a Data\Art tree) - the core folder is one of them when selected.
	void BuildModContentRoots();

public:
	IFile* getFile(const std::string& path);
	IFile* getFileWithWinner(const std::string& path, AssetWinner* winner) override;

	// Hot-swap the active "mod": its content roots (mod folder + bundled
	// sub-content folders) are checked before the regular basepaths during
	// loose-file lookups. Pass an empty string to clear (Unmodded). Clears the
	// selected submod stack (a new mod has its own submods).
	void SetModPath(const std::wstring& path) override;
	const std::wstring& GetModPath() const override { return modpath; }

	// Select the ordered submod stack under the active mod (empty for none)
	// and rebuild the content roots. Front = highest precedence. No-op effect if
	// no mod is active.
	void SetSubmods(const std::vector<std::wstring>& names) override;
	const std::vector<std::wstring>& GetSubmods() const override { return submods; }

	// The base install search paths, so a background thread can construct an
	// ISOLATED FileManager over the same roots (own MEG handles -> no seek-race with
	// this instance) and rebuild the game-object catalog off the UI thread. 
	// Replicate the active content with SetLayers(GetContentRoots()).
	const std::vector<std::wstring>& GetBasepaths() const override { return basepaths; }

	// Set the loose-file content roots directly from an ORDERED layer stack
	// (absolute paths, front = highest precedence). Each path is kept if the folder
	// exists (existence only — NOT Data\Art-gated, matching the old unconditional
	// mod-root push) and de-duplicated case-insensitively; rebuilds modContentRoots.
	// The stack-aware successor to SetModPath/SetSubmods; ModManager drives it.
	void SetLayers(const std::vector<std::wstring>& absoluteLayers) override;
	const std::vector<std::wstring>& GetContentRoots() const override { return modContentRoots; }

	// The names of the files directly in `directory` (a relative folder ending in
	// a backslash) that end in `extension`, across every content root, base path
	// and MEG: uppercased, sorted, each once. getFile names each one's winner.
	std::vector<std::string> listFiles(const std::string& directory, const std::string& extension) const;

	FileManager(const std::vector<std::wstring>& basepaths);
	~FileManager();
	FileManager(const FileManager&) = delete;
	FileManager& operator=(const FileManager&) = delete;
};

#endif
