#ifndef MANAGERS_H
#define MANAGERS_H

#include <string>
#include <map>
#include <vector>
#include "Effect.h"
#include "MegaFiles.h"

//
// Manager interfaces
//
class IFileManager
{
public:
	virtual IFile* getFile(const std::string& path) = 0;

	// Hot-swap the priority basepath for active-mod loose-file lookups.
	// Default is a no-op so simple test mocks don't have to implement
	// it; FileManager overrides with the real basepath swap.
	virtual void SetModPath(const std::wstring& /*path*/) {}

	// Select an ORDERED stack of submod folders under the active mod, whose
	// Data\Art layers on top of the mod's shared core folder.
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
	// See tasks/post-audit-followups.md F6.
	virtual void OnLostDevice() = 0;
};

class IShaderManager
{
public:
	virtual Effect* getShader(IDirect3DDevice9* pDevice, std::string name) = 0;
	virtual void Clear() = 0;
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
	// Additive — SetModPath/SetSubmods remain the active mutators in Phase 1.
	void SetLayers(const std::vector<std::wstring>& absoluteLayers) override;
	const std::vector<std::wstring>& GetContentRoots() const override { return modContentRoots; }

	FileManager(const std::vector<std::wstring>& basepaths);
	~FileManager();
};

#endif
