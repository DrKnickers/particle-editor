#include <algorithm>
#include <iostream>
#include "managers.h"
#include "common/exceptions.h"
#include "common/crc32.h"
#include "xml.h"
#include "common/utils.h"
#include "ModLayers.h"
#include <cstdarg>     // [shader-gate] va_list for ShaderLog
#include <cstdio>      // [shader-gate] headless diagnostic logging (stdout)
#include <set>
#include "AssetPathSafety.h"
#include "common/ResourceLimits.h"   // kMaxTextureAssetBytes (asset-read size caps)
#include "Resources/resource.h"
using namespace std;

//
// FileManager class
//
IFile* FileManager::getFile(const string& path)
{
	return getFileWithWinner(path, NULL);
}

IFile* FileManager::getFileWithWinner(const string& path, AssetWinner* winner)
{
	// If a mod is selected, try its content roots first (in precedence order --
	// the selected submod layers, then the mod root; see BuildModContentRoots) so mod
	// loose files shadow the base game's. First match wins: the engine REPLACES a
	// file by precedence, never merges, so this single resolved copy is faithful.
	//
	// A drive ("C:...") or rooted ("\...") path is used as-is; anything else is
	// relative to each root. Length-checked: path may be shorter than two chars.
	const bool relative = (path.size() < 2 || path[1] != ':') && (path.empty() || path[0] != '\\');
	for (vector<wstring>::const_iterator root = modContentRoots.begin(); root != modContentRoots.end(); ++root)
	{
		try
		{
			wstring wpath = AnsiToWide(path);
			wstring filename = relative ? *root + wpath : wpath;
			IFile* file = new PhysicalFile(filename);
			if (winner != NULL) { winner->kind = AssetWinner::Kind::Loose; winner->path = AssetPathUtf8(filename); }
			return file;
		}
		catch (IOException&)
		{
		}
	}

	// First see if we can open it physically
	for (vector<wstring>::const_iterator base = basepaths.begin(); base != basepaths.end(); base++)
	{
		try
		{
			wstring wpath = AnsiToWide(path);
			wstring filename = relative ? *base + wpath : wpath;
			IFile* file = new PhysicalFile(filename);
			if (winner != NULL) { winner->kind = AssetWinner::Kind::Loose; winner->path = AssetPathUtf8(filename); }
			return file;
		}
		catch (IOException&)
		{
		}
	}

	// Search in the index
	try
	{
		for (vector<MegaFile*>::iterator i = megafiles.begin(); i != megafiles.end(); i++)
		{
			MegaFile::Member member;
			IFile* file = (*i)->getFile( path, &member );
			if (file != NULL)
			{
				if (winner != NULL)
				{
					winner->kind   = AssetWinner::Kind::Archive;
					winner->path   = AssetPathUtf8((*i)->getPath());
					winner->member = member.name;
					winner->start  = member.start;
					winner->size   = member.size;
				}
				return file;
			}
		}
	}
	catch (IOException&)
	{
	}

	return NULL;
}

FileManager::FileManager(const vector<wstring>& basepaths)
{
	XMLTree xml;
	this->basepaths = basepaths;
	for (vector<wstring>::const_iterator path = basepaths.begin(); path != basepaths.end(); path++)
	{
		try
		{
			PhysicalFile* file = new PhysicalFile( *path + L"Data\\MegaFiles.xml" );
			xml.parse( file );
			file->Release();

			const XMLNode* root = xml.getRoot();
			if (root->getName() != L"Mega_Files")
			{
				throw BadFileException();
			}

			// Create a file index from all mega files
			for (unsigned int i = 0; i < root->getNumChildren(); i++)
			{
				const XMLNode* child = root->getChild(i);
				if (child->getName() != L"File")
				{
					// Tolerate non-<File> children (e.g. <Info Name=.../> in mod MegaFiles.xml):
					// skip them instead of throwing (the ctor's catch(...) would rethrow ->
					// std::terminate, crashing the editor on a standard-format mod).
					continue;
				}
		
				wstring filename = *path + child->getData();
				try
				{
					// Hold the creation reference in a local and drop it after
					// handing the file to MegaFile (which takes its own AddRef).
					// The old new-inside-new form leaked that reference on every
					// path — permanently pinning the Win32 HANDLE when a
					// malformed MEG made the MegaFile constructor throw.
					PhysicalFile* file = new PhysicalFile(filename);   // rc=1
					try
					{
						megafiles.push_back(new MegaFile(file, filename));   // rc=2
					}
					catch (...)
					{
						file->Release();
						throw;
					}
					file->Release();   // rc=1, now owned by the MegaFile
				}
				catch (IOException)
				{
				}
			}
		}
		catch (FileNotFoundException)
		{
			continue;
		}
		catch (...)
		{
			for (vector<MegaFile*>::iterator i = megafiles.begin(); i != megafiles.end(); i++)
			{
				delete *i;
			}
			throw;
		}
	}

	if (megafiles.empty())
	{
		throw FileNotFoundException(L"MegaFiles.xml");
	}
}

FileManager::~FileManager()
{
	for (vector<MegaFile*>::iterator i = megafiles.begin(); i != megafiles.end(); i++)
	{
		delete (*i);
	}
}

void FileManager::SetModPath(const wstring& path)
{
	modpath = path;
	if (!modpath.empty() && modpath.back() != L'\\' && modpath.back() != L'/')
	{
		modpath += L'\\';
	}
	submods.clear();   // a new mod has its own submods; reset the stack
	BuildModContentRoots();
}

// Select the ordered submod stack under the active mod (empty to clear) and
// rebuild the content roots. Selected layers stack in precedence order (front
// wins); only what the user selected is searched — nothing is added implicitly.
void FileManager::SetSubmods(const vector<wstring>& names)
{
	submods = names;
	BuildModContentRoots();
}

// A mod can keep a large shared CORE of assets next to its root Data\ that
// the per-submod content layers on top of -- a folder holding hundreds of loose
// .alo (e.g. GalloFree_HTT26.alo) shared across that mod's submods. The editor
// once searched only the mod ROOT, so all of that core was invisible. It is now
// selected and ordered like any other submod layer, so nothing here special-cases
// its name. The mod root is the LOWEST-precedence mod layer (see the ordering
// note below) -- it does NOT shadow a submod copy of the same file.
//
// A mod can stack several submods explicitly, in precedence order. The
// order matches such a mod's own launch parameters (LEFT = highest), where the mod root is
// the LOWEST mod layer -- a stale file in the root must NOT shadow a submod's copy.
// Search order (first match wins in getFile; the game replaces per file, never merges):
//   submods[0..n] (the selected stack, front = highest precedence; each needs a Data\Art tree)
//   mod root      (lowest mod layer; the game lists it last)
//   ...base game  (appended later in getFile)
// A shared core folder is just another entry in `submods` -- the user selects + orders
// it in the Submods dialog (it was previously auto-appended here, which wrongly forced
// it on for the submods that exclude it).
void FileManager::BuildModContentRoots()
{
	modContentRoots.clear();
	if (modpath.empty()) return;

	auto hasArtTree = [](const wstring& root) -> bool {
		const DWORD attr = GetFileAttributesW((root + L"Data\\Art").c_str());
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
	};

	// FAITHFUL precedence, matching such a mod's launch parameters (a submod ships a
	// chain like `Modpath=...\<Submod> Modpath=...<Core> Modpath=...`, LEFT = highest):
	// the selected submod stack first (front = highest, the core folder among them where the
	// user placed it), then the MOD ROOT LAST. The earlier mod-root-FIRST order was
	// inverted -- it let a stale file in the mod root (e.g. its old HardPointDataFiles.xml)
	// shadow the active submod's real one, which the game replaces the other way round.
	// The game REPLACES per file by precedence (never merges), so getFile's first-match
	// is faithful once the root order is right.
	for (const wstring& sub : submods)
	{
		if (sub.empty()) continue;
		const wstring subRoot = modpath + sub + L"\\";
		if (hasArtTree(subRoot))
			modContentRoots.push_back(subRoot);
	}

	// The mod root is the LOWEST-precedence mod layer (the game lists it last).
	modContentRoots.push_back(modpath);
}

// Stack-aware content-root setter (see managers.h). Delegates the
// canonicalize + existence-filter + dedup + slash-terminate logic to the pure
// modlayers::BuildContentRoots, supplying a real directory-existence predicate.
void FileManager::SetLayers(const vector<wstring>& absoluteLayers)
{
	modContentRoots = modlayers::BuildContentRoots(absoluteLayers,
		[](const wstring& dir) -> bool {
			const DWORD a = GetFileAttributesW(dir.c_str());
			return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
		});
}

