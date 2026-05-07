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
};

class ITextureManager
{
public:
	virtual IDirect3DTexture9* getTexture(IDirect3DDevice9* pDevice, std::string name) = 0;
};

class IShaderManager
{
public:
	virtual Effect* getShader(IDirect3DDevice9* pDevice, std::string name) = 0;
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
	void SetModPath(const std::wstring& path);
	const std::wstring& GetModPath() const { return modpath; }

	FileManager(const std::vector<std::wstring>& basepaths);
	~FileManager();
};

#endif