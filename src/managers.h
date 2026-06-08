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
	std::wstring              modpath;     // priority basepath for the active mod, or empty

public:
	IFile* getFile(const std::string& path);

	// Hot-swap a "mod" base path that's checked before the regular basepaths
	// during loose-file lookups. Pass an empty string to clear (Unmodded).
	void SetModPath(const std::wstring& path) override;
	const std::wstring& GetModPath() const { return modpath; }

	FileManager(const std::vector<std::wstring>& basepaths);
	~FileManager();
};

#endif