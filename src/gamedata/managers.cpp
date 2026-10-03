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
			return new PhysicalFile(filename);
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
			return new PhysicalFile(filename);
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
			IFile* file = (*i)->getFile( path );
			if (file != NULL)
			{
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
						megafiles.push_back(new MegaFile(file));       // rc=2
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

IDirect3DTexture9* TextureManager::createTexture(IDirect3DDevice9* pDevice, const std::vector<unsigned char>& bytes)
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

IDirect3DTexture9* TextureManager::load(IDirect3DDevice9* pDevice, const string& filename)
{
	TextureMap::iterator p = textures.find(filename);
	if (p != textures.end())
	{
		// Texture has already been loaded
		return p->second;
	}

	IFile* file = fileManager->getFile( basePath + filename );
	if (file == NULL)
	{
		return NULL;
	}
	// ReadAndRelease consumes the IFile* reference
	// (which the previous code leaked) and enforces exact-byte reads.
	try
	{
		return createTexture(pDevice, ReadAndReleaseCapped(file, kMaxTextureAssetBytes));
	}
	catch (ReadException&)
	{
		return NULL;
	}
}

IDirect3DTexture9* TextureManager::getTexture(IDirect3DDevice9* pDevice, string filename)
{
	size_t pos;
	transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c) { return (char)toupper(c); });
	filename = SanitizeAssetName(filename);   // F-PATH: strip absolute/UNC/.. before any CreateFile
	if (ShaderDiagEnabled()) ShaderLog("[tex-gate] getTexture(%s)\n", filename.c_str());

	// Cache lookup FIRST. The cache is consulted inside load() below, but
	// the direct "file exists as specified" path never reaches load() when
	// it succeeds — so a repeat call for a texture that resolves at its
	// literal path re-read it from disk AND leaked the result: the tail's
	// textures.insert() silently no-ops on the existing key (std::map does
	// not overwrite), leaving the map holding the OLD texture while the
	// unconditional AddRef stranded the NEW one at refcount 1, referenced by
	// nothing.
	//
	// +1 to the caller matches what every other return path hands back.
	{
		TextureMap::iterator cached = textures.find(filename);
		if (cached != textures.end())
		{
			cached->second->AddRef();
			return cached->second;
		}
	}

	IDirect3DTexture9* pTexture = NULL;

	// See if the file exists as specified
	try
	{
		IFile* file = new PhysicalFile(AnsiToWide(filename));
		// ReadAndRelease handles exact-byte
		// reads and the IFile Release (was `delete file;` which
		// violated the refcounted IFile abstraction).
		try
		{
			pTexture = createTexture(pDevice, ReadAndReleaseCapped(file, kMaxTextureAssetBytes));
		}
		catch (ReadException&) {}
	}
	catch (FileNotFoundException&)
	{
	}

	if (pTexture == NULL)
	{
		// Use the part after the (back)slash, if any
            if (filename.find_first_of(":") != string::npos && (pos = filename.find_last_of("\\/")) != string::npos)
		{
			filename = filename.substr(pos + 1);
		}

		pTexture = load(pDevice, filename);
	}

	if (pTexture == NULL)
	{
		string name = filename;
		if ((pos = filename.rfind('.')) != string::npos)
		{
			name = name.substr(0, pos) + ".DDS";
		}
	
		pTexture = load(pDevice, name);
		if (pTexture == NULL)
		{
			// Load and return default placeholder texture
			if (pDefaultTexture == NULL)
			{
				D3DXCreateTextureFromResource( pDevice, GetModuleHandle(NULL), MAKEINTRESOURCE(IDB_MISSING), &pDefaultTexture );
			}

			if (pDefaultTexture != NULL)
			{
				pTexture = pDefaultTexture;
				pDefaultTexture->AddRef();
			}
		}
	}

	if (pTexture != NULL)
	{
		textures.insert(make_pair(filename, pTexture));
		pTexture->AddRef();
	}

	return pTexture;
}

void TextureManager::Clear()
{
	for (TextureMap::iterator p = textures.begin(); p != textures.end(); p++)
	{
		SAFE_RELEASE(p->second);
	}
	textures.clear();
}

void TextureManager::OnLostDevice()
{
	Clear();
	SAFE_RELEASE(pDefaultTexture);
}

TextureManager::TextureManager(IFileManager* fileManager, const std::string& basePath)
{
	this->basePath		  = basePath;
	this->fileManager	  = fileManager;
	this->pDefaultTexture = NULL;
}

TextureManager::~TextureManager()
{
	SAFE_RELEASE(pDefaultTexture);
	Clear();
}

Effect* ShaderManager::createShader(IDirect3DDevice9* pDevice, const std::vector<unsigned char>& bytes)
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

Effect* ShaderManager::load(IDirect3DDevice9* pDevice, const string& filename)
{
	ShaderMap::iterator p = shaders.find(filename);
	if (p != shaders.end())
	{
		// Texture has already been loaded
		return p->second;
	}

	IFile* file = fileManager->getFile( basePath + filename );
	if (file == NULL)
	{
		return NULL;
	}
	// ReadAndRelease consumes the IFile* reference
	// (was leaked) and enforces exact-byte reads.
	try
	{
		return createShader(pDevice, ReadAndReleaseCapped(file, kMaxShaderAssetBytes));
	}
	catch (ReadException&)
	{
		return NULL;
	}
}

Effect* ShaderManager::getShader(IDirect3DDevice9* pDevice, string filename)
{
	size_t pos;
	transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c) { return (char)toupper(c); });
	filename = SanitizeAssetName(filename);   // F-PATH: strip absolute/UNC/.. before any CreateFile
	if (ShaderDiagEnabled()) ShaderLog("[shader-gate] getShader(%s)\n", filename.c_str());

	// Cache lookup FIRST — identical shape to getTexture above, and the same
	// defect: the direct "file exists as specified" path below short-circuits
	// before load() (which owns the lookup) is ever reached, so a repeat call
	// recompiled the effect from disk and then stranded it at refcount 1 when
	// shaders.insert() no-oped on the existing key.
	{
		ShaderMap::iterator cached = shaders.find(filename);
		if (cached != shaders.end())
		{
			cached->second->AddRef();
			return cached->second;
		}
	}

	Effect* pShader = NULL;

	// See if the file exists as specified
	try
	{
		IFile* file = new PhysicalFile(AnsiToWide(filename));
		// ReadAndRelease handles exact-byte
		// reads and the IFile Release (was `delete file;` which
		// violated the refcounted IFile abstraction).
		try
		{
			pShader = createShader(pDevice, ReadAndReleaseCapped(file, kMaxShaderAssetBytes));
		}
		catch (ReadException&) {}
	}
	catch (FileNotFoundException&)
	{
	}

	if (pShader == NULL)
	{
		// Use the part after the (back)slash, if any
            if (filename.find_first_of(":") != string::npos && (pos = filename.find_last_of("\\/")) != string::npos)
		{
			filename = filename.substr(pos + 1);
		}

		pShader = load(pDevice, filename);
	}

	if (pShader == NULL)
	{
		string name = filename;
		if ((pos = filename.rfind('.')) != string::npos)
		{
			name = name.substr(0, pos) + ".FXO";
		}
	
		pShader = load(pDevice, name);
		if (pShader == NULL)
		{
			// Load and return default placeholder texture
			if (pDefaultShader == NULL)
			{
                    ID3DXEffect* pDefaultEffect;
				if (SUCCEEDED(D3DXCreateEffectFromResource( pDevice, GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_DEFAULT_SHADER), NULL, NULL, D3DXFX_NOT_CLONEABLE, NULL, &pDefaultEffect, NULL)))
                    {
                        pDefaultShader = new Effect(pDefaultEffect);
                        SAFE_RELEASE(pDefaultEffect);
                    }
			}

			if (pDefaultShader != NULL)
			{
				pShader = pDefaultShader;
				pDefaultShader->AddRef();
			}
		}
	}

	if (pShader != NULL)
	{
		shaders.insert(make_pair(filename, pShader));
		pShader->AddRef();
	}

	return pShader;
}

void ShaderManager::Clear()
{
	for (ShaderMap::iterator p = shaders.begin(); p != shaders.end(); p++)
	{
		SAFE_RELEASE(p->second);
	}
	shaders.clear();
}

void ShaderManager::OnLostDevice()
{
	std::set<Effect*> unique;
	if (pDefaultShader != NULL) unique.insert(pDefaultShader);
	for (const auto& entry : shaders)
	{
		if (entry.second != NULL) unique.insert(entry.second);
	}
	for (Effect* effect : unique) effect->OnLostDevice();
}

void ShaderManager::OnResetDevice()
{
	std::set<Effect*> unique;
	if (pDefaultShader != NULL) unique.insert(pDefaultShader);
	for (const auto& entry : shaders)
	{
		if (entry.second != NULL) unique.insert(entry.second);
	}
	for (Effect* effect : unique) effect->OnResetDevice();
}

ShaderManager::ShaderManager(IFileManager* fileManager, const std::string& basePath)
{
	this->basePath		 = basePath;
	this->fileManager	 = fileManager;
	this->pDefaultShader = NULL;
}

ShaderManager::~ShaderManager()
{
	SAFE_RELEASE(pDefaultShader);
	Clear();
}