vector<string> FileManager::listFiles(const string& directory, const string& extension) const
{
	auto upper = [](string s) {
		transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)toupper(c); });
		return s;
	};
	const string folder = upper(directory), suffix = upper(extension);
	set<string> names;
	auto add = [&](const string& name) {
		// Directly in the folder: no separator after it.
		const string path = upper(name);
		if (path.size() > folder.size() + suffix.size() && path.compare(0, folder.size(), folder) == 0 &&
			path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0 &&
			path.find_first_of("\\/", folder.size()) == string::npos)
			names.insert(path);
	};

	vector<wstring> roots(modContentRoots);
	roots.insert(roots.end(), basepaths.begin(), basepaths.end());
	for (const wstring& root : roots)
	{
		WIN32_FIND_DATAW found;
		HANDLE search = FindFirstFileW((root + AnsiToWide(folder) + L"*").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE) continue;
		do
		{
			if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			// A name the ANSI code page cannot spell, getFile cannot open either.
			const string name = WideToAnsi(found.cFileName, "?");
			if (name.find('?') == string::npos) add(folder + name);
		} while (FindNextFileW(search, &found));
		FindClose(search);
	}
	for (const MegaFile* mega : megafiles)
		for (const string& name : mega->getFilenames()) add(name);
	return vector<string>(names.begin(), names.end());
}

//
// Texture and shader managers
//

// [shader-gate] Headless diagnostic logger (stdout-flushed + debugger). Defined here so both
// TextureManager and ShaderManager can use it; HostWindowImpl::Log is not reachable from here.
static void ShaderLog(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	OutputDebugStringA(buf);
	fputs(buf, stdout);
	fflush(stdout);
}

// [shader-gate] Gate for the *verbose* per-asset diagnostics (every getTexture /
// getShader fetch, successful texture loads). Behind the ALO_SHADER_DIAG env var
// (matches the repo's ALO_* test hooks) so normal interactive use stays silent.
// Genuine failures (load/compile FAILED) call ShaderLog unconditionally and keep
// surfacing regardless of this gate.
static bool ShaderDiagEnabled()
{
	static int s_diag = -1;
	if (s_diag < 0) { char b[8]; s_diag = (GetEnvironmentVariableA("ALO_SHADER_DIAG", b, sizeof(b)) > 0) ? 1 : 0; }
	return s_diag != 0;
}

// The bytes of one of the executable's own resources: the fallback's identity.
static std::vector<unsigned char> EmbeddedResourceBytes(int id, WORD type)
{
	HMODULE module = GetModuleHandle(NULL);
	HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(type));
	HGLOBAL loaded = found ? LoadResource(module, found) : NULL;
	const unsigned char* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : NULL;
	return data ? std::vector<unsigned char>(data, data + SizeofResource(module, found)) : std::vector<unsigned char>();
}

static IDirect3DTexture9* createTexture(IDirect3DDevice9* pDevice, const std::vector<unsigned char>& bytes)
{
	IDirect3DTexture9* pTexture = NULL;
	HRESULT thr = D3DXCreateTextureFromFileInMemory( pDevice, bytes.data(), (unsigned long)bytes.size(), &pTexture );
	if (thr != D3D_OK)
	{
		ShaderLog("[tex-gate] D3DXCreateTextureFromFileInMemory FAILED hr=0x%08lx (%u bytes)\n",
		          (unsigned long)thr, (unsigned)bytes.size());
		return NULL;
	}
	{
		D3DSURFACE_DESC d; if (ShaderDiagEnabled() && pTexture && SUCCEEDED(pTexture->GetLevelDesc(0, &d)))
			ShaderLog("[tex-gate] loaded %ux%u fmt=%d\n", d.Width, d.Height, (int)d.Format);
	}
	return pTexture;
}

namespace {
class TextureDecoder : public IAssetDecoder<IDirect3DTexture9>
{
	IDirect3DDevice9* m_device;
public:
	explicit TextureDecoder(IDirect3DDevice9* device) : m_device(device) {}
	IDirect3DTexture9* Decode(const std::vector<unsigned char>& bytes) override { return createTexture(m_device, bytes); }
	IDirect3DTexture9* CreateFallback(std::vector<unsigned char>& bytes, std::string& resourceId) override
	{
		// Load and return default placeholder texture
		IDirect3DTexture9* pTexture = NULL;
		D3DXCreateTextureFromResource( m_device, GetModuleHandle(NULL), MAKEINTRESOURCE(IDB_MISSING), &pTexture );
		bytes = EmbeddedResourceBytes(IDB_MISSING, 2 /* RT_BITMAP */);
		resourceId = "IDB_MISSING";
		return pTexture;
	}
};
}

IDirect3DTexture9* TextureManager::getTexture(IDirect3DDevice9* pDevice, string filename)
{
	if (ShaderDiagEnabled()) ShaderLog("[tex-gate] getTexture(%s)\n", filename.c_str());
	TextureDecoder decoder(pDevice);
	return cache.Get(decoder, filename);
}

// Provenance by pointer identity: the placeholder (or nothing) is unresolved,
// whichever path produced it and whoever cached it first.
IDirect3DTexture9* TextureManager::getEmitterTexture(IDirect3DDevice9* pDevice, string filename)
{
	IDirect3DTexture9* pTexture = getTexture(pDevice, filename);
	if (filename.empty()) return pTexture;
	if (pTexture == NULL || pTexture == cache.Default())
	{
		resolved.erase(filename);
		unresolved.insert(filename);
	}
	else if (!unresolved.count(filename))
	{
		resolved.insert(filename);
	}
	return pTexture;
}

void TextureManager::ResetTextureAttestation()
{
	resolved.clear();
	unresolved.clear();
}

void TextureManager::TakeTextureAttestation(std::vector<std::string>& resolvedNames, std::vector<std::string>& unresolvedNames)
{
	resolvedNames.assign(resolved.begin(), resolved.end());
	unresolvedNames.assign(unresolved.begin(), unresolved.end());
	ResetTextureAttestation();
}

void TextureManager::Clear()
{
	cache.Clear();
}

void TextureManager::OnLostDevice()
{
	cache.Clear();
	cache.ReleaseDefault();
}

TextureManager::TextureManager(IFileManager* fileManager, const std::string& basePath)
	: cache("texture", fileManager, basePath, kTextureRetryExtension, kMaxTextureAssetBytes)
{
}

static Effect* createShader(IDirect3DDevice9* pDevice, const std::vector<unsigned char>& bytes)
{
	ID3DXEffect* pShader = NULL;
	ID3DXBuffer* pErrors = NULL;
	// [shader-gate] capture + surface D3DX compile errors (was swallowed: last arg NULL).
	if (FAILED(D3DXCreateEffect( pDevice, bytes.data(), (unsigned long)bytes.size(), NULL, NULL, D3DXFX_NOT_CLONEABLE, NULL, &pShader, &pErrors )))
	{
		if (pErrors != NULL)
		{
			ShaderLog("[shader-gate] D3DXCreateEffect FAILED: %.*s\n",
			          (int)pErrors->GetBufferSize(), (const char*)pErrors->GetBufferPointer());
			pErrors->Release();
		}
		else
		{
			ShaderLog("[shader-gate] D3DXCreateEffect FAILED (no error text)\n");
		}
		return NULL;
	}
	if (pErrors != NULL) pErrors->Release();

        D3DXHANDLE technique;
        pShader->FindNextValidTechnique(NULL, &technique);
        pShader->SetTechnique(technique);

	Effect* pEffect = new Effect(pShader);
        SAFE_RELEASE(pShader);
        return pEffect;
}

namespace {
class ShaderDecoder : public IAssetDecoder<Effect>
{
	IDirect3DDevice9* m_device;
public:
	explicit ShaderDecoder(IDirect3DDevice9* device) : m_device(device) {}
	Effect* Decode(const std::vector<unsigned char>& bytes) override { return createShader(m_device, bytes); }
	Effect* CreateFallback(std::vector<unsigned char>& bytes, std::string& resourceId) override
	{
		Effect* pShader = NULL;
		ID3DXEffect* pDefaultEffect;
		if (SUCCEEDED(D3DXCreateEffectFromResource( m_device, GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_DEFAULT_SHADER), NULL, NULL, D3DXFX_NOT_CLONEABLE, NULL, &pDefaultEffect, NULL)))
		{
			pShader = new Effect(pDefaultEffect);
			SAFE_RELEASE(pDefaultEffect);
		}
		bytes = EmbeddedResourceBytes(IDR_DEFAULT_SHADER, 10 /* RT_RCDATA */);
		resourceId = "IDR_DEFAULT_SHADER";
		return pShader;
	}
};
}

Effect* ShaderManager::getShader(IDirect3DDevice9* pDevice, string filename)
{
	if (ShaderDiagEnabled()) ShaderLog("[shader-gate] getShader(%s)\n", filename.c_str());
	ShaderDecoder decoder(pDevice);
	return cache.Get(decoder, filename);
}

void ShaderManager::Clear()
{
	cache.Clear();
}

void ShaderManager::OnLostDevice()
{
	for (Effect* effect : cache.Unique()) effect->OnLostDevice();
}

void ShaderManager::OnResetDevice()
{
	for (Effect* effect : cache.Unique()) effect->OnResetDevice();
}

ShaderManager::ShaderManager(IFileManager* fileManager, const std::string& basePath)
	: cache("shader", fileManager, basePath, ".FXO", kMaxShaderAssetBytes)
{
}
