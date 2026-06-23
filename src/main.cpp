#define _WIN32_WINNT 0x0501
// Pull comctl32 v6 declarations (TVN_ITEMCHANGED / NMTVITEMCHANGE) — needed
// for's checkbox-tree cascade.
#define _WIN32_IE 0x0600
#include <cmath>
#include <iostream>
#include <iomanip>
#include <memory>
#include <string>
#include <algorithm>
#include <cfloat>
#include <sstream>
#include <queue>
#include <cstdlib>     // _set_abort_behavior (headless --capture: no abort dialog)
#include <crtdbg.h>    // _CrtSetReportMode/File (route Debug asserts to stderr)
#include <exception>   // std::set_terminate (log unhandled exceptions headlessly)
#include <cstdio>      // [shader-gate] headless diagnostic logging (stdout)
#include <cstdarg>     // [shader-gate] va_list for ShaderLog

#include "exceptions.h"
#include "UI/TexturePalette.h"
#include "SpawnerDriver.h"
#include "UndoStack.h"
#include "LinkGroup.h"
#include "Autosave.h"
#include "utils.h"
#include "engine.h"
#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include "Rescale.h"
#include "ParticleSystemIO.h"
#include "ModManager.h"
#include "resource.h"

//: the WebView2 + D3D9 host declared here is the ONLY UI — WinMain runs
// it unconditionally (the `--legacy` / `--legacy-ui` / `--new-ui` flags are
// gone; an unknown flag is ignored). The host requires Windows 10+ (DPI
// awareness v2, WebView2) and is x64-only (the only platform the build
// exposes).
#include "host/Run.h"
#include "host/WindowCapture.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commdlg.h>
using namespace std;

// Application version — single source of truth (also drives the binary's
// VS_VERSION_INFO in ParticleEditor.rc and the React About via vite.config.ts).
#include "version.h"

// Show up to this amount of files in the File menu
static const int NUM_HISTORY_ITEMS = 9;

static const int N_TRACKS          = 7;
static const int MIN_WINDOW_WIDTH  = 860;
static const int MIN_WINDOW_HEIGHT = 750;

//
// A class to measure the FPS
//
class FPSMeasurer
{
    static const int MAX_FRAMES = 32;

    float  m_frames[MAX_FRAMES];
    size_t m_iFrame;
    size_t m_nFrames;
    size_t m_lastFrame;
    size_t m_firstFrame;

public:
    float getFPS()
    {
        if (m_nFrames > 0)
        {
            float diff = (m_frames[m_lastFrame] - m_frames[m_firstFrame]);
            if (diff > 0.0f)
            {
                return m_nFrames / diff;
            }
        }
        return 0.0f;
    }

    void measure()
    {
        m_lastFrame = m_iFrame;
        m_frames[m_iFrame] = GetTickCount() / 1000.0f;
        m_nFrames   = min(m_nFrames + 1, MAX_FRAMES);
        m_iFrame    = (m_iFrame + 1) % MAX_FRAMES;
        if (m_iFrame == m_firstFrame)
        {
            m_firstFrame = (m_firstFrame + 1) % MAX_FRAMES;
        }
    }

    FPSMeasurer()
    {
        m_firstFrame = 0;
        m_lastFrame  = 0;
        m_iFrame     = 0;
        m_nFrames    = 0;
    }
};

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

class TextureManager : public ITextureManager
{
	typedef map<string,IDirect3DTexture9*> TextureMap;

	TextureMap			textures;
	string				basePath;
	IFileManager*		fileManager;
	IDirect3DTexture9*  pDefaultTexture;

	// F13+F14: takes the decoded bytes by reference rather
	// than an IFile* — file lifetime + exact-byte reads are handled by
	// ReadAndRelease at the call sites.
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

	IDirect3DTexture9* load(IDirect3DDevice9* pDevice, const string& filename)
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
		// F13: ReadAndRelease consumes the IFile* reference
		// (which the previous code leaked) and enforces exact-byte reads.
		try
		{
			return createTexture(pDevice, ReadAndRelease(file));
		}
		catch (ReadException&)
		{
			return NULL;
		}
	}

public:
	IDirect3DTexture9* getTexture(IDirect3DDevice9* pDevice, string filename)
	{
		size_t pos;
		transform(filename.begin(), filename.end(), filename.begin(), toupper);
		if (ShaderDiagEnabled()) ShaderLog("[tex-gate] getTexture(%s)\n", filename.c_str());
		IDirect3DTexture9* pTexture = NULL;

		// See if the file exists as specified
		try
		{
			IFile* file = new PhysicalFile(AnsiToWide(filename));
			// F13/F14/N4: ReadAndRelease handles exact-byte
			// reads and the IFile Release (was `delete file;` which
			// violated the refcounted IFile abstraction).
			try
			{
				pTexture = createTexture(pDevice, ReadAndRelease(file));
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

	void Clear()
	{
		for (TextureMap::iterator p = textures.begin(); p != textures.end(); p++)
		{
			SAFE_RELEASE(p->second);
		}
		textures.clear();
	}

	// F6: drop every cached resource (including the missing-
	// texture placeholder) for the device-reset path. Under D3D9Ex,
	// D3DXCreateTextureFromFileInMemory and D3DXCreateTextureFromResource
	// silently use D3DPOOL_DEFAULT — those handles are stale after
	// IDirect3DDevice9::Reset, so all of them must go. getTexture()
	// lazy-reloads on next call.
	void OnLostDevice() override
	{
		Clear();
		SAFE_RELEASE(pDefaultTexture);
	}

	TextureManager(IFileManager* fileManager, const std::string& basePath)
	{
		this->basePath		  = basePath;
		this->fileManager	  = fileManager;
		this->pDefaultTexture = NULL;
	}

	~TextureManager()
	{
		SAFE_RELEASE(pDefaultTexture);
		Clear();
	}
};

class ShaderManager : public IShaderManager
{
	typedef map<string,Effect*> ShaderMap;

	ShaderMap	  shaders;
	string		  basePath;
	IFileManager* fileManager;
	Effect*       pDefaultShader;

	// F13+F14: takes decoded bytes by reference rather than
	// an IFile* (file lifetime + exact-byte reads handled by
	// ReadAndRelease at call sites).
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

	Effect* load(IDirect3DDevice9* pDevice, const string& filename)
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
		// F13: ReadAndRelease consumes the IFile* reference
		// (was leaked) and enforces exact-byte reads.
		try
		{
			return createShader(pDevice, ReadAndRelease(file));
		}
		catch (ReadException&)
		{
			return NULL;
		}
	}

public:
	Effect* getShader(IDirect3DDevice9* pDevice, string filename)
	{
		size_t pos;
		transform(filename.begin(), filename.end(), filename.begin(), toupper);
		if (ShaderDiagEnabled()) ShaderLog("[shader-gate] getShader(%s)\n", filename.c_str());
		Effect* pShader = NULL;

		// See if the file exists as specified
		try
		{
			IFile* file = new PhysicalFile(AnsiToWide(filename));
			// F13/F14/N4: ReadAndRelease handles exact-byte
			// reads and the IFile Release (was `delete file;` which
			// violated the refcounted IFile abstraction).
			try
			{
				pShader = createShader(pDevice, ReadAndRelease(file));
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

	void Clear()
	{
		for (ShaderMap::iterator p = shaders.begin(); p != shaders.end(); p++)
		{
			SAFE_RELEASE(p->second);
		}
		shaders.clear();
	}

	ShaderManager(IFileManager* fileManager, const std::string& basePath)
	{
		this->basePath		 = basePath;
		this->fileManager	 = fileManager;
		this->pDefaultShader = NULL;
	}

	~ShaderManager()
	{
		SAFE_RELEASE(pDefaultShader);
		Clear();
	}
};

// MouseCursor + GetCursorPos3D were factored out to src/MouseCursor.h so
// the --new-ui host can reuse them. The header is included alongside
// ParticleSystemInstance.h above (line 23 brings in engine.h transitively;
// MouseCursor.h re-includes engine.h with its own guard).
#include "MouseCursor.h"


// Emitter duplicate-name helper (relocated here when arch-A's EmitterList.cpp
// was removed in). Returns "<base>_<n>" where <n> is one more than the
// highest numeric suffix already in use among emitters in `system` whose name
// matches `<base>` or `<base>_<digits>`. If `sourceName` itself ends in
// `_<digits>` that suffix is stripped first, so duplicating "Foo_3" repeatedly
// yields Foo_4, Foo_5 rather than Foo_3_1, Foo_3_1_1. An emitter named exactly
// `<base>` counts as n=0 for the purpose of picking the next free slot. Used by
// the host's BridgeDispatcher (duplicate / import emitter paths).
std::string GenerateDuplicateName(const ParticleSystem* system, const std::string& sourceName)
{
    auto trailingDigitCount = [](const std::string& s, size_t startAfter) -> size_t {
        size_t n = 0;
        for (size_t i = startAfter; i < s.size(); ++i)
        {
            if (!isdigit((unsigned char)s[i])) return 0;
            ++n;
        }
        return n;
    };

    std::string base = sourceName;
    size_t underscore = base.rfind('_');
    if (underscore != std::string::npos && trailingDigitCount(base, underscore + 1) > 0)
    {
        base.resize(underscore);
    }

    int maxN = 0;
    const std::vector<ParticleSystem::Emitter*>& emitters = system->getEmitters();
    for (size_t i = 0; i < emitters.size(); ++i)
    {
        const std::string& name = emitters[i]->name;
        if (name == base) continue;  // n=0; maxN already starts there
        if (name.size() > base.size() + 1 &&
            name.compare(0, base.size(), base) == 0 &&
            name[base.size()] == '_' &&
            trailingDigitCount(name, base.size() + 1) > 0)
        {
            int n = atoi(name.c_str() + base.size() + 1);
            if (n > maxN) maxN = n;
        }
    }

    char suffix[32];
    sprintf_s(suffix, sizeof(suffix), "_%d", maxN + 1);
    return base + suffix;
}

// ── Pure-IO ParticleSystem helpers (host-state plumbing) ─────────
//
// These free functions are declared in `src/ParticleSystemIO.h` and
// implemented here so the host's BridgeDispatcher can read/write .alo
// files without duplicating the PhysicalFile + ParticleSystem(IFile*)
// ctor dance.

std::unique_ptr<ParticleSystem> LoadParticleSystem(const std::wstring& path,
                                                   std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    PhysicalFile* file = NULL;
    try
    {
        file = new PhysicalFile(path);
    }
    catch (wexception& e)
    {
        if (errorOut) *errorOut = WideToAnsi(e.what());
        return nullptr;
    }
    catch (...)
    {
        if (errorOut) *errorOut = "could not open file";
        return nullptr;
    }

    std::unique_ptr<ParticleSystem> system;
    try
    {
        system.reset(new ParticleSystem(file));
    }
    catch (wexception& e)
    {
        if (errorOut) *errorOut = WideToAnsi(e.what());
        system.reset();
    }
    catch (...)
    {
        if (errorOut) *errorOut = "not a valid particle system";
        system.reset();
    }
    file->Release();
    return system;
}

bool SaveParticleSystem(ParticleSystem* system, const std::wstring& path,
                        std::string* errorOut)
{
    if (errorOut) errorOut->clear();
    if (system == NULL)
    {
        if (errorOut) *errorOut = "null particle system";
        return false;
    }
    PhysicalFile* file = NULL;
    try
    {
        file = new PhysicalFile(path, PhysicalFile::WRITE);
    }
    catch (wexception& e)
    {
        if (errorOut) *errorOut = WideToAnsi(e.what());
        return false;
    }
    catch (...)
    {
        if (errorOut) *errorOut = "could not open file for writing";
        return false;
    }

    bool ok = true;
    try
    {
        system->write(file);
    }
    catch (wexception& e)
    {
        if (errorOut) *errorOut = WideToAnsi(e.what());
        ok = false;
    }
    catch (...)
    {
        if (errorOut) *errorOut = "write failed";
        ok = false;
    }
    file->Release();
    return ok;
}


// EaW Gold Pack on Steam splits assets across "GameData" (base EaW) and
// "corruption" (FoC). Pointing the editor at one means missing textures from
// the other. If we detect either, also include the sibling.
static void AddSiblingGamePath(vector<wstring>& paths, const wstring& picked)
{
	wstring trimmed = picked;
	while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();

	size_t sep = trimmed.find_last_of(L"\\/");
	if (sep == wstring::npos) return;

	wstring parent = trimmed.substr(0, sep);
	wstring leaf   = trimmed.substr(sep + 1);
	wstring sibling;
	if (_wcsicmp(leaf.c_str(), L"corruption") == 0) sibling = parent + L"\\GameData";
	else if (_wcsicmp(leaf.c_str(), L"GameData") == 0) sibling = parent + L"\\corruption";
	else return;

	if (PathIsDirectory(sibling.c_str()))
	{
		paths.push_back(sibling);
	}
}

//
// Mods support — registry helpers (ReadModNickname / WriteModNickname /
// ReadLastMod / WriteLastMod) and disk scanning (ScanModsDir /
// DiscoverMods) moved to ModManager (src/ModManager.{h,cpp}) in D6.
// The nickname helpers are exposed via ModManager.h so the legacy
// nickname dialog WM_COMMAND can still write them directly without
// going through a ModManager method.
//

// View-state persistence. Registry layout under
// HKCU\Software\AloParticleEditor:
//   BackgroundColor (REG_DWORD)         — Engine::m_background COLORREF
//   ShowGround      (REG_DWORD, 0/1)    — Engine::m_showGround
//   CustomColors    (REG_BINARY, 64 b)  — ChooseColor's 16-slot palette
//
// Each helper takes a default that's returned when the value is absent
// or has the wrong type, so a fresh registry preserves today's behavior.
// Writes happen on every change; matches LastMod / ModNickname.


static FileManager* createFileManager( HWND hWnd, const vector<wstring>& argv, vector<wstring>* outGameRoots = NULL )
{
	// Search for the Empire at War path
	vector<wstring> EmpireAtWarPaths;
	if (argv.size() > 1)
	{
		// Override on the command line; use that
		for (size_t i = 1; i < argv.size(); i++)
		{
			if (PathIsDirectory(argv[i].c_str()))
			{
    			EmpireAtWarPaths.push_back(argv[i]);
            }
		}
	}

	if (EmpireAtWarPaths.empty())
	{
		// Try the previously-saved game path
		HKEY hKey;
		if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
		{
			TCHAR savedPath[MAX_PATH] = {0};
			DWORD type, size = sizeof(savedPath);
			if (RegQueryValueEx(hKey, L"GameDataPath", NULL, &type, (LPBYTE)savedPath, &size) == ERROR_SUCCESS && type == REG_SZ && savedPath[0] != L'\0')
			{
				EmpireAtWarPaths.push_back(savedPath);
				AddSiblingGamePath(EmpireAtWarPaths, savedPath);
			}
			RegCloseKey(hKey);
		}

		// Fall back to the current directory
		TCHAR buffer[MAX_PATH];
		GetCurrentDirectory(MAX_PATH, buffer);
		EmpireAtWarPaths.push_back(buffer);
	}
	FileManager* fileManager = NULL;
	wstring pickedPath;

	while (fileManager == NULL)
	{
		for (size_t i = 0; i < EmpireAtWarPaths.size(); i++)
		{
			if (*EmpireAtWarPaths[i].rbegin() != '\\') EmpireAtWarPaths[i] += '\\';
		}

		try
		{
			// Initialize the file manager
			fileManager = new FileManager( EmpireAtWarPaths );
		}
		catch (FileNotFoundException&)
		{
			// This path didn't work; ask the user to select a path
            const wstring title = LoadString(IDS_QUERY_DATA_PATH);

			BROWSEINFO bi;
			bi.hwndOwner      = hWnd;
			bi.pidlRoot       = NULL;
			bi.pszDisplayName = NULL;
			bi.lpszTitle      = title.c_str();
			bi.ulFlags        = BIF_RETURNONLYFSDIRS;
			bi.lpfn           = NULL;
			LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
			if (pidl == NULL)
			{
				fileManager = NULL;
				break;
			}

			TCHAR path[MAX_PATH];
			if (SHGetPathFromIDList( pidl, path ))
			{
				EmpireAtWarPaths.push_back(path);
				AddSiblingGamePath(EmpireAtWarPaths, path);
				pickedPath = path;
			}
			CoTaskMemFree(pidl);
		}
	}

	// If the user picked a path that worked, persist it for next launch.
	// --drive (ephemeral) must NOT persist — scan argv directly since the
	// driveScriptPath local is out of scope inside createFileManager.
	bool driveMode = false;
	for (const std::wstring& a : argv) if (a == L"--drive") { driveMode = true; break; }
	if (fileManager != NULL && !pickedPath.empty() && !driveMode)
	{
		HKEY hKey;
		if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
		{
			RegSetValueEx(hKey, L"GameDataPath", 0, REG_SZ, (const BYTE*)pickedPath.c_str(), (DWORD)((pickedPath.size() + 1) * sizeof(TCHAR)));
			RegCloseKey(hKey);
		}
	}
	if (outGameRoots != NULL && fileManager != NULL)
	{
		*outGameRoots = EmpireAtWarPaths;
	}
	return fileManager;
}


static vector<wstring> parseCommandLine()
{
	vector<wstring> argv;
	TCHAR* cmdline = GetCommandLine();

	bool quoted = false;
	wstring arg;
	for (TCHAR* p = cmdline; p == cmdline || *(p - 1) != '\0'; p++)
	{
		if (*p == '\0' || (*p == ' ' && !quoted))
		{
			if (arg != L"")
			{
				argv.push_back(arg);
				arg = L"";
			}
		}
		else if (*p == '"') quoted = !quoted;
		else arg += *p;
	}
	return argv;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
#ifndef NDEBUG
	AllocConsole();
	freopen("CONOUT$", "w", stdout);
#endif

	// Stable AppUserModelID. Without one, Windows derives a taskbar
	// identity from the .exe path; if the same path was previously
	// run with a different (or missing) icon, the taskbar caches a
	// generic glyph and stubbornly reuses it. Pinning an explicit ID
	// here gives the editor its own slot keyed off this name, so the
	// taskbar pulls the current WM_SETICON / WNDCLASS icon instead.
	// Loaded dynamically because the prototype lives in shell32 from
	// Windows 7 onwards and the project's _WIN32_WINNT is set to XP.
	if (HMODULE hShell32 = GetModuleHandleW(L"shell32.dll"))
	{
		typedef HRESULT (WINAPI *PFN_SetAppId)(PCWSTR);
		PFN_SetAppId pSet = (PFN_SetAppId)GetProcAddress(hShell32, "SetCurrentProcessExplicitAppUserModelID");
		if (pSet) pSet(L"DrKnickers.AloParticleEditor");
	}

	// Stage 2 — the WebView2 + D3D9 host is the ONLY UI. We build the
	// three managers the (now-dead) legacy main() built (so the host can
	// construct Engine identically) and run host::Run unconditionally; no
	// legacy WNDCLASS / windows are registered. The `--legacy` / `--legacy-ui`
	// / `--new-ui` flags are gone — an unknown flag is simply ignored.
	{
		const std::vector<std::wstring> argv = parseCommandLine();
		bool devUi    = false;
		bool testHost = false;
		// --gen-nt5-fixture <path>: one-shot CLI to produce a
		// .alo file with a known pre-singleton link group. Used
		// to exercise the load-time enforcement sweep at file/open.
		// Since the production save path always runs through
		// post-mutation enforcement (which would have demoted the
		// singleton already), no other code path can produce such a
		// file. This flag bypasses BridgeDispatcher entirely and
		// constructs the singleton state directly via the
		// ParticleSystem API + SaveParticleSystem.
		std::wstring genNt5FixturePath;
		std::wstring genA11yFixturePath;
		std::wstring genSmokeTestPath;   // [shaded-smoke] all-three test particle
		// [shaded-smoke] --smoke-color <warm|grey>: color ramp for the gen-smoke-test
		// plume. warm (default) = white->orange->red (proves vivid tint); grey =
		// light->dark grey (reads as real CoH-style smoke). Shader is tint-agnostic;
		// this only sets the demo fixture's per-life RGB tracks.
		int          smokeColorMode = 0;   // 0 = warm, 1 = grey
		// [shaded-smoke] --smoke-spin <on|off>: per-sprite rotation for the gen-smoke-test
		// plume. on (default) keeps randomRotation; off zeroes the spin so a directional
		// (lit-from-the-right) shader stays anchored across all puffs (option (c) demo).
		int          smokeSpin = 1;        // 1 = spin (default), 0 = no spin
		// rendering-fidelity] --capture <alo> <png> [--frames N]:
		// headless render-fidelity capture. Loads <alo>, renders N frames,
		// writes the engine's render target to <png>, exits. Implies the
		// --new-ui host path (it owns the engine). See src/host/Run.h.
		std::wstring captureAlo;
		// --capture-ref <objectName> <png>: render a game reference object
		// (with its shadow) headlessly instead of a particle system.
		std::wstring captureRef;
		std::wstring capturePng;
		std::wstring snapWindowPath;   // --snap-window <png>: screenshot the running editor, then exit
		bool         snapWindowFlagSeen = false;  // --snap-window present (with or without a path)
		// Default ~180 frames ≈ 3 s of sim (loop paces ~16 ms/frame) so a
		// freshly-spawned effect has time to fill before the snapshot.
		int          captureFrames = 180;
		// --skydome <slot>: render the capture with a background skydome
		// (0 = Off / solid colour — the default; 1-8 bundled scenes).
		int          captureSkydome = 0;
		// [world-lit] --ambient r,g,b / --sun r,g,b / --sun-intensity f:
		// drive scene lighting in a headless --capture run so a lit shader's
		// response can be verified offline. Each is opt-in; unset leaves the
		// engine's ctor-default lighting untouched.
		bool         captureHasAmbient = false; float captureAmbient[3] = {0,0,0};
		bool         captureHasSun = false;     float captureSun[3] = {0,0,0};
		bool         captureHasSunI = false;    float captureSunIntensity = 1.0f;
		// --drive <script.json>: scripted non-CDP composite capture, then exit.
		std::wstring driveScriptPath;
		for (size_t i = 1; i < argv.size(); ++i)
		{
			if (argv[i] == L"--dev-ui")    devUi    = true;
			if (argv[i] == L"--test-host") testHost = true;
			if (argv[i] == L"--drive")
			{
				// Require an explicit path: a bare --drive must NOT fall through
				// to a normal (persisting) launch — that would half-arm ephemeral
				// mode (the GameDataPath scan keys off the bare token) and clobber
				// the daily driver's settings. Fail loudly instead.
				if (i + 1 >= argv.size())
				{
					fwprintf(stderr, L"--drive requires a <script.json> path\n");
					return 2;
				}
				driveScriptPath = argv[i + 1];
				++i;
				continue;
			}
			if (argv[i] == L"--gen-nt5-fixture" && i + 1 < argv.size())
			{
				genNt5FixturePath = argv[i + 1];
			}
			if (argv[i] == L"--gen-a11y-fixture" && i + 1 < argv.size())
			{
				genA11yFixturePath = argv[i + 1];
			}
			if (argv[i] == L"--gen-smoke-test" && i + 1 < argv.size())
			{
				genSmokeTestPath = argv[i + 1];
			}
			if (argv[i] == L"--capture" && i + 2 < argv.size())
			{
				captureAlo = argv[i + 1];
				capturePng = argv[i + 2];
			}
			if (argv[i] == L"--capture-ref" && i + 2 < argv.size())
			{
				captureRef = argv[i + 1];
				capturePng = argv[i + 2];
			}
			if (argv[i] == L"--snap-window")
			{
				snapWindowFlagSeen = true;
				if (i + 1 < argv.size()) snapWindowPath = argv[i + 1];
			}
			if (argv[i] == L"--frames" && i + 1 < argv.size())
			{
				captureFrames = _wtoi(argv[i + 1].c_str());
			}
			// --skydome <slot>: apply a background skydome in --capture mode
			// (0 = Off / solid colour, 1-8 bundled).
			if (argv[i] == L"--skydome" && i + 1 < argv.size())
			{
				captureSkydome = _wtoi(argv[i + 1].c_str());
			}
			// [world-lit] --ambient r,g,b: scene ambient (0..1 floats).
			if (argv[i] == L"--ambient" && i + 1 < argv.size())
			{
				if (swscanf_s(argv[i + 1].c_str(), L"%f,%f,%f",
				              &captureAmbient[0], &captureAmbient[1], &captureAmbient[2]) == 3)
					captureHasAmbient = true;
				else
					fwprintf(stderr, L"--ambient: expected r,g,b floats, got '%s' -- ignored\n", argv[i + 1].c_str());
			}
			// [world-lit] --sun r,g,b: sun diffuse colour (0..1 floats).
			if (argv[i] == L"--sun" && i + 1 < argv.size())
			{
				if (swscanf_s(argv[i + 1].c_str(), L"%f,%f,%f",
				              &captureSun[0], &captureSun[1], &captureSun[2]) == 3)
					captureHasSun = true;
				else
					fwprintf(stderr, L"--sun: expected r,g,b floats, got '%s' -- ignored\n", argv[i + 1].c_str());
			}
			// [world-lit] --sun-intensity f: scalar folded into the sun colour.
			// Validate with swscanf_s (not _wtof, which silently returns 0.0 on
			// garbage -> a black sun with no warning).
			if (argv[i] == L"--sun-intensity" && i + 1 < argv.size())
			{
				float sunI = 0.0f;
				if (swscanf_s(argv[i + 1].c_str(), L"%f", &sunI) == 1)
				{
					captureSunIntensity = sunI;
					captureHasSunI = true;
				}
				else
					fwprintf(stderr, L"--sun-intensity: expected a float, got '%s' -- ignored\n", argv[i + 1].c_str());
			}
		}
		// --drive and --capture/--capture-ref are mutually-exclusive one-shots
		// (both arm a captureMode-style branch). Reject the combination loudly
		// rather than run an ambiguous mix. Checked BEFORE the capture/capture-ref
		// warning so an already-rejected run doesn't also print that noise.
		if (!driveScriptPath.empty() &&
		    (!captureAlo.empty() || !capturePng.empty() || !captureRef.empty()))
		{
			fwprintf(stderr, L"--drive cannot be combined with --capture/--capture-ref\n");
			return 2;
		}
		// Warn when both --capture and --capture-ref are supplied: --capture-ref
		// wins (last write to capturePng, captureRef takes the branch in HostWindow).
		// This used to be a silent surprise; make it explicit so operators notice.
		if (!captureAlo.empty() && !captureRef.empty())
			fprintf(stderr, "warning: both --capture and --capture-ref supplied; using --capture-ref\n");
		// Clamp a garbage/zero --frames back to the default.
		if (captureFrames < 1) captureFrames = 180;
		// A headless --capture must NEVER pop a modal CRT assert/abort dialog
		// -- it hangs the run AND disrupts the user's screen (and tells us nothing).
		// Route Debug asserts/errors + unhandled exceptions to stderr (captured to
		// the redirect file) so a crash is diagnosable headlessly. Interactive Debug
		// launches keep their dialogs.
#ifndef NDEBUG
		if ((!captureAlo.empty() || !captureRef.empty()) && !capturePng.empty())
		{
			setvbuf(stderr, nullptr, _IONBF, 0);   // unbuffered: pre-crash traces survive a terminate
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
			_CrtSetReportMode(_CRT_ERROR,  _CRTDBG_MODE_FILE);
			_CrtSetReportFile(_CRT_ERROR,  _CRTDBG_FILE_STDERR);
			std::set_terminate([]() {
				fprintf(stderr, "[capture] FATAL: std::terminate (unhandled exception)\n");
				try { if (auto e = std::current_exception()) std::rethrow_exception(e); }
				catch (wexception& we) { fwprintf(stderr, L"  wexception: %ls\n", we.what()); }
				catch (const std::exception& ex) { fprintf(stderr, "  std::exception: %s\n", ex.what()); }
				catch (...) { fprintf(stderr, "  (non-std exception)\n"); }
				fflush(stderr);
				_exit(3);
			});
		}
#endif
		if (!genNt5FixturePath.empty())
		{
			auto sys = std::make_unique<ParticleSystem>();
			sys->addRootEmitter();
			sys->addRootEmitter();
			auto& emitters = sys->getEmitters();
			if (emitters.size() < 2 || !emitters[0] || !emitters[1])
			{
				fwprintf(stderr, L"gen-nt5-fixture: failed to add 2 root emitters\n");
				return 2;
			}
			// Put both in link group 1 (transient — valid 2-member
			// state), then manually demote emitter 0 back to 0.
			// Result on disk: emitter 0 at linkGroup=0, emitter 1
			// alone at linkGroup=1 — the pre-"singleton group"
			// state that file/open's enforcement sweep must demote.
			emitters[0]->linkGroup = 1;
			emitters[1]->linkGroup = 1;
			emitters[0]->linkGroup = 0;
			std::string err;
			const bool ok = SaveParticleSystem(sys.get(), genNt5FixturePath, &err);
			if (!ok)
			{
				fwprintf(stderr, L"gen-nt5-fixture: SaveParticleSystem failed: %hs\n",
				         err.c_str());
				return 2;
			}
			fwprintf(stderr, L"gen-nt5-fixture: wrote %s "
			                 L"(emitter 0 linkGroup=0, emitter 1 linkGroup=1 — singleton)\n",
			         genNt5FixturePath.c_str());
			return 0;
		}
		// [shaded-smoke] --gen-smoke-test <path>: one root emitter set up to exercise the
		// all-three shaded-smoke shader -- blendMode 5 (Depth Transparent -> PrimDepthSpriteAlpha.fx),
		// big scale, several particles spread across a box, randomRotation on (so a single capture
		// shows varied sprite angles), and a per-life color ramp (young warm -> old red) on UN-aliased
		// R/G/B/A tracks (so per-particle tint is visible). Textures keep the default names so a mod
		// can override them (color + dome normal). Bypasses BridgeDispatcher; uses the API directly.
		if (!genSmokeTestPath.empty())
		{
			typedef ParticleSystem PS;
			typedef PS::Emitter::Track Track;
			auto sys = std::make_unique<ParticleSystem>();
			PS::Emitter* e = sys->addRootEmitter();
			if (e == nullptr) { fwprintf(stderr, L"gen-smoke-test: failed to add root emitter\n"); return 2; }
			e->blendMode           = PS::BLEND_TRANSPARENT;   // 2 -> PrimAlpha.fx (plain alpha blend, honors
			                                                   // per-pixel output alpha; depth-transparent
			                                                   // mode 5 squared the soft overlap)
			e->lifetime            = 4.0f;
			e->nParticlesPerSecond = 6;
			e->useBursts           = false;
			e->randomRotation      = true;     // varied spin angles -> per-sprite rotation visible
			e->noDepthTest         = true;     // smoke: no depth test -> overlapping soft orbs composite
			                                   // cleanly (depth-test in mode 5 hard-intersects quad footprints)
			// Engine: m_baseRotation = average * (1 + rand(-variance, variance)), in TURNS
			// (angle = 2*PI*rotation). With average 0 the variance is moot -> every sprite
			// starts at angle 0. average 0.5 / variance 1.0 => baseRotation in [0,1] turn =
			// a full 0..360 deg random start-angle spread. (randomRotation=true means the
			// rotation-SPEED track is ignored, so the start angle is the only spin source.)
			e->randomRotationAverage  = 0.5f;
			e->randomRotationVariance = 1.0f;
			e->textureSize         = 1;       // ATLAS FRAME COUNT (textureSizeSqrt=floor(sqrt)), NOT pixels!
			                                   // 1 => 1x1 atlas => quad UVs span the FULL 0..1. (256 gave a
			                                   // 16x16 atlas => each particle sampled a 1/16 UV tile => the
			                                   // near-constant-UV bug that broke all per-pixel lighting.)
			// spawn spread across the view (wide in X, tall in Z, thin in depth Y)
			e->groups[PS::GROUP_POSITION].type = PS::GT_BOX;
			e->groups[PS::GROUP_POSITION].minX = -90.0f; e->groups[PS::GROUP_POSITION].maxX = 90.0f;
			e->groups[PS::GROUP_POSITION].minY = -10.0f; e->groups[PS::GROUP_POSITION].maxY = 10.0f;
			e->groups[PS::GROUP_POSITION].minZ = -50.0f; e->groups[PS::GROUP_POSITION].maxZ = 50.0f;

			// [shaded-smoke] DENSE RISING PLUME override (later writes win). A compact
			// source slab at the bottom of frame + upward Z speed + zero gravity makes a
			// tapering smoke column; high spawn rate + growing scale make it read dense.
			// Kept within the capture camera's framed band (X ~+/-90, Z ~-50..+50).
			e->lifetime            = 4.0f;
			e->nParticlesPerSecond = 40;     // was 6 -> dense overlap into a column
			e->gravity             = 0.0f;   // rising smoke must not fall back down
			e->groups[PS::GROUP_POSITION].minX = -16.0f; e->groups[PS::GROUP_POSITION].maxX = 16.0f;
			e->groups[PS::GROUP_POSITION].minY = -10.0f; e->groups[PS::GROUP_POSITION].maxY = 10.0f;
			e->groups[PS::GROUP_POSITION].minZ = -46.0f; e->groups[PS::GROUP_POSITION].maxZ = -34.0f;
			// initial velocity: mostly up (Z), small lateral drift -> natural turbulence.
			// rise over 4 s life ~= Z*4 = 48..72 units, so from base ~-40 the column tops
			// out around +10..+30 (in frame) before the particle dies.
			e->groups[PS::GROUP_SPEED].type = PS::GT_BOX;
			e->groups[PS::GROUP_SPEED].minX = -4.0f; e->groups[PS::GROUP_SPEED].maxX = 4.0f;
			e->groups[PS::GROUP_SPEED].minY = -3.0f; e->groups[PS::GROUP_SPEED].maxY = 3.0f;
			e->groups[PS::GROUP_SPEED].minZ = 12.0f; e->groups[PS::GROUP_SPEED].maxZ = 18.0f;
			auto setTrack = [&](int id, float v0, float v1, Track::InterpolationType it)
			{
				e->trackContents[id].keys.clear();
				e->trackContents[id].interpolation = it;
				e->trackContents[id].keys.insert(Track::Key(0.0f, v0));
				e->trackContents[id].keys.insert(Track::Key(100.0f, v1));
				e->tracks[id] = &e->trackContents[id];   // un-alias from the Red channel
			};
			setTrack(PS::TRACK_SCALE,          35.0f, 50.0f, Track::IT_LINEAR);
			setTrack(PS::TRACK_RED_CHANNEL,     1.0f, 1.0f,  Track::IT_LINEAR);
			setTrack(PS::TRACK_GREEN_CHANNEL,   1.0f, 0.15f, Track::IT_LINEAR);
			setTrack(PS::TRACK_BLUE_CHANNEL,    0.7f, 0.05f, Track::IT_LINEAR);
			setTrack(PS::TRACK_ALPHA_CHANNEL,   0.9f, 0.9f,  Track::IT_LINEAR);
			setTrack(PS::TRACK_ROTATION_SPEED,  0.0f, 0.0f,  Track::IT_LINEAR);
			// [shaded-smoke] plume overrides of the tracks set above (later writes win):
			setTrack(PS::TRACK_SCALE,         25.0f, 90.0f, Track::IT_LINEAR);  // billow wider as it rises
			setTrack(PS::TRACK_ALPHA_CHANNEL,  0.85f, 0.0f, Track::IT_LINEAR);  // dissipate at the top
			// color mode from --smoke-color (parsed here to avoid touching the arg loop):
			// 0/warm (default, white->orange->red, set above) vs 1/grey (real-smoke look).
			for (size_t a = 1; a < argv.size(); ++a)
				if (argv[a] == L"--smoke-color" && a + 1 < argv.size())
					smokeColorMode = (argv[a + 1] == L"grey" || argv[a + 1] == L"gray") ? 1 : 0;
			if (smokeColorMode == 1)
			{
				setTrack(PS::TRACK_RED_CHANNEL,   0.90f, 0.40f, Track::IT_LINEAR);
				setTrack(PS::TRACK_GREEN_CHANNEL, 0.90f, 0.40f, Track::IT_LINEAR);
				setTrack(PS::TRACK_BLUE_CHANNEL,  0.92f, 0.42f, Track::IT_LINEAR);
			}
			// spin mode from --smoke-spin (parsed here like --smoke-color): on (default) vs
			// off. off zeroes per-sprite rotation so a directional shader's lit side stays
			// anchored across every puff -- the option (c) demo for "light from the right".
			for (size_t a = 1; a < argv.size(); ++a)
				if (argv[a] == L"--smoke-spin" && a + 1 < argv.size())
					smokeSpin = (argv[a + 1] == L"off" || argv[a + 1] == L"0") ? 0 : 1;
			if (smokeSpin == 0)
			{
				e->randomRotation         = false;
				e->randomRotationVariance = 0.0f;
			}
			// [bump-spin test] --bumpspin: reconfigure the generated emitter into a clean
			// GEOMETRIC-SPIN bump test -- mode 11, textureSize=1, the arrow+dome test textures,
			// a few large stationary sprites spinning continuously via the rotation-speed track
			// (randomRotation=false). Proves bump + spin + world-light headlessly (capture two frames:
			// the arrow turns; whether the dome's lit crescent stays sun-anchored answers the (B) Q).
			bool bumpSpin = false;
			for (size_t a = 1; a < argv.size(); ++a) if (argv[a] == L"--bumpspin") bumpSpin = true;
			if (bumpSpin)
			{
				e->blendMode           = 11;                       // Bump -> PrimParticleBumpAlpha.fx
				e->colorTexture        = "zz_bumptest_base0.tga";  // arrow base (shows spin)
				e->normalTexture       = "zz_bumptest_nm.tga";     // dome normal (shows bump)
				e->textureSize         = 1;
				e->lifetime            = 12.0f;
				e->nParticlesPerSecond = 2;
				e->gravity             = 0.0f;
				e->noDepthTest         = true;
				e->randomRotation      = false;                    // continuous spin from the speed track
				e->groups[PS::GROUP_POSITION].type = PS::GT_BOX;
				e->groups[PS::GROUP_POSITION].minX = -70; e->groups[PS::GROUP_POSITION].maxX = 70;
				e->groups[PS::GROUP_POSITION].minY =  -5; e->groups[PS::GROUP_POSITION].maxY =  5;
				e->groups[PS::GROUP_POSITION].minZ = -35; e->groups[PS::GROUP_POSITION].maxZ = 35;
				e->groups[PS::GROUP_SPEED].type = PS::GT_EXACT;
				e->groups[PS::GROUP_SPEED].valX = 0; e->groups[PS::GROUP_SPEED].valY = 0; e->groups[PS::GROUP_SPEED].valZ = 0;
				setTrack(PS::TRACK_SCALE,         40.0f, 40.0f, Track::IT_LINEAR);
				setTrack(PS::TRACK_RED_CHANNEL,    1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_GREEN_CHANNEL,  1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_BLUE_CHANNEL,   1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_ALPHA_CHANNEL,  1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_ROTATION_SPEED, 1.0f,  1.0f,  Track::IT_LINEAR);  // continuous spin
			}
			// [bump-spin SINGLE] --bumpspin1: like --bumpspin but exactly ONE large, centered,
			// long-lived sprite (single burst of 1 at the EXACT origin). Two captures at different
			// --frames then show the SAME sprite at two clean, NON-overlapping spin phases -> the
			// decisive §6 test (does the lit crescent track the spin or stay sun-anchored?).
			bool bumpSpin1 = false;
			for (size_t a = 1; a < argv.size(); ++a) if (argv[a] == L"--bumpspin1") bumpSpin1 = true;
			if (bumpSpin1)
			{
				e->blendMode           = 11;                       // Bump -> PrimParticleBumpAlpha.fx
				e->colorTexture        = "zz_bumptest_base0.tga";  // arrow base (shows spin phase)
				e->normalTexture       = "zz_bumptest_nm.tga";     // dome normal (the lit crescent)
				e->textureSize         = 1;
				e->lifetime            = 600.0f;                   // persists across the whole capture
				e->initialDelay        = 0.0f;
				e->gravity             = 0.0f;
				e->noDepthTest         = true;
				e->randomRotation      = false;                    // continuous spin from the speed track
				e->useBursts           = true;                     // a single burst...
				e->nBursts             = 1;                        // ...fired once...
				e->nParticlesPerBurst  = 1;                        // ...of exactly one particle
				e->burstDelay          = 10000.0f;                 // no second burst during any capture
				e->groups[PS::GROUP_POSITION].type = PS::GT_EXACT; // single centered point at origin
				e->groups[PS::GROUP_POSITION].valX = 0; e->groups[PS::GROUP_POSITION].valY = 0; e->groups[PS::GROUP_POSITION].valZ = 0;
				e->groups[PS::GROUP_SPEED].type = PS::GT_EXACT;
				e->groups[PS::GROUP_SPEED].valX = 0; e->groups[PS::GROUP_SPEED].valY = 0; e->groups[PS::GROUP_SPEED].valZ = 0;
				setTrack(PS::TRACK_SCALE,         60.0f, 60.0f, Track::IT_LINEAR);  // large, fills the frame
				setTrack(PS::TRACK_RED_CHANNEL,    1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_GREEN_CHANNEL,  1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_BLUE_CHANNEL,   1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_ALPHA_CHANNEL,  1.0f,  1.0f,  Track::IT_LINEAR);
				setTrack(PS::TRACK_ROTATION_SPEED, 1.0f,  1.0f,  Track::IT_LINEAR);  // continuous spin
			}
			// [bump-spin GRID] --bumpspin-grid: a DETERMINISTIC 4x3 constellation of large,
			// OFF-CENTER, overlapping, continuously-spinning mode-11 sprites at FIXED positions.
			// Camera-orbit flicker only manifests on off-center / reprojecting sprites (a centered
			// billboard at the look-at target is invariant under yaw -- proven empirically), so this
			// is the harness's PRIMARY flicker scene. Fixed positions + single burst-of-1 per cell =>
			// identical particle layout at every camera-yaw step (no random spawn churn to pollute
			// the inter-frame difference metric).
			bool bumpGrid = false;
			for (size_t a = 1; a < argv.size(); ++a) if (argv[a] == L"--bumpspin-grid") bumpGrid = true;
			if (bumpGrid)
			{
				const float gx[4] = { -52.0f, -17.0f, 17.0f, 52.0f };
				const float gz[3] = { -30.0f, 0.0f, 30.0f };
				auto cfgCell = [](PS::Emitter* em, float x, float z)
				{
					em->blendMode          = 11;                       // Bump -> PrimParticleBumpAlpha.fx
					em->colorTexture       = "zz_bumptest_base0.tga";  // arrow base (spin landmark)
					em->normalTexture      = "zz_bumptest_nm.tga";     // dome normal (puffy relief)
					em->textureSize        = 1;
					em->lifetime           = 600.0f;                   // persists across the whole capture
					em->initialDelay       = 0.0f;
					em->gravity            = 0.0f;
					em->noDepthTest        = true;
					em->randomRotation     = false;                    // continuous spin from the speed track
					em->useBursts          = true;
					em->nBursts            = 1;
					em->nParticlesPerBurst = 1;
					em->burstDelay         = 10000.0f;                 // no second burst during any capture
					em->groups[PS::GROUP_POSITION].type = PS::GT_EXACT;
					em->groups[PS::GROUP_POSITION].valX = x; em->groups[PS::GROUP_POSITION].valY = 0.0f; em->groups[PS::GROUP_POSITION].valZ = z;
					em->groups[PS::GROUP_SPEED].type = PS::GT_EXACT;
					em->groups[PS::GROUP_SPEED].valX = 0.0f; em->groups[PS::GROUP_SPEED].valY = 0.0f; em->groups[PS::GROUP_SPEED].valZ = 0.0f;
					const int   ids[6] = { PS::TRACK_SCALE, PS::TRACK_RED_CHANNEL, PS::TRACK_GREEN_CHANNEL,
					                       PS::TRACK_BLUE_CHANNEL, PS::TRACK_ALPHA_CHANNEL, PS::TRACK_ROTATION_SPEED };
					const float val[6] = { 40.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };  // scale, rgb, alpha, spin
					for (int t = 0; t < 6; ++t)
					{
						int id = ids[t];
						em->trackContents[id].keys.clear();
						em->trackContents[id].interpolation = Track::IT_LINEAR;
						em->trackContents[id].keys.insert(Track::Key(0.0f,   val[t]));
						em->trackContents[id].keys.insert(Track::Key(100.0f, val[t]));
						em->tracks[id] = &em->trackContents[id];
					}
				};
				bool first = true;
				for (int ix = 0; ix < 4; ++ix)
					for (int iz = 0; iz < 3; ++iz)
					{
						PS::Emitter* em = first ? e : sys->addRootEmitter();
						first = false;
						if (em == nullptr)
						{
							// A short grid silently invalidates the flicker metric (it relies on
							// the full fixed 4x3 layout) -- fail loudly like the single path above.
							fwprintf(stderr, L"gen-smoke-test: --bumpspin-grid failed to add root emitter (cell %d,%d)\n", ix, iz);
							return 2;
						}
						cfgCell(em, gx[ix], gz[iz]);
					}
			}
			std::string err;
			if (!SaveParticleSystem(sys.get(), genSmokeTestPath, &err))
			{
				fwprintf(stderr, L"gen-smoke-test: SaveParticleSystem failed: %hs\n", err.c_str());
				return 2;
			}
			// Report the ACTUAL root blendMode (the bump flags override it to 11; the base
			// smoke is 2 -- the old hardcoded "blendMode 5" was stale) and the emitter count
			// (the grid produces 12).
			fwprintf(stderr, L"gen-smoke-test: wrote %s (root blendMode %d, %zu emitter(s))\n",
			         genSmokeTestPath.c_str(), (int)e->blendMode, sys->getEmitters().size());
			return 0;
		}
		// T9.2] --gen-a11y-fixture <path>: one-shot CLI to produce a
		// .alo file with a 3-emitter tree (1 root + 1 lifetime child + 1
		// death child). Used by the T9 a11y specs so every surface driver's
		// beforeEach can open a deterministic file with enough tree depth to
		// exercise ArrowRight expand (kbd-arrow-tree-expanded) as well as the
		// simpler single-row surfaces. Bypasses BridgeDispatcher; uses the
		// ParticleSystem API directly (addRootEmitter / addLifetimeEmitter /
		// addDeathEmitter) and saves via SaveParticleSystem.
		if (!genA11yFixturePath.empty())
		{
			auto sys = std::make_unique<ParticleSystem>();
			// Root emitter
			ParticleSystem::Emitter* root = sys->addRootEmitter();
			if (root == nullptr)
			{
				fwprintf(stderr, L"gen-a11y-fixture: failed to add root emitter\n");
				return 2;
			}
			// Lifetime child (spawns during root's life)
			ParticleSystem::Emitter* lifetimeChild = sys->addLifetimeEmitter(root);
			if (lifetimeChild == nullptr)
			{
				fwprintf(stderr, L"gen-a11y-fixture: failed to add lifetime child emitter\n");
				return 2;
			}
			// Death child (spawns on root's death)
			ParticleSystem::Emitter* deathChild = sys->addDeathEmitter(root);
			if (deathChild == nullptr)
			{
				fwprintf(stderr, L"gen-a11y-fixture: failed to add death child emitter\n");
				return 2;
			}
			std::string err;
			const bool ok = SaveParticleSystem(sys.get(), genA11yFixturePath, &err);
			if (!ok)
			{
				fwprintf(stderr, L"gen-a11y-fixture: SaveParticleSystem failed: %hs\n",
				         err.c_str());
				return 2;
			}
			const auto& emitters = sys->getEmitters();
			fwprintf(stderr,
			         L"gen-a11y-fixture: wrote %s "
			         L"(%zu emitters: root[0], lifetime-child[1], death-child[2])\n",
			         genA11yFixturePath.c_str(),
			         emitters.size());
			return 0;
		}
		// --snap-window <png>: capture the already-running editor's composed window
		// (PrintWindow PW_RENDERFULLCONTENT on the AloHostMain top-level window) to a
		// PNG, then exit. No engine/WebView2 is created — it only screenshots an
		// existing window. Class literal kept in lockstep with kHostWindowClassName
		// (HostWindow.cpp).
		//
		// A bare --snap-window (flag present, no <png> path arg) must error out
		// rather than silently fall through and boot the full interactive editor.
		if (snapWindowFlagSeen && snapWindowPath.empty())
		{
			fwprintf(stderr, L"--snap-window requires a <png> path\n");
			return 2;
		}
		if (!snapWindowPath.empty())
		{
			return host::SnapWindowOneShot(L"AloHostMain", snapWindowPath);
		}
		// D6: capture gameRoots so the host can build its
		// ModManager. createFileManager already discovers them
		// internally; pass &gameRoots to extract.
		std::vector<std::wstring> gameRoots;
		FileManager* fileManager = createFileManager(NULL, argv, &gameRoots);
		if (fileManager == NULL)
		{
			// User cancelled the data-path picker. Exit cleanly with the
			// same -1 sentinel.
			return -1;
		}
		TextureManager textureManager(fileManager, "Data\\Art\\Textures\\");
		ShaderManager  shaderManager (fileManager, "Data\\Art\\Shaders\\");
		int hostResult = host::Run(hInstance, SW_SHOWDEFAULT,
		                           textureManager, shaderManager, *fileManager,
		                           gameRoots,
		                           devUi, testHost,
		                           captureAlo, capturePng, captureFrames,
		                           captureSkydome, captureRef,
		                           captureHasAmbient, captureAmbient[0], captureAmbient[1], captureAmbient[2],
		                           captureHasSun, captureSun[0], captureSun[1], captureSun[2],
		                           captureHasSunI, captureSunIntensity,
		                           driveScriptPath);
		delete fileManager;
#ifndef NDEBUG
		FreeConsole();
#endif
		return hostResult;
	}
}
