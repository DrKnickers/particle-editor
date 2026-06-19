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
#include "UI/UI.h"
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

// Task 1.3: the WebView2 + D3D9 host declared here is the DEFAULT UI
// on x64 — launching with no flag enters it; `--legacy` (alias `--legacy-ui`)
// opts back into the classic Win32 chrome. (`--new-ui` is kept as a no-op so the
// native test harness and old launch shortcuts keep working.) The host
// requires Windows 10+ (DPI awareness v2, WebView2) and is only built for
// x64 — the legacy Win32 configuration defaults to legacy and silently
// treats --new-ui as unrecognised. The .sln only exposes x64, so this gate
// is purely belt-and-suspenders against an out-of-tree Win32 build.
#ifdef _WIN64
#include "host/Run.h"
#endif

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

static INT_PTR CALLBACK AboutProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            HWND hVersion = GetDlgItem(hWnd, IDC_VERSION);
            wstring text = GetWindowStr(hVersion);
            text = FormatString(text.c_str(), PE_VERSION_MAJOR, PE_VERSION_MINOR, PE_VERSION_PATCH);
            SetWindowText(hVersion, text.c_str());
            
            HWND hBuildDate = GetDlgItem(hWnd, IDC_BUILDDATE);
            text = GetWindowStr(hBuildDate);
            const char* s = __DATE__;
            text = FormatString(text.c_str(), s);
            SetWindowText(hBuildDate, text.c_str());

            wstring copyright = LoadString(IDS_EXPAT_COPYRIGHT);
            SetWindowText(GetDlgItem(hWnd, IDC_EXPAT_COPYRIGHT), copyright.c_str());

            wstring disclaimer = LoadString(IDS_DISCLAIMER);
            SetWindowText(GetDlgItem(hWnd, IDC_DISCLAIMER), disclaimer.c_str());

            return TRUE;
        }

        case WM_COMMAND:
		{
			WORD code = HIWORD(wParam);
			WORD id   = LOWORD(wParam);
			if (code == BN_CLICKED && id == IDOK || id == IDCANCEL)
			{
                EndDialog(hWnd, 0);
            }
            break;
        }
    }
    return FALSE;
}

void ShowAboutDialog(HWND hWndParent)
{
    DialogBox(NULL, MAKEINTRESOURCE(IDD_ABOUT), hWndParent, AboutProc);
}

// Mod selection — `ModEntry` and the discovery / activation logic
// moved to src/ModManager.{h,cpp} in D6 so the same code can be
// reached by the new-UI bridge dispatcher.

// Reserved WM_COMMAND IDs for the dynamically-built Mods menu.
// Picked above the standard MFC range (0xE100+) and below 0xF000.
static const UINT ID_MOD_NONE     = 0xA000;
static const UINT ID_MOD_REFRESH  = 0xA001;
static const UINT ID_MOD_FIRST    = 0xA100;
static const UINT ID_MOD_LAST     = 0xAFFF;

// Header-strip spinner for the ground-plane Z offset. Lives on the
// row below the rebar alongside hLeaveParticles / hBackgroundLabel,
// since the rebar control doesn't forward WM_COMMAND from children
// out of the box. Above the IDC_* dialog-ID range (max ~1322) and
// below ID_MOD_NONE to avoid collisions in WM_COMMAND.
static const UINT ID_GROUNDZ_SPINNER          = 0x5000;
static const UINT ID_GROUND_TEXTURE_PREVIEW   = 0x5001;   // toolbar preview button
static const UINT ID_BACKGROUND_PREVIEW       = 0x5002;   // unified background preview (colour swatch or skydome thumbnail)

// Posted to the main window from WM_MENURBUTTONUP to defer the nickname
// dialog until after the menu's modal loop has finished tearing down.
// wParam = WM_COMMAND ID of the moused-over mod entry.
static const UINT WM_APP_SHOW_NICKNAME = WM_APP + 1;

// Forward declarations for the Mods support code (defined later).
// Discovery / activation / nickname-registry are now in ModManager
// (src/ModManager.h); these are legacy-Win32-menu-only.
struct APPLICATION_INFO;
static void             RebuildModsMenu(APPLICATION_INFO* info);
static void             SelectMod(APPLICATION_INFO* info, const wstring& modPath);
static ModEntry*        FindModById(APPLICATION_INFO* info, UINT id);
static bool             ShowNicknameDialog(HWND hParent, const ModEntry& mod, wstring& outNickname);
static const wstring&   ModDisplayLabel(const ModEntry& m);
static void             EnsureMenuFonts(APPLICATION_INFO* info);
// View-state persistence (defined later, near the other Read/Write helpers).
static COLORREF         ReadBackgroundColor(COLORREF defaultValue);
static void             WriteBackgroundColor(COLORREF color);
static bool             ReadShowGround(bool defaultValue);
static float            ReadGroundZ(float defaultValue);
static void             WriteGroundZ(float z);
static int              ReadGroundTexture(int defaultValue);
static void             WriteGroundTexture(int index);
static std::wstring     ReadGroundSlotPath(int slot);
static void             WriteGroundSlotPath(int slot, const std::wstring& path);
static void             DeleteAllGroundSlotPaths();
static COLORREF         ReadGroundSolidColor(COLORREF defaultValue);
static void             WriteGroundSolidColor(COLORREF color);
static HBITMAP          MakeGroundSlotThumbnail(IDirect3DDevice9* pDevice,
                                                  int slot, int size,
                                                  const std::wstring& customPath,
                                                  COLORREF solidColor);
static void             RebuildGroundTexturePreviewBitmap(APPLICATION_INFO* info);
static void             ShowGroundTexturePicker(HWND hParent, APPLICATION_INFO* info);
static HBITMAP          MakeSkydomeSlotThumbnail(IDirect3DDevice9* pDevice,
                                                   int slot, int sizePx,
                                                   const std::wstring& customPath,
                                                   COLORREF bgColor,
                                                   IFileManager* fileManager);
static void             RebuildBackgroundPreviewBitmap(APPLICATION_INFO* info);
static void             ShowSkydomePicker(HWND hParent, APPLICATION_INFO* info);
static void             WriteSkydomeIndex(int value);
static void             WriteSkydomeCustomPath(int slot, const std::wstring& path);
static bool             ReadSkydomePickerPos(RECT& out);
static void             WriteSkydomePickerPos(const RECT& in);
struct GroundPickerPos;
static void             WriteGroundPickerPos(int x, int y);
static bool             ReadBloomEnabled(bool defaultValue);
static void             WriteBloomEnabled(bool enabled);
static float            ReadBloomFloat(const wchar_t* name, float defaultValue);
static void             WriteBloomFloat(const wchar_t* name, float value);
static void             WriteShowGround(bool show);
static bool             ReadCustomColors(COLORREF out[16]);
static void             WriteCustomColors(const COLORREF in[16]);
static void             ResetViewSettings();
static bool             ReadSpawnerDialogPos(RECT& out);
static void             WriteSpawnerDialogPos(const RECT& in);
static void             ToggleSpawnerDialog(APPLICATION_INFO* info);
static void             ToggleBloomDialog(APPLICATION_INFO* info);
static void             ToggleLightingDialog(APPLICATION_INFO* info);
static void             PushLightingToEngine(Engine* engine);
static void             ApplyLightingDefaults(APPLICATION_INFO* info);
static bool             ReadLightingDialogPos(RECT& out);
static INT_PTR CALLBACK SpawnerDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

// Undo / redo helpers (defined alongside the WM_NOTIFY handlers).
static void             SetFileChanged(APPLICATION_INFO* info, bool changed);
static size_t           IndexOfEmitter(const ParticleSystem* sys, const ParticleSystem::Emitter* e);
static void             CaptureUndo(APPLICATION_INFO* info, DWORD coalesceKey);
static void             RestoreFromSnapshot(APPLICATION_INFO* info, const std::vector<char>& buf, size_t selIdx);
static void             UpdateUndoRedoUI(APPLICATION_INFO* info);
static void             DoUndo(APPLICATION_INFO* info);
static void             DoRedo(APPLICATION_INFO* info);
static void             SpawnerDlg_LoadFromConfig(HWND hDlg, const SpawnerConfig& cfg);

struct APPLICATION_INFO
{
	HINSTANCE hInstance;
	HWND      hMainWnd;
	HWND      hRenderWnd;
	bool	  isMinimized;

	map<ULONGLONG, wstring> history;

    HWND      hLeaveParticles;
    HWND      hBackgroundLabel;
    HWND      hBackgroundBtn;
    // rework: thumbnail bitmap painted on hBackgroundBtn when a
    // skydome slot is active. Stays NULL when SkydomeIndex == 0, in
    // which case the owner-draw paints a flat colour swatch instead.
    HBITMAP   hBackgroundPreviewBitmap;
    HWND      hGroundZLabel;
    HWND      hGroundZSpinner;
    HWND      hGroundTextureLabel;       // "Ground Texture:" caption
    HWND      hGroundTexturePreview;     // owner-drawn mini preview; click opens picker dialog
    // Thumbnail bitmap for the toolbar preview button. Regenerated
    // whenever the active slot or its texture changes; freed when
    // the editor exits.
    HBITMAP   hGroundTexturePreviewBitmap;
    HWND      hEmitterList;
	HWND      hPropertyTabs;
	HWND      hRebar;
	HWND	  hToolbar;
	HWND	  hStatusBar;
	HWND	  hTrackTabs;
	HWND      hTrackEditors[N_TRACKS];

    // Semi-transparent black overlay shown over the property tabs +
    // track editor when 2+ emitters are multi-selected (). Layered
    // child window (WS_EX_LAYERED + LWA_ALPHA). Created once in the
    // main window's WM_CREATE, repositioned in WM_SIZE, and shown /
    // hidden by SetEmitterInfo based on multi-select state.
    HWND      hMultiSelectOverlay;

	Engine*         engine;
	MouseCursor		mouseCursor;

	ParticleSystem*			 particleSystem;
    ParticleSystem::Emitter* selectedEmitter;
	ParticleSystemInstance*  attachedParticleSystem;

	wstring   filename;
	bool      changed;

	// Mod state — set up after createFileManager and used by the Mods menu.
	// D6: `mods` and `selectedModPath` moved into ModManager (single
	// source of truth shared with the new-UI bridge). The pointer below is
	// borrowed from a stack-local in WinMain; lifetime is the editor session.
	FileManager*    fileManager;     // non-owning; owned by main()
	TextureManager* textureManager;  // non-owning; owned by main()
	ShaderManager*  shaderManager;   // non-owning; owned by main()
	vector<wstring> gameRoots;       // the EmpireAtWarPaths used to construct fileManager
	ModManager*     modManager;      // non-owning; owned by main() stack
	HMENU           hModsMenu;       // top-level "Mods" submenu (HMENU of the popup)
	HFONT           hMenuFont;       // cached for owner-drawn mod entries
	HFONT           hMenuItalicFont; // italic variant for the nickname parenthetical

	// Programmable particle spawner (View → Spawner… / F7)
	SpawnerDriver*  spawner;            // owned; created in WinMain init
	HWND            hSpawnerDlg;        // NULL until first show; lazy-created
	bool            spawnerVisible;     // tracks visibility for menu check-mark
	RECT            spawnerWindowRect;  // last-known position (session + registry)

	// Ground-texture picker (modeless). Set when ShowGroundTexturePicker
	// creates the dialog; routed into the main message pump's
	// IsDialogMessage chain so Tab / Esc / arrow-key navigation work.
	HWND            hGroundPicker;

	//: skydome picker (modeless). Same lifecycle as hGroundPicker.
	// Reached through the unified Background button (no standalone toolbar
	// entry — slot 0 of the picker covers the flat-colour case).
	HWND            hSkydomePicker;

	// Bloom config dialog (View → Bloom… / Ctrl+B). Same modeless
	// toggle pattern as the spawner. Engine owns the bloom state
	// itself; this dialog is just a UI surface over it.
	HWND            hBloomDlg;
	bool            bloomDlgVisible;
	RECT            bloomDlgRect;

	//: Lighting dialog (View → Lighting…). Modeless, lifecycle
	// matches the Bloom dialog above. Engine owns the lighting state;
	// the dialog is a UI surface plus the source of truth for the
	// per-light az/tilt/intensity/color decomposition (the engine only
	// stores the resulting vec4s).
	HWND            hLightingDlg;
	bool            lightingDlgVisible;
	RECT            lightingDlgRect;

	// Undo / redo stack. Holds whole-system byte snapshots; cleared on
	// file open / new. See src/UndoStack.h for the rationale.
	UndoStack       undoStack;

	// Dragging
	enum { NONE, ROTATE, MOVE, ZOOM, OBJECT_Z } dragmode;
	long			xstart;
	long			ystart;
	Engine::Camera	startCam;
	bool            dragged;
    D3DXVECTOR3     dragStartPosition;
};

static void GetHistory(map<ULONGLONG, wstring>& history)
{
	history.clear();

	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS)
	{
		LONG error;
		for (int i = 0;; i++)
		{
			TCHAR  name[256] = {'\0'};
			DWORD length = 255;
			DWORD type, size;
			if ((error = RegEnumValue(hKey, i, name, &length, NULL, &type, NULL, &size)) != ERROR_SUCCESS)
			{
				break;
			}

			if (type == REG_BINARY && size == sizeof(FILETIME))
			{
				FILETIME filetime;
				if (RegQueryValueEx(hKey, name, NULL, &type, (BYTE*)&filetime, &size) != ERROR_SUCCESS)
				{
					break;
				}
				ULARGE_INTEGER largeint;
				largeint.LowPart  = filetime.dwLowDateTime;
				largeint.HighPart = filetime.dwHighDateTime;
				history.insert(make_pair(largeint.QuadPart, name));
			}
		}

		if (error == ERROR_NO_MORE_ITEMS)
		{
			// Graceful loop end, now delete everything older than the X-th oldest item
			map<ULONGLONG,wstring>::const_reverse_iterator p = history.rbegin();
			for (int j = 0; p != history.rend() && j < NUM_HISTORY_ITEMS; p++, j++);

			// Now start deleting
			for (; p != history.rend(); p++)
			{
				RegDeleteValue(hKey, p->second.c_str());
			}
		}

		RegCloseKey(hKey);
	}
}

// Adds the Alo Viewer history to the file menu
static bool AppendHistory(APPLICATION_INFO* info, HWND hWnd)
{
	// Get the history (timestamp, filename pairs)
	GetHistory(info->history);

	HMENU hMenu = GetMenu(hWnd);
	hMenu = GetSubMenu(hMenu, 0);

	MENUITEMINFO mii;
	mii.cbSize = sizeof(MENUITEMINFO);
	mii.fMask  = MIIM_TYPE;
	mii.cch    = 0;

	// Find the first seperator
	int i = 0;
	do {
		if (!GetMenuItemInfo(hMenu, i++, true, &mii))
		{
			return false;
		}
	} while (mii.fType != MFT_SEPARATOR);

	// Delete everything after it until only Exit's left
	mii.fMask = MIIM_ID;
	while (GetMenuItemInfo(hMenu, i, true, &mii) && mii.wID != ID_FILE_EXIT)
	{
		DeleteMenu(hMenu, i, MF_BYPOSITION);
	}

	if (!info->history.empty())
	{
		int j = 0;
		for (map<ULONGLONG,wstring>::const_reverse_iterator p = info->history.rbegin(); p != info->history.rend() && j < NUM_HISTORY_ITEMS; p++, i++, j++)
		{
			wstring name = p->second.c_str();

			HDC hDC = GetDC(info->hMainWnd);
			SelectObject(hDC, GetStockObject(DEFAULT_GUI_FONT));
			PathCompactPath(hDC, (LPTSTR)name.c_str(), 400);
			ReleaseDC(info->hMainWnd, hDC);

			if (j < 9)
			{
				name = wstring(L"&") + (TCHAR)(L'1' + j) + L" " + name;
			}
			InsertMenu(hMenu, i, MF_BYPOSITION | MF_STRING, ID_FILE_HISTORY_0 + j, name.c_str());
		}

		// Finally, add the seperator
		InsertMenu(hMenu, i, MF_BYPOSITION | MF_SEPARATOR, 0, 0);
	}
	return true;
}

// Adds this filename to the history
static void AddToHistory(const wstring& name)
{
	// Get the current date & time
	FILETIME   filetime;
	SYSTEMTIME systime;
	GetSystemTime(&systime);
	SystemTimeToFileTime(&systime, &filetime);

	HKEY hKey;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, name.c_str(), 0, REG_BINARY, (BYTE*)&filetime, sizeof(FILETIME));
		RegCloseKey(hKey);
	}
}

static void SetEmitterInfo(APPLICATION_INFO* info)
{
	bool show = (info->particleSystem != NULL && info->selectedEmitter != NULL);

    if (show)
    {
        EmitterProps_SetEmitter(info->hPropertyTabs, info->selectedEmitter);
	    for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
	    {
            TrackEditor_SetTrack(info->hTrackEditors[i], info->selectedEmitter->trackContents, info->selectedEmitter->tracks);
	    }

        TrackEditor_EnableTrack(info->hTrackEditors[ParticleSystem::TRACK_ROTATION_SPEED], !info->selectedEmitter->randomRotation);
    }

	ShowWindow(info->hPropertyTabs, show ? SW_SHOW : SW_HIDE);
	ShowWindow(info->hTrackTabs,    show ? SW_SHOW : SW_HIDE);
	for (int i = 0; i < N_TRACKS; i++)
	{
		ShowWindow(info->hTrackEditors[i], (show && TabCtrl_GetCurSel(info->hTrackTabs) == i) ? SW_SHOW : SW_HIDE);
	}

    // Multi-select indicator (). When 2+ emitters are selected,
    // disable the inspector controls (so any clicks / spinner drags
    // don't mutate the primary unexpectedly) AND show a 50%-alpha
    // black overlay over the panels so the disabled state is
    // unambiguous regardless of whether the custom controls paint
    // themselves greyed.
    bool multiEditing = (info->hEmitterList != NULL &&
                          EmitterList_GetMultiSelectionSize(info->hEmitterList) >= 2);
    BOOL canEdit = (show && !multiEditing) ? TRUE : FALSE;
    EnableWindow(info->hPropertyTabs, canEdit);
    EnableWindow(info->hTrackTabs,    canEdit);
    for (int i = 0; i < N_TRACKS; i++)
    {
        EnableWindow(info->hTrackEditors[i], canEdit);
    }
    if (info->hMultiSelectOverlay != NULL)
    {
        BOOL showOverlay = (show && multiEditing);
        if (showOverlay)
        {
            SetWindowPos(info->hMultiSelectOverlay, HWND_TOP,
                          0, 0, 0, 0,
                          SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        else
        {
            ShowWindow(info->hMultiSelectOverlay, SW_HIDE);
        }
    }
}

//
// Undo / redo
//
// Edit boundaries: every change to the live ParticleSystem funnels
// through one of three notifications (EP_CHANGE, TE_CHANGE,
// ELN_LISTCHANGED) handled below in WM_NOTIFY, plus the system-wide
// hLeaveParticles checkbox. After the change has landed in the model
// we call CaptureUndo with a coalesce key that decides whether the
// new state replaces the previous entry (rapid-fire spinner ticks,
// curve-key drag) or starts a fresh one (structural ops). The Undo /
// Redo paths swap the entire ParticleSystem out from under the editor
// and re-resolve the selected emitter by index.

static size_t IndexOfEmitter(const ParticleSystem* sys,
                             const ParticleSystem::Emitter* e)
{
    if (sys == NULL || e == NULL) return SIZE_MAX;
    const std::vector<ParticleSystem::Emitter*>& v = sys->getEmitters();
    for (size_t i = 0; i < v.size(); i++)
    {
        if (v[i] == e) return i;
    }
    return SIZE_MAX;
}

#ifndef NDEBUG
#define UNDO_LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
static void DumpSystem(const char* tag, const ParticleSystem* sys)
{
    if (sys == NULL) { UNDO_LOG("[Undo] %s sys=NULL\n", tag); fflush(stdout); return; }
    const std::vector<ParticleSystem::Emitter*>& v = sys->getEmitters();
    UNDO_LOG("[Undo] %s emitters=%zu\n", tag, v.size());
    for (size_t i = 0; i < v.size(); i++)
    {
        const ParticleSystem::Emitter* e = v[i];
        // %zd works for size_t-as-signed; -1 prints as -1 with %zd.
        long sd = (e->spawnDuringLife == (size_t)-1) ? -1 : (long)e->spawnDuringLife;
        long sd2 = (e->spawnOnDeath    == (size_t)-1) ? -1 : (long)e->spawnOnDeath;
        long parentIdx = -1;
        if (e->parent != NULL)
        {
            for (size_t j = 0; j < v.size(); j++) if (v[j] == e->parent) { parentIdx = (long)j; break; }
        }
        UNDO_LOG("[Undo]   [%zu] '%s' tex='%s' size=%lu sDL=%ld sOD=%ld parent=%ld vis=%d\n",
               i, e->name.c_str(), e->colorTexture.c_str(),
               (unsigned long)e->textureSize, sd, sd2, parentIdx, e->visible ? 1 : 0);
    }
    fflush(stdout); fflush(stderr);
}
#else
#define DumpSystem(tag, sys) ((void)0)
#endif

static void CaptureUndo(APPLICATION_INFO* info, DWORD coalesceKey)
{
    if (info == NULL || info->particleSystem == NULL) return;
    if (info->undoStack.IsApplying()) return;

    // Link-group propagation (). If the edited emitter is in a
    // link group, copy its non-exempt parameters to every sibling
    // before snapshotting, so one user action produces one undo step
    // that covers the whole group's new state. The snapshot below
    // captures the whole system, so no special multi-emitter undo
    // machinery is required.
    if (info->selectedEmitter != NULL && info->selectedEmitter->linkGroup != 0)
    {
        std::vector<ParticleSystem::Emitter*> siblings
            = GetLinkGroupMembers(*info->particleSystem,
                                    info->selectedEmitter->linkGroup);
        //: consult the group's per-group exempt set instead of
        // the static v1 defaults. Groups without a custom entry fall
        // back to GetDefaultLinkExemptFlags() inside the accessor.
        const LinkExemptFlags& exempt
            = info->particleSystem->getLinkExemptFlags(
                info->selectedEmitter->linkGroup);
        for (size_t i = 0; i < siblings.size(); i++)
        {
            if (siblings[i] != info->selectedEmitter)
            {
                siblings[i]->copySharedParamsFrom(*info->selectedEmitter, exempt);
            }
        }
#ifndef NDEBUG
        UNDO_LOG("[Link] propagate group=%u members=%zu edited='%s'\n",
                 info->selectedEmitter->linkGroup,
                 siblings.size(),
                 info->selectedEmitter->name.c_str());
#endif
    }

    size_t selIdx = IndexOfEmitter(info->particleSystem, info->selectedEmitter);
    bool pushed = info->undoStack.Capture(*info->particleSystem, selIdx, coalesceKey);
#ifndef NDEBUG
    UNDO_LOG("[Undo] capture key=0x%08lx sel=%zu emitters=%zu -> %s, depth=%zu cursor=%zu\n",
           (unsigned long)coalesceKey, selIdx,
           info->particleSystem->getEmitters().size(),
           pushed ? "pushed" : "coalesced",
           info->undoStack.Depth(), info->undoStack.Cursor());
    fflush(stdout); fflush(stderr);
#else
    (void)pushed;
#endif
    UpdateUndoRedoUI(info);
}

static void RestoreFromSnapshot(APPLICATION_INFO* info,
                                 const std::vector<char>& buf,
                                 size_t selIdx)
{
    info->undoStack.BeginApplying();

    ParticleSystem* sys = NULL;
    try
    {
        sys = UndoStack::Deserialize(buf);
    }
    catch (...)
    {
        info->undoStack.EndApplying();
        return;
    }
    if (sys == NULL)
    {
        info->undoStack.EndApplying();
        return;
    }

    // Order matches LoadFile + OnFileChange so the safety guarantees
    // line up: while EmitterList_SetParticleSystem is mid-rebuild,
    // TreeView_DeleteAllItems can fire TVN_SELCHANGED with item.lParam
    // pointing at a now-freed Emitter from the old system. The handler
    // bubbles ELN_SELCHANGED to main.cpp's pump, which only reads from
    // the model when info->particleSystem != NULL. Keep it NULL across
    // the rebuild and SetEmitterInfo will safely bail.
    //
    // Bug history: an earlier draft installed sys / selectedEmitter
    // BEFORE the rebuild, which let SetEmitterInfo dereference the
    // stale tree-item lParam → "child emitter vanished and the editor
    // crashed" (PR self-test feedback).
    if (info->engine != NULL) info->engine->Clear();
    info->attachedParticleSystem = NULL;  // engine->Clear() invalidated it
    delete info->particleSystem;
    info->particleSystem  = NULL;
    info->selectedEmitter = NULL;

    EmitterList_SetParticleSystem(info->hEmitterList, sys);

    // Install the new system, then re-select the emitter that was
    // active at capture time. EmitterList_SetParticleSystem auto-
    // selects the first root; if the user was editing a child, that
    // would jump them off it — call EmitterList_SelectEmitter to
    // override. TreeView_SelectItem fires TVN_SELCHANGED →
    // ELN_SELCHANGED → main.cpp's pump sets info->selectedEmitter.
    info->particleSystem = sys;
    if (selIdx < sys->getEmitters().size())
    {
        EmitterList_SelectEmitter(info->hEmitterList, &sys->getEmitter(selIdx));
    }
    info->selectedEmitter = EmitterList_GetSelection(info->hEmitterList);

    SendMessage(info->hLeaveParticles, BM_SETCHECK,
                sys->getLeaveParticles() ? BST_CHECKED : BST_UNCHECKED, 0);
    SetEmitterInfo(info);

    if (info->engine != NULL) info->engine->OnParticleSystemChanged(-1);

    SetFileChanged(info, !info->undoStack.IsAtSavedState());
    UpdateUndoRedoUI(info);

    info->undoStack.EndApplying();
}

static void UpdateUndoRedoUI(APPLICATION_INFO* info)
{
    if (info == NULL) return;
    bool canUndo = info->undoStack.CanUndo();
    bool canRedo = info->undoStack.CanRedo();
    if (info->hToolbar != NULL)
    {
        SendMessage(info->hToolbar, TB_ENABLEBUTTON, ID_EDIT_UNDO, MAKELONG(canUndo, 0));
        SendMessage(info->hToolbar, TB_ENABLEBUTTON, ID_EDIT_REDO, MAKELONG(canRedo, 0));
    }
}

static void DoUndo(APPLICATION_INFO* info)
{
    // Belt-and-braces guard against the accelerator gate at main.cpp's
    // pump regressing — a Restore mid-drag would free the Emitter the
    // drag holds a pointer to. The pump should already block this,
    // but the cost is two lines and the value is "we don't crash even
    // if the pump regresses."
    if (EmitterList_IsDragging(info->hEmitterList)) return;

    const std::vector<char>* buf = NULL;
    size_t selIdx = SIZE_MAX;
    if (info->undoStack.Undo(&buf, &selIdx) && buf != NULL)
    {
#ifndef NDEBUG
        UNDO_LOG("[Undo] UNDO bufBytes=%zu selIdx=%zu cursor=%zu\n",
               buf->size(), selIdx, info->undoStack.Cursor());
        fflush(stdout); fflush(stderr);
#endif
        RestoreFromSnapshot(info, *buf, selIdx);
#ifndef NDEBUG
        if (info->particleSystem != NULL)
        {
            UNDO_LOG("[Undo]   restored emitters=%zu\n",
                   info->particleSystem->getEmitters().size());
            fflush(stdout); fflush(stderr);
        }
#endif
    }
}

static void DoRedo(APPLICATION_INFO* info)
{
    if (EmitterList_IsDragging(info->hEmitterList)) return;  // see DoUndo above

    const std::vector<char>* buf = NULL;
    size_t selIdx = SIZE_MAX;
    if (info->undoStack.Redo(&buf, &selIdx) && buf != NULL)
    {
#ifndef NDEBUG
        UNDO_LOG("[Undo] REDO bufBytes=%zu selIdx=%zu cursor=%zu\n",
               buf->size(), selIdx, info->undoStack.Cursor());
        fflush(stdout); fflush(stderr);
#endif
        RestoreFromSnapshot(info, *buf, selIdx);
#ifndef NDEBUG
        if (info->particleSystem != NULL)
        {
            UNDO_LOG("[Undo]   restored emitters=%zu\n",
                   info->particleSystem->getEmitters().size());
            fflush(stdout); fflush(stderr);
        }
#endif
    }
}

static void SetFileChanged(APPLICATION_INFO* info, bool changed)
{
    info->changed = changed;

	// Load the proper name in the title bar
	wstring name = GetWindowStr(info->hMainWnd);
	size_t pos = name.find_first_of('-');
	if (pos != wstring::npos)
	{
		name = name.substr(0, pos - 1);
	}

	if (system != NULL)
	{
		name += L" - [" + (info->filename == L"" ? LoadString(IDS_TITLE_NEW_FILE) : info->filename);
		if (info->changed)
		{
			name += L"*";
		}
		name += L"]";
	}
	SetWindowText(info->hMainWnd, name.c_str());
}

static void OnFileChange(APPLICATION_INFO* info, ParticleSystem* system)
{
    // Suppress capture for the duration of the file-change handler.
    // EmitterList_SetParticleSystem and SetEmitterInfo dispatch
    // notifications during their setup; without the guard those would
    // push spurious entries into the stack we're about to reset.
    info->undoStack.BeginApplying();

    SetFileChanged(info, false);

    // Update the emitter list
    EmitterList_SetParticleSystem(info->hEmitterList, system);

    // Set the emitter property panel
    info->particleSystem = system;

    if (info->particleSystem != NULL)
    {
        // Set the global particle system info
        SendMessage(info->hLeaveParticles, BM_SETCHECK, info->particleSystem->getLeaveParticles() ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    // Set the selected emitter info
    SetEmitterInfo(info);

    // Reset the undo stack and seed it with the load-time baseline so
    // the first Ctrl+Z after opening a file rewinds back to the loaded
    // state, not into nothing. Mark this snapshot as "saved" so the
    // title-bar asterisk clears when the user undoes back to it.
    info->undoStack.Clear();
    info->undoStack.EndApplying();   // re-enable Capture for the seed

    if (system != NULL)
    {
        size_t selIdx = IndexOfEmitter(system, info->selectedEmitter);
        info->undoStack.Capture(*system, selIdx, 0);
        info->undoStack.MarkSaved();
    }
    UpdateUndoRedoUI(info);
}

// Format a FILETIME's age (now - ft) into "X seconds" or "X minutes"
// for the recovery prompt. Coarse units only — the user doesn't need
// "3 minutes 42 seconds" precision.
static wstring FormatAge(const FILETIME& ft)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = ft.dwLowDateTime;  a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return L"just now";
    ULONGLONG diffSec = (b.QuadPart - a.QuadPart) / 10000000ULL;
    wchar_t buf[64];
    if (diffSec < 60)        swprintf_s(buf, 64, L"%llu seconds ago", diffSec);
    else if (diffSec < 3600) swprintf_s(buf, 64, L"%llu minutes ago", diffSec / 60);
    else                     swprintf_s(buf, 64, L"%llu hours ago",   diffSec / 3600);
    return buf;
}

// Build and show the recovery prompt. Returns IDYES / IDNO / IDCANCEL
// per the MessageBox conventions; the caller maps these to restore-
// recent / restore-stable / discard.
//
// Button mapping varies depending on which tiers are available:
//   - Both:        MB_YESNOCANCEL — Yes=recent, No=stable, Cancel=discard
//   - Recent only: MB_YESNO       — Yes=recent, No=discard
//   - Stable only: MB_YESNO       — Yes=stable, No=discard
//
// Callers interpret the return separately for each case (see startup
// flow below).
static int ShowRecoveryPrompt(APPLICATION_INFO* info, const Autosave::OrphanSession& s)
{
    wstring originalLabel = s.originalFilename.empty()
                          ? L"Unsaved new file"
                          : s.originalFilename;

    wstring msg = L"Unsaved changes detected from a previous session.\n\nOriginal: ";
    msg += originalLabel;
    msg += L"\n\n";

    UINT flags = MB_ICONQUESTION;
    bool hasRecent = !s.recentPath.empty();
    bool hasStable = !s.stablePath.empty();

    if (hasRecent && hasStable)
    {
        flags |= MB_YESNOCANCEL;
        msg += L"[Yes]    Restore most recent autosave from "  + FormatAge(s.recentMtime) + L"\n";
        msg += L"[No]     Restore stable backup from "         + FormatAge(s.stableMtime) + L"\n";
        msg += L"[Cancel] Discard and start fresh";
    }
    else if (hasRecent)
    {
        flags |= MB_YESNO;
        msg += L"[Yes] Restore autosave from "        + FormatAge(s.recentMtime) + L"\n";
        msg += L"[No]  Discard and start fresh";
    }
    else  // stable-only
    {
        flags |= MB_YESNO;
        msg += L"[Yes] Restore stable backup from " + FormatAge(s.stableMtime) + L"\n";
        msg += L"[No]  Discard and start fresh";
    }

    return MessageBoxW(info->hMainWnd, msg.c_str(),
                      L"Particle Editor — Recover unsaved changes?", flags);
}

// Load `restorePath` as the current particle system but display it as
// if it were `originalFilename`. Sets info->changed = true so the
// title-bar asterisk shows and Ctrl+S targets the original. Unlike
// LoadFile, doesn't push the temp path into the file-history menu —
// the user shouldn't see %TEMP%\...\autosave-1234-recent.alo as a
// recent file.
static bool RestoreFromAutosave(APPLICATION_INFO* info,
                                const wstring& restorePath,
                                const wstring& originalFilename)
{
    if (info->engine != NULL) info->engine->Clear();
    delete info->particleSystem;
    info->particleSystem = NULL;

    PhysicalFile* file = new PhysicalFile(restorePath);
    ParticleSystem* system = NULL;
    try
    {
        system = new ParticleSystem(file);
        file->Release();
    }
    catch (wexception& e)
    {
        system = NULL;
        file->Release();
        MessageBox(info->hMainWnd, LoadString(IDS_ERROR_FILE_OPEN, e.what()).c_str(),
                   NULL, MB_OK | MB_ICONERROR);
    }
    if (system == NULL) return false;

    // Pretend we loaded the original — info->filename drives the
    // title bar and the Ctrl+S target. OnFileChange would normally
    // SetFileChanged(false); override afterwards so the title shows
    // the asterisk (the recovered content is still unsaved relative
    // to the on-disk original).
    info->filename = originalFilename;
    OnFileChange(info, system);
    SetFileChanged(info, true);
    return true;
}

// forward decl (definition lives between NicknameDialog and createFileManager).
static void DoImportEmittersFromFile(APPLICATION_INFO* info);
// reuses the name-collision helper from EmitterList.cpp.
extern std::string GenerateDuplicateName(const ParticleSystem* system, const std::string& sourceName);

// ── Pure-IO ParticleSystem helpers (host-state plumbing) ─────────
//
// These free functions are declared in `src/ParticleSystemIO.h` and
// implemented here so the new-UI BridgeDispatcher (which knows nothing
// about `APPLICATION_INFO*`) can read/write .alo files without
// duplicating the PhysicalFile + ParticleSystem(IFile*) ctor dance.
// Legacy `LoadFile` / `DoSaveFile` / `ImportEmitters_LoadFile` below
// are rewritten to delegate to these helpers and retain their UI
// side-effects (dialogs, autosave flush, history append, etc.) so the
// `--legacy-ui` codepaths keep working unchanged from the caller's
// perspective.

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

static bool LoadFile(APPLICATION_INFO* info, const wstring& filename)
{
	// Delete old particle system
	if (info->engine != NULL)
	{
		info->engine->Clear();
	}
	delete info->particleSystem;
	info->particleSystem = NULL;

    //: delegate the actual PhysicalFile + ParticleSystem(IFile*)
    // round-trip to the pure-IO helper so the new-UI host can reuse it.
    // The helper swallows wexception internally and reports the message
    // via errorOut; the legacy UI side-effect (MessageBox + history
    // append + OnFileChange) stays here.
    std::string err;
    std::unique_ptr<ParticleSystem> owned = LoadParticleSystem(filename, &err);
    ParticleSystem* system = owned.release();
    if (system != NULL)
    {
        info->filename = filename;
    }
    else
    {
        MessageBox(info->hMainWnd,
                   LoadString(IDS_ERROR_FILE_OPEN, AnsiToWide(err).c_str()).c_str(),
                   NULL, MB_OK | MB_ICONERROR);
    }

    if (system != NULL)
    {
	    // Add it to the history
        AddToHistory(info->filename);
        AppendHistory(info, info->hMainWnd);
    }

    OnFileChange(info, system);
	return (system != NULL);
}

static void OpenHistoryFile(APPLICATION_INFO* info, int idx)
{
	// Find the correct entry
	map<ULONGLONG, wstring>::const_reverse_iterator p = info->history.rbegin();
	for (int j = 0; p != info->history.rend() && idx > 0; idx--, p++);
	if (p != info->history.rend())
	{
		LoadFile(info, p->second);
	}
}

static void DoNewFile(APPLICATION_INFO* info)
{
	info->particleSystem  = NULL;
	info->selectedEmitter = NULL;
    ParticleSystem* system = new ParticleSystem();
    system->addRootEmitter();
    OnFileChange(info, system);
    // Discarding in-progress work — the brand-new file has nothing
    // worth recovering, and any leftover autosave from the previous
    // session here would be confusing.
    Autosave::DeleteOurSession();
}

static bool DoOpenFile(APPLICATION_INFO* info)
{
	// Query for the  file
	TCHAR filename[MAX_PATH];
	filename[0] = L'\0';

    wstring filter = LoadString(IDS_FILES_ALO) + wstring(L" (*.alo)\0*.ALO\0", 15)
                   + LoadString(IDS_FILES_ALL) + wstring(L" (*.*)\0*.*\0", 11);

	OPENFILENAME ofn;
	memset(&ofn, 0, sizeof(OPENFILENAME));
	ofn.lStructSize  = sizeof(OPENFILENAME);
	ofn.hwndOwner    = info->hMainWnd;
	ofn.hInstance    = info->hInstance;
    ofn.lpstrFilter  = filter.c_str();
	ofn.nFilterIndex = 1;
	ofn.lpstrFile    = filename;
	ofn.nMaxFile     = MAX_PATH;
	ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
	if (GetOpenFileName(&ofn) == 0)
	{
		return false;
	}

    return LoadFile(info, filename);
}

static bool DoSaveFile(APPLICATION_INFO* info, bool saveas = false)
{
	if (info->filename == L"")
	{
		saveas = true;
	}

	if (saveas)
	{
		// Query for the filename
		TCHAR filename[MAX_PATH];
		filename[0] = L'\0';

        wstring filter = LoadString(IDS_FILES_ALO) + wstring(L" (*.alo)\0*.ALO\0", 15)
                       + LoadString(IDS_FILES_ALL) + wstring(L" (*.*)\0*.*\0", 11);

        OPENFILENAME ofn;
		memset(&ofn, 0, sizeof(OPENFILENAME));
		ofn.lStructSize  = sizeof(OPENFILENAME);
		ofn.hwndOwner    = info->hMainWnd;
		ofn.hInstance    = info->hInstance;
        ofn.lpstrFilter  = filter.c_str();
        ofn.lpstrDefExt  = L"alo";
		ofn.nFilterIndex = 1;
		ofn.lpstrFile    = filename;
		ofn.nMaxFile     = MAX_PATH;
		ofn.Flags        = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
		if (GetSaveFileName( &ofn ) == 0)
		{
			return false;
		}
		info->filename = filename;
	}

    // Create particleSystem name from filename (lowercase basename
    // without extension). Match the previous behaviour exactly so the
    // on-disk shape doesn't shift.
    {
        wstring name = info->filename;
        size_t pos = name.find_last_of('\\');
        if (pos != wstring::npos) name = name.substr(pos + 1);
        pos = name.find_last_of('.');
        if (pos != wstring::npos) name = name.substr(0, pos);
        transform(name.begin(), name.end(), name.begin(), tolower);
        info->particleSystem->setName(WideToAnsi(name,"_"));
    }

    //: delegate the actual write to the pure-IO helper. The
    // legacy UI side-effect (MessageBox on failure + undo-stack
    // bookkeeping + autosave flush) stays here.
    std::string err;
    if (!SaveParticleSystem(info->particleSystem, info->filename, &err))
    {
        MessageBox(info->hMainWnd,
                   LoadString(IDS_ERROR_FILE_SAVE, AnsiToWide(err).c_str()).c_str(),
                   NULL, MB_OK | MB_ICONERROR);
        // Save failed (disk full, permission denied, writer I/O
        // exception): keep the dirty flag, leave the undo stack
        // unmarked, and KEEP the autosave so recovery stays available.
        // Return false so DoCheckChanges aborts the close/new/open it
        // was guarding -- otherwise the user loses the document
        // believing it was saved. The host twin (BridgeDispatcher
        // file/save) already gates its bookkeeping the same way.
        return false;
    }
    SetFileChanged(info, false);
    info->undoStack.MarkSaved();
    UpdateUndoRedoUI(info);
    // User just saved — the on-disk file is now authoritative; the
    // autosave is no longer needed for recovery. Delete both tiers
    // so we don't leave orphans that would prompt for recovery on
    // next launch.
    Autosave::DeleteOurSession();
	return true;
}

static bool DoCheckChanges(APPLICATION_INFO* info)
{
	if (info->particleSystem != NULL)
	{
		if (info->changed)
		{
			switch (MessageBox(info->hMainWnd, LoadString(IDS_QUERY_SAVE_CHANGES).c_str(), LoadString(IDS_WARNING).c_str(), MB_YESNOCANCEL | MB_ICONQUESTION))
			{
				case IDYES:    return DoSaveFile(info);
				case IDCANCEL: return false;
			}
		}
	}
	return true;
}

static bool DoCloseFile(APPLICATION_INFO* info)
{
	if (info->particleSystem != NULL)
	{
		if (!DoCheckChanges(info))
		{
			return false;
		}

        if (info->engine != NULL)
        {
		    info->engine->Clear();
        }
		delete info->particleSystem;
		info->particleSystem         = NULL;
		info->attachedParticleSystem = NULL;
	}
	info->filename   = L"";
	info->selectedEmitter = NULL;
    OnFileChange(info, NULL);
    // Closing means the in-memory work is being discarded (with the
    // user's consent via DoCheckChanges if it was dirty). The
    // autosave from this session is no longer meaningful — clear it
    // so it doesn't surface as orphan-from-crash on next launch.
    Autosave::DeleteOurSession();
	return true;
}

static void DoMenuInit(HMENU hMenu, APPLICATION_INFO* info)
{
    EnableMenuItem(hMenu, ID_EDIT_UNDO, MF_BYCOMMAND | (info->undoStack.CanUndo() ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, ID_EDIT_REDO, MF_BYCOMMAND | (info->undoStack.CanRedo() ? MF_ENABLED : MF_GRAYED));

    EnableMenuItem(hMenu, ID_EDIT_CLEARALLPARTICLES, MF_BYCOMMAND | (info->engine == NULL || info->engine->GetNumInstances() > 0 ? MF_ENABLED : MF_GRAYED ));

    EnableMenuItem(hMenu, ID_NEW_EMITTER_LIFETIME,      MF_BYCOMMAND | (info->selectedEmitter != NULL && info->selectedEmitter->spawnDuringLife == -1 ? MF_ENABLED : MF_GRAYED ));
    EnableMenuItem(hMenu, ID_NEW_EMITTER_DEATH,         MF_BYCOMMAND | (info->selectedEmitter != NULL && info->selectedEmitter->spawnOnDeath    == -1 ? MF_ENABLED : MF_GRAYED ));
    EnableMenuItem(hMenu, ID_EMITTER_RENAME,            MF_BYCOMMAND | (info->selectedEmitter != NULL ? MF_ENABLED : MF_GRAYED ));
    EnableMenuItem(hMenu, ID_EMITTER_RESCALE,           MF_BYCOMMAND | (info->selectedEmitter != NULL ? MF_ENABLED : MF_GRAYED ));
    EnableMenuItem(hMenu, ID_TOGGLE_EMITTER_VISIBILITY, MF_BYCOMMAND | (info->selectedEmitter != NULL ? MF_ENABLED : MF_GRAYED ));

    CheckMenuItem (hMenu, ID_VIEW_SHOWGROUND, MF_BYCOMMAND | (info->engine != NULL && info->engine->GetGround()     ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem (hMenu, ID_VIEW_DEBUGHEAT,  MF_BYCOMMAND | (info->engine != NULL && info->engine->GetHeatDebug()  ? MF_CHECKED : MF_UNCHECKED));

    EnableMenuItem(hMenu, ID_VIEW_RELOAD_SHADERS,  MF_BYCOMMAND | (info->engine != NULL ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, ID_VIEW_RELOAD_TEXTURES, MF_BYCOMMAND | (info->engine != NULL ? MF_ENABLED : MF_GRAYED));

    // Pause / frame-step preview. The step entries are meaningful only
    // while paused; greying them communicates "press Pause first".
    bool paused = IsPreviewPaused();
    CheckMenuItem (hMenu, ID_VIEW_PAUSE_PREVIEW,  MF_BYCOMMAND | (paused ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(hMenu, ID_VIEW_STEP_1_FRAME,   MF_BYCOMMAND | (paused ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(hMenu, ID_VIEW_STEP_10_FRAMES, MF_BYCOMMAND | (paused ? MF_ENABLED : MF_GRAYED));
}

// Spawner dt accumulator. Lives at file scope (rather than as a static
// inside Render) so the frame-step path in DoStepFrames can update it
// after manually ticking the spawner — otherwise the next natural
// Render would re-apply the elapsed step time and double-tick.
static TimeF g_spawnerLastFrameTime = 0.0f;

// Frame-step the preview by N notional 60 Hz frames. Implemented as a
// loop of N small (1/60 s) integrations rather than one big (N/60 s)
// integration so spawner-owned moving instances interpolate cleanly:
// the projectile's position advances in N small hops, and the smoke
// emitters spawn a particle at each intermediate position, producing
// a continuous trail. A single big dt would jump the projectile by
// N×Δp in one shot and leave a visible gap in the trail (since the
// smoke emitter only gets one spawn opportunity at the post-jump
// position).
//
// `g_spawnerLastFrameTime` is reset after the loop so the natural
// Render-loop doesn't re-apply the elapsed step time and double-tick
// the spawner. No-op when not paused.
static void DoStepFrames(APPLICATION_INFO* info, int frames)
{
    if (!IsPreviewPaused() || info->engine == NULL || frames <= 0) return;

    const float dtPerFrame = 1.0f / 60.0f;
    for (int i = 0; i < frames; i++)
    {
        StepPreviewFrames(1);
        if (info->spawner != NULL)
        {
            info->spawner->Tick(dtPerFrame, info->particleSystem, info->engine);
        }
        info->engine->Update();
    }
    g_spawnerLastFrameTime = GetTimeF();

    if (info->hRenderWnd != NULL)
    {
        InvalidateRect(info->hRenderWnd, NULL, FALSE);
    }
}

static bool DoMenuItem(APPLICATION_INFO* info, UINT id)
{
	switch (id)
	{
		case ID_FILE_NEW:     if (DoCloseFile   (info)) DoNewFile(info); break;
		case ID_FILE_OPEN:	  if (DoCheckChanges(info)) DoOpenFile(info); break;
		case ID_FILE_EXIT:    if (DoCheckChanges(info)) DestroyWindow(info->hMainWnd); break;
		case ID_FILE_SAVE:    DoSaveFile(info); break;
		case ID_FILE_IMPORT_EMITTERS: DoImportEmittersFromFile(info); break;
		case ID_FILE_SAVE_AS: DoSaveFile(info, true); break;

        case ID_EDIT_UNDO:    DoUndo(info); break;
        case ID_EDIT_REDO:    DoRedo(info); break;
        case ID_EDIT_COPY:    SendMessage(GetFocus(), WM_COPY,  0, 0); break;
        case ID_EDIT_CUT:     SendMessage(GetFocus(), WM_CUT,   0, 0); break;
        case ID_EDIT_PASTE:   SendMessage(GetFocus(), WM_PASTE, 0, 0); break;
        case ID_EDIT_DELETE:  SendMessage(GetFocus(), WM_CLEAR, 0, 0); break;
        case ID_EDIT_RESCALE:
            if (RescaleParticleSystem(info->hMainWnd, info->particleSystem))
            {
                SetEmitterInfo(info);
                SetFileChanged(info, true);
            }
            break;

        case ID_EDIT_CLEARALLPARTICLES:
            if (info->engine != NULL)
            {
                info->engine->Clear();
            }
            break;

        case ID_NEW_EMITTER_ROOT:          EmitterList_AddRootEmitter(info->hEmitterList); break;
        case ID_NEW_EMITTER_LIFETIME:      EmitterList_AddLifetimeEmitter(info->hEmitterList); break;
        case ID_NEW_EMITTER_DEATH:         EmitterList_AddDeathEmitter(info->hEmitterList); break;
        case ID_EMITTER_RENAME:            EmitterList_RenameEmitter(info->hEmitterList); break;
        case ID_TOGGLE_EMITTER_VISIBILITY: EmitterList_ToggleEmitterVisibility(info->hEmitterList); break;
        case ID_SHOW_ALL_EMITTERS:         EmitterList_SetAllEmitterVisibility(info->hEmitterList, true);  break;
        case ID_HIDE_ALL_EMITTERS:         EmitterList_SetAllEmitterVisibility(info->hEmitterList, false); break;
        case ID_EMITTERS_RESCALE:
            if (info->selectedEmitter != NULL)
            {
                if (RescaleEmitter(info->hMainWnd, info->selectedEmitter))
                {
                    SetEmitterInfo(info);
                    SetFileChanged(info, true);
                }
            }
            break;

		case ID_VIEW_SHOWGROUND:
            if (info->engine != NULL)
            {
			    info->engine->SetGround(!info->engine->GetGround());
			    SendMessage(info->hToolbar, TB_CHECKBUTTON, id, MAKELONG(info->engine->GetGround(), 0));
                WriteShowGround(info->engine->GetGround());
                // Grey the Ground Z label + spinner in lockstep with the
                // toggle so the disabled state communicates "this knob
                // doesn't do anything right now."
                EnableWindow(info->hGroundZLabel,   info->engine->GetGround());
                EnableWindow(info->hGroundZSpinner, info->engine->GetGround());
            }
			break;

		case ID_VIEW_DEBUGHEAT:
            if (info->engine != NULL)
            {
			    info->engine->SetHeatDebug(!info->engine->GetHeatDebug());
			    SendMessage(info->hToolbar, TB_CHECKBUTTON, id, MAKELONG(info->engine->GetHeatDebug(), 0));
            }
			break;

        case ID_EMITTER_SPAWNER:
            ToggleSpawnerDialog(info);
            break;

        case ID_VIEW_BLOOM:
            ToggleBloomDialog(info);
            break;

        case ID_VIEW_LIGHTING:
            ToggleLightingDialog(info);
            break;

        case ID_VIEW_BLOOM_TOGGLE:
            if (info->engine != NULL && info->engine->IsBloomAvailable())
            {
                bool newState = !info->engine->GetBloom();
                info->engine->SetBloom(newState);
                WriteBloomEnabled(newState);
                SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_BLOOM_TOGGLE,
                            MAKELONG(newState ? TRUE : FALSE, 0));
                // Keep the dialog's "Enable bloom" checkbox in lockstep
                // so a user with the dialog open isn't confused by a
                // toolbar toggle. WM_USER re-seeds all bloom controls.
                if (info->hBloomDlg != NULL)
                {
                    SendMessage(info->hBloomDlg, WM_USER, 0, 0);
                }
                RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            break;

        case ID_SPAWNER_TRIGGER:
            // Ctrl+Space global hotkey. In Manual mode, fires a single
            // burst from the current path-anchor with the configured
            // velocity / jitter. In Auto mode, no-op (the schedule is
            // already running). The chord moved from Shift+Space in
            // so that Shift is reserved for "modify gesture"
            // semantics; Ctrl is the idiomatic "trigger discrete
            // action" modifier.
            if (info->spawner != NULL)
            {
                info->spawner->Trigger(info->particleSystem, info->engine);
            }
            break;

        case ID_VIEW_PAUSE_PREVIEW:
        {
            // Pause / resume the preview simulation. GetTimeF() is
            // frozen while paused; Engine::Render() continues so the
            // last frame stays drawn for inspection.
            bool newState = !IsPreviewPaused();
            SetPreviewPaused(newState);
            SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_PAUSE_PREVIEW,
                        MAKELONG(newState ? TRUE : FALSE, 0));
            // Step buttons are only meaningful while paused; grey them
            // out otherwise so the disabled state matches the menu.
            SendMessage(info->hToolbar, TB_SETSTATE, ID_VIEW_STEP_1_FRAME,
                        MAKELONG(newState ? TBSTATE_ENABLED : 0, 0));
            SendMessage(info->hToolbar, TB_SETSTATE, ID_VIEW_STEP_10_FRAMES,
                        MAKELONG(newState ? TBSTATE_ENABLED : 0, 0));
            break;
        }

        case ID_VIEW_STEP_1_FRAME:
            DoStepFrames(info, 1);
            break;

        case ID_VIEW_STEP_10_FRAMES:
            DoStepFrames(info, 10);
            break;

        case ID_VIEW_RESET_VIEW_SETTINGS:
            // Confirm — destructive: clears persisted background color,
            // ground-plane visibility, and the ChooseColor custom-color
            // palette. Camera is intentionally NOT included; it has its
            // own Reset Camera command above. Spawner config is
            // session-only and resets each launch; we still reset the
            // in-memory spawner state here for parity.
            if (MessageBox(info->hMainWnd,
                           L"Reset background color, ground plane visibility, ground texture, ground Z offset, skydome, bloom, lighting, and the color picker's custom colors to defaults?",
                           L"Reset View Settings",
                           MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                ResetViewSettings();
                //: wipe lighting keys + push defaults to engine +
                // re-seed the open Lighting dialog if any. Done before
                // the bloom block so the lighting visual snaps cleanly
                // along with everything else.
                ApplyLightingDefaults(info);
                if (info->engine != NULL)
                {
                    // Defaults match Engine's constructor (engine.cpp). Kept
                    // in sync by hand — there's only one default each, and
                    // they rarely change.
                    info->engine->SetBackground(RGB(0x14, 0x08, 0x34));
                    info->engine->SetGround(true);
                    info->engine->SetGroundZ(0.0f);
                    info->engine->SetGroundTexture(0);   //: dirt default
                    info->engine->SetBloom(false);
                    info->engine->SetBloomStrength(0.0f);
                    info->engine->SetBloomCutoff(0.90f);
                    info->engine->SetBloomSize(0.10f);
                    SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_SHOWGROUND,
                                MAKELONG(info->engine->GetGround(), 0));
                    //: Reset View Settings returns selection to
                    // slot 0 (dirt) but does NOT clear per-slot custom
                    // paths — those are "user data" not "view
                    // settings". A separate "Reset all slots" button
                    // in the picker dialog handles that.
                    RebuildGroundTexturePreviewBitmap(info);
                    InvalidateRect(info->hGroundTexturePreview, NULL, TRUE);
                    //: reset skydome to Off (slot 0). Background colour
                    // is reset above (SetBackground); the unified Background
                    // button repaints itself off engine state — no ColorButton
                    // path remains since the rework.
                    info->engine->SetSkydomeSlot(0);
                    WriteSkydomeIndex(0);
                    RebuildBackgroundPreviewBitmap(info);
                    InvalidateRect(info->hBackgroundBtn, NULL, TRUE);
                    // Reseed the picker if it's open.
                    if (info->hSkydomePicker != NULL && IsWindowVisible(info->hSkydomePicker))
                        SendMessage(info->hSkydomePicker, WM_USER, 0, 0);
                    {
                        SPINNER_INFO si;
                        si.Mask = SPIF_VALUE;
                        si.IsFloat = true;
                        si.f.Value = 0.0f;
                        Spinner_SetInfo(info->hGroundZSpinner, &si);
                    }
                    EnableWindow(info->hGroundZLabel,   TRUE);
                    EnableWindow(info->hGroundZSpinner, TRUE);
                    // If the bloom dialog is open, re-seed its controls
                    // from the freshly-reset engine values.
                    if (info->hBloomDlg != NULL)
                    {
                        SendMessage(info->hBloomDlg, WM_USER, 0, 0);
                    }
                    // Sync the bloom-toggle toolbar button.
                    SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_BLOOM_TOGGLE,
                                MAKELONG(FALSE, 0));
                    RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                }
                COLORREF zero[16] = {0};
                ColorButton_SetCustomColors(zero);
                if (info->spawner != NULL)
                {
                    info->spawner->SetConfig(SpawnerConfig());
                    if (info->hSpawnerDlg != NULL)
                    {
                        SpawnerDlg_LoadFromConfig(info->hSpawnerDlg, info->spawner->GetConfig());
                    }
                }
                memset(&info->spawnerWindowRect, 0, sizeof(info->spawnerWindowRect));
            }
            break;

        case ID_VIEW_RESETCAMERA:
            if (info->engine != NULL)
            {
                Engine::Camera camera =
                {
                    D3DXVECTOR3(0,-250,125),
                    D3DXVECTOR3(0,0,0),
                    D3DXVECTOR3(0,0,1)
                };
                info->engine->SetCamera(camera);
            }
            break;

        case ID_VIEW_RELOAD_SHADERS:
            if (info->engine != NULL)
            {
                if (info->engine->ReloadShaders())
                {
                    SendMessage(info->hStatusBar, SB_SETTEXT, 4, (LPARAM)L"Shaders reloaded");
                }
                else
                {
                    SendMessage(info->hStatusBar, SB_SETTEXT, 4,
                                (LPARAM)L"Shader reload failed — keeping previous shaders");
                }
                if (info->hRenderWnd != NULL) InvalidateRect(info->hRenderWnd, NULL, TRUE);
            }
            break;

        case ID_VIEW_RELOAD_TEXTURES:
            if (info->engine != NULL)
            {
                info->engine->ReloadTextures();
                SendMessage(info->hStatusBar, SB_SETTEXT, 4, (LPARAM)L"Textures reloaded");
                if (info->hRenderWnd != NULL) InvalidateRect(info->hRenderWnd, NULL, TRUE);
            }
            break;

        case ID_HELP_ABOUT:
            ShowAboutDialog(info->hMainWnd);
			break;
    }
	return true;
}

static void Render(APPLICATION_INFO* info)
{
    static FPSMeasurer measurer;

    // Drive the programmable spawner first so any new instances it
    // creates are visible to the engine update + render this frame.
    {
        TimeF now = GetTimeF();
        float dt = (g_spawnerLastFrameTime > 0.0f) ? (float)(now - g_spawnerLastFrameTime) : 0.0f;
        g_spawnerLastFrameTime = now;
        if (info->spawner != NULL)
        {
            info->spawner->Tick(dt, info->particleSystem, info->engine);
        }
    }

	// Update and Render!
	info->engine->Update();
    info->engine->Render();
    measurer.measure();

    const D3DXVECTOR3 cursor = info->mouseCursor.GetPosition();
    info->mouseCursor.UpdateVelocity();

    // Update status bar
    SendMessage(info->hStatusBar, SB_SETTEXT, 0, (LPARAM)LoadString(IDS_STATUS_INSTANCES, info->engine->GetNumInstances(), info->engine->GetNumEmitters()).c_str());
    SendMessage(info->hStatusBar, SB_SETTEXT, 1, (LPARAM)LoadString(IDS_STATUS_PARTICLES, info->engine->GetNumParticles()).c_str());
    {
        // Append " · PAUSED" so the pause state is glanceable without
        // adding a sixth status-bar pane.
        std::wstring fps = LoadString(IDS_STATUS_FPS, (int)measurer.getFPS());
        if (IsPreviewPaused()) fps += L" · PAUSED";
        SendMessage(info->hStatusBar, SB_SETTEXT, 2, (LPARAM)fps.c_str());
    }

    // If the spawner dialog is open, refresh its status counter.
    if (info->spawner != NULL && info->engine != NULL && info->hSpawnerDlg != NULL && info->spawnerVisible)
    {
        wchar_t buf[64];
        int active = info->engine->ActiveSpawnerInstanceCount();
        bool capped = active >= SpawnerDriver::MAX_ACTIVE_INSTANCES;
        swprintf_s(buf, 64,
                   capped ? L"Status: %d / %d active (limited)" : L"Status: %d / %d active",
                   active, SpawnerDriver::MAX_ACTIVE_INSTANCES);
        SetDlgItemText(info->hSpawnerDlg, IDC_SPAWNER_STATUS, buf);
    }
}

static LRESULT CALLBACK MainWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static int SIDEBAR_WIDTH = 310;

	APPLICATION_INFO* info = (APPLICATION_INFO*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	switch (uMsg)
	{
		case WM_CREATE:
		{
			CREATESTRUCT* pcs = (CREATESTRUCT*)lParam;
			info = (APPLICATION_INFO*)pcs->lpCreateParams;
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)info);

			HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

			//
			// Create the emitter list
			//
            if ((info->hEmitterList = CreateWindow(L"EmitterList", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
				4, 4, SIDEBAR_WIDTH, 200, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
            {
                return -1;
            }
			SendMessage(info->hEmitterList, WM_SETFONT, (WPARAM)hFont, FALSE);

			//
			// Create the property tab window
			//
			// Height bumped from 514→537 (+23 px) for — the Appearance
			// tab grew 14 du for the palette button row above the texture
			// fields. The WM_SIZE handler at line ~2518 resizes this to
			// the actual layout dynamically; the initial size matters
			// because GetClientRect on first paint reads it.
			if ((info->hPropertyTabs = CreateWindowEx(WS_EX_CONTROLPARENT, L"EmitterProps", NULL, WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE | TCS_FOCUSNEVER | WS_TABSTOP,
				4, 4, SIDEBAR_WIDTH, 537, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}
			SendMessage(info->hPropertyTabs, WM_SETFONT, (WPARAM)hFont, FALSE);

			//
			// Create the tool bar
			//
			if ((info->hToolbar = CreateWindow(TOOLBARCLASSNAME, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | CCS_NORESIZE | CCS_NODIVIDER | TBSTYLE_TOOLTIPS,
				0, 0, 0, 0, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}

			SendMessage(info->hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

			HBITMAP hBmpToolbar = LoadBitmap(pcs->hInstance, MAKEINTRESOURCE(IDR_TOOLBAR1));
			// 11 cells now: file new/open/save (0..2), ground/heat (3..4),
			// undo/redo (5..6), bloom (7), pause/step1/step10 (8..10).
			// See tasks/extend_toolbar1_bmp.ps1, _bloom.ps1, _pause.ps1,
			// and _step.ps1 for the bitmap-extension pattern.
			HIMAGELIST hImgList = ImageList_Create(16, 16, ILC_COLOR24 | ILC_MASK, 11, 0);
			ImageList_AddMasked(hImgList, hBmpToolbar, RGB(0,128,128));
			DeleteObject(hBmpToolbar);
			SendMessage(info->hToolbar, TB_SETIMAGELIST, 0, (LPARAM)hImgList);

			// Step buttons start disabled (pause is off at launch); the
			// pause WM_COMMAND handler toggles their TBSTATE_ENABLED bit
			// to mirror IsPreviewPaused().
			TBBUTTON buttons[] = {
				{0, 0, 0, BTNS_SEP},
				{0, ID_FILE_NEW,  TBSTATE_ENABLED, BTNS_BUTTON},
				{1, ID_FILE_OPEN, TBSTATE_ENABLED, BTNS_BUTTON},
				{2, ID_FILE_SAVE, TBSTATE_ENABLED, BTNS_BUTTON},
				{0, 0, 0, BTNS_SEP},
				{5, ID_EDIT_UNDO, 0,                BTNS_BUTTON},
				{6, ID_EDIT_REDO, 0,                BTNS_BUTTON},
				{0, 0, 0, BTNS_SEP},
				{3, ID_VIEW_SHOWGROUND,     TBSTATE_ENABLED | TBSTATE_CHECKED, BTNS_CHECK},
				{4, ID_VIEW_DEBUGHEAT,      TBSTATE_ENABLED,                   BTNS_CHECK},
				{7, ID_VIEW_BLOOM_TOGGLE,   TBSTATE_ENABLED,                   BTNS_CHECK},
				{8, ID_VIEW_PAUSE_PREVIEW,  TBSTATE_ENABLED,                   BTNS_CHECK},
				{9, ID_VIEW_STEP_1_FRAME,   0,                                 BTNS_BUTTON},
				{10,ID_VIEW_STEP_10_FRAMES, 0,                                 BTNS_BUTTON},
			};
			SendMessage(info->hToolbar, TB_ADDBUTTONS, 14, (LPARAM)&buttons);
			
			if ((info->hRebar = CreateWindow(REBARCLASSNAME, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
				0, 0, 0, 0, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}

			SIZE size;
			SendMessage(info->hToolbar, TB_GETMAXSIZE, 0, (LPARAM)&size);

			REBARBANDINFO rbbi;
			rbbi.cbSize     = sizeof(REBARBANDINFO);
			rbbi.fMask      = RBBIM_STYLE | RBBIM_CHILD | RBBIM_SIZE | RBBIM_CHILDSIZE;
			rbbi.fStyle     = RBBS_NOGRIPPER;
			rbbi.hwndChild  = info->hToolbar;
			rbbi.cxMinChild = size.cx;
			rbbi.cyMinChild = size.cy + 2;
			rbbi.cx         = rbbi.cxMinChild;
			SendMessage(info->hRebar, RB_INSERTBAND, -1, (LPARAM)&rbbi);

			//
			// Create the status bar
			//
			if ((info->hStatusBar = CreateWindow(STATUSCLASSNAME, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
				0, 0, 0, 0, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}

            INT widths[] = {140, 230, 280, 475, -1};
            SendMessage(info->hStatusBar, SB_SETPARTS, 5, (LPARAM)widths);
            SendMessage(info->hStatusBar, SB_SETTEXT, 4, (LPARAM)LoadString(IDS_STATUS_SHIFT_TO_SPAWN).c_str());

			//
			// Create the track tab window
			//
			if ((info->hTrackTabs = CreateWindowEx(WS_EX_CONTROLPARENT, WC_TABCONTROL, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | TCS_FOCUSNEVER, 4, 4, 300, 175, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}
			SendMessage(info->hTrackTabs, WM_SETFONT, (WPARAM)hFont, FALSE);

			const UINT trackLabels[N_TRACKS] = {
                IDS_LABEL_TRACK_RED, IDS_LABEL_TRACK_GREEN, IDS_LABEL_TRACK_BLUE, IDS_LABEL_TRACK_ALPHA,
                IDS_LABEL_TRACK_SCALE, IDS_LABEL_TRACK_INDEX, IDS_LABEL_TRACK_RPS};

			for (int i = 0; i < N_TRACKS; i++)
			{
                wstring label = LoadString(trackLabels[i]);

				TCITEM item;
				item.mask    = TCIF_TEXT;
				item.pszText = (LPWSTR)label.c_str();
				SendMessage(info->hTrackTabs, TCM_INSERTITEM, i, (LPARAM)&item);
			}

			//
			// Create the track editors
			//
			for (int i = 0; i < N_TRACKS; i++)
			{
				if ((info->hTrackEditors[i] = CreateWindowEx(WS_EX_CONTROLPARENT, L"TrackEditor", NULL, WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 100, 100, info->hTrackTabs, NULL, pcs->hInstance, (LPVOID)(LONG_PTR)i)) == NULL)
				{
					return -1;
				}
			}

            // Create the background control
			if ((info->hBackgroundLabel = CreateWindowEx(0, L"STATIC", LoadString(IDS_LABEL_BACKGROUND).c_str(), WS_CHILD | WS_VISIBLE,
				0, 0, 65, 16, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}
			SendMessage(info->hBackgroundLabel, WM_SETFONT, (WPARAM)hFont, FALSE);
			
			// rework: the Background button is the single entry point for
			// background state. Owner-draw paints either a flat colour swatch
			// (SkydomeIndex == 0) or the current skydome thumbnail (slots 1+).
			// Click opens the picker dialog. Class changes from ColorButton to
			// plain BUTTON so we own the paint path; ChooseColor is now reached
			// through slot 0 of the picker, not through this button directly.
			if ((info->hBackgroundBtn = CreateWindowEx(0, L"BUTTON", NULL,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				0, 0, 24, 24, hWnd, (HMENU)(UINT_PTR)ID_BACKGROUND_PREVIEW,
				pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}
			info->hBackgroundPreviewBitmap = NULL;
			// Bitmap built later, after engine init + registry load, by
			// RebuildBackgroundPreviewBitmap().

            // Create the "leave particles" check box
            if ((info->hLeaveParticles = CreateWindow(L"BUTTON", LoadString(IDS_LABEL_LEAVE_PARTICLES).c_str(), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                0, 0, 300, 16, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
            {
                return -1;
            }
            SendMessage(info->hLeaveParticles, WM_SETFONT, (WPARAM)hFont, FALSE);

            // Ground Z spinner + label. Final position is set in WM_SIZE
            // alongside the other header-strip controls.
            if ((info->hGroundZLabel = CreateWindowEx(0, L"STATIC", LoadString(IDS_LABEL_GROUND_Z).c_str(), WS_CHILD | WS_VISIBLE,
                0, 0, 90, 16, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
            {
                return -1;
            }
            SendMessage(info->hGroundZLabel, WM_SETFONT, (WPARAM)hFont, FALSE);

            // ground texture picker. Label + owner-drawn mini
            // preview button. Click opens the picker dialog
            // (IDD_GROUND_TEXTURE_PICKER) which shows all 12 slots
            // as a grid; selecting one closes the picker and updates
            // the engine + this preview.
            if ((info->hGroundTextureLabel = CreateWindowEx(0, L"STATIC",
                LoadString(IDS_LABEL_GROUND_TEXTURE).c_str(),
                WS_CHILD | WS_VISIBLE,
                0, 0, 80, 16, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
            {
                return -1;
            }
            SendMessage(info->hGroundTextureLabel, WM_SETFONT, (WPARAM)hFont, FALSE);

            if ((info->hGroundTexturePreview = CreateWindowEx(0, L"BUTTON", NULL,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 24, 24, hWnd, (HMENU)(UINT_PTR)ID_GROUND_TEXTURE_PREVIEW,
                pcs->hInstance, NULL)) == NULL)
            {
                return -1;
            }
            info->hGroundTexturePreviewBitmap = NULL;
            // Thumbnail is built later (after engine init + registry
            // load) by RebuildGroundTexturePreviewBitmap().

            if ((info->hGroundZSpinner = CreateWindowEx(WS_EX_CLIENTEDGE, L"Spinner", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 80, 20, hWnd, (HMENU)(UINT_PTR)ID_GROUNDZ_SPINNER, pcs->hInstance, NULL)) == NULL)
            {
                return -1;
            }
            SendMessage(info->hGroundZSpinner, WM_SETFONT, (WPARAM)hFont, FALSE);
            // Add 4 px right margin on the edit child so the digits don't
            // crowd the spinner chevrons. EM_SETMARGINS sets a text-inset;
            // ES_RIGHT then flushes the number to that inset edge instead
            // of all the way to the buttons. The first child of a Spinner
            // is its EDIT (see Spinner.cpp WM_CREATE).
            {
                HWND hEdit = GetWindow(info->hGroundZSpinner, GW_CHILD);
                if (hEdit != NULL)
                {
                    SendMessage(hEdit, EM_SETMARGINS, EC_RIGHTMARGIN, MAKELPARAM(0, 4));
                }
            }
            {
                SPINNER_INFO si;
                si.Mask        = SPIF_ALL;
                si.IsFloat     = true;
                si.f.Value     = 0.0f;
                si.f.MinValue  = -100.0f;
                si.f.MaxValue  =  100.0f;
                si.f.Increment = 0.1f;
                Spinner_SetInfo(info->hGroundZSpinner, &si);
            }

            // Multi-select overlay (). A WS_EX_LAYERED child window
            // with a solid black brush + 50% alpha gives a semi-
            // transparent dark veil over the inspector + curve
            // editor when 2+ emitters are selected. Created hidden;
            // shown / repositioned by SetEmitterInfo and WM_SIZE.
            //
            // Register a one-off WNDCLASS so we get a guaranteed
            // black-painted background. The STATIC + SS_BLACKRECT
            // combo doesn't reliably paint under WS_EX_LAYERED.
            {
                static bool s_overlayClassRegistered = false;
                if (!s_overlayClassRegistered)
                {
                    WNDCLASSEX wc = { sizeof(wc) };
                    wc.lpfnWndProc   = DefWindowProc;
                    wc.hInstance     = pcs->hInstance;
                    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
                    wc.lpszClassName = L"MultiSelOverlay";
                    RegisterClassEx(&wc);
                    s_overlayClassRegistered = true;
                }
            }
            // Multi-select overlay as a top-level popup, owned by the
            // main window (). A child-window overlay loses the
            // paint race against custom controls that schedule their
            // own WM_PAINT (Spinner, ColorButton, etc. inside the
            // EmitterProps tabs) — they repaint over the overlay
            // because their paint cycle is independent of the
            // sibling Z-order. Top-level layered windows are
            // composited by the DWM on top of any child controls of
            // any window underneath, so this is reliable.
            //
            // WS_EX_TOOLWINDOW: keeps it out of the taskbar / Alt-Tab.
            // WS_EX_NOACTIVATE: clicks on it don't steal focus from
            // the main window.
            info->hMultiSelectOverlay = CreateWindowEx(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
                              | WS_EX_TRANSPARENT,
                L"MultiSelOverlay", NULL,
                WS_POPUP,
                0, 0, 0, 0,
                hWnd, NULL, pcs->hInstance, NULL);
            if (info->hMultiSelectOverlay != NULL)
            {
                // Alpha 48/255 ≈ 19% — very subtle veil. Bump to
                // 64 (25%) / 96 (38%) / 128 (50%) for progressively
                // heavier dim.
                SetLayeredWindowAttributes(info->hMultiSelectOverlay,
                                            0, 48, LWA_ALPHA);
            }

			SetEmitterInfo(info);
            AppendHistory(info, hWnd);
			ShowWindow(info->hTrackEditors[0], SW_SHOW);
            SetFocus(info->hEmitterList);

			// Start the two autosave timers. They fire whether or not
			// a file is loaded; the WM_TIMER handler bails when the
			// model is unchanged or absent. See src/Autosave.h.
			SetTimer(hWnd, Autosave::RECENT_TIMER_ID, Autosave::RECENT_INTERVAL_MS, NULL);
			SetTimer(hWnd, Autosave::STABLE_TIMER_ID, Autosave::STABLE_INTERVAL_MS, NULL);
			break;
		}

        case WM_TIMER:
            // Two autosave tiers — see src/Autosave.h. Both gated on
            // particleSystem != NULL && info->changed so we don't
            // write identical bytes when nothing has changed since
            // the last tick. Write() is best-effort; IO errors are
            // swallowed so a disk-full / permission-denied condition
            // doesn't spam the user every 30 seconds.
            if (wParam == Autosave::RECENT_TIMER_ID || wParam == Autosave::STABLE_TIMER_ID)
            {
                if (info->particleSystem != NULL && info->changed)
                {
                    Autosave::Tier tier = (wParam == Autosave::RECENT_TIMER_ID)
                                        ? Autosave::Tier::Recent
                                        : Autosave::Tier::Stable;
                    Autosave::Write(*info->particleSystem, info->filename, tier);
                }
                return 0;
            }
            break;

        case WM_CLOSE:
            if (DoCloseFile(info))
            {
                DestroyWindow(hWnd);
            }
            return 0;

		case WM_DESTROY:
            // Clean shutdown — kill the autosave timers and remove
            // this session's recovery files. Any leftover here would
            // be misinterpreted as orphan-from-crash on next launch.
            KillTimer(hWnd, Autosave::RECENT_TIMER_ID);
            KillTimer(hWnd, Autosave::STABLE_TIMER_ID);
            Autosave::DeleteOurSession();
            if (info->engine != NULL)
            {
			    info->engine->Clear();
            }
			delete info->particleSystem;
			info->particleSystem = NULL;
			PostQuitMessage(0);
			break;

		case WM_INITMENU:
			DoMenuInit((HMENU)wParam, info);
			break;

		case WM_MEASUREITEM:
		{
			MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lParam;
			if (mis->CtlType != ODT_MENU) break;
			if (mis->itemID < ID_MOD_FIRST || mis->itemID > ID_MOD_LAST) break;

			size_t idx = (size_t)mis->itemData;
			if (!info->modManager) break;
			const auto& mods = info->modManager->GetMods();
			if (idx >= mods.size()) break;
			const ModEntry& m = mods[idx];

			EnsureMenuFonts(info);
			HDC hdc = GetDC(hWnd);
			HFONT hOld = (HFONT)SelectObject(hdc, info->hMenuFont);

			SIZE szFolder;
			GetTextExtentPoint32(hdc, m.folderName.c_str(), (int)m.folderName.size(), &szFolder);

			SIZE szNick = {0, 0};
			if (!m.nickname.empty())
			{
				SelectObject(hdc, info->hMenuItalicFont);
				wstring nick = L" (" + m.nickname + L")";
				GetTextExtentPoint32(hdc, nick.c_str(), (int)nick.size(), &szNick);
			}

			SelectObject(hdc, hOld);
			ReleaseDC(hWnd, hdc);

			int checkW = GetSystemMetrics(SM_CXMENUCHECK);
			int padding = 16;
			mis->itemWidth  = checkW + szFolder.cx + szNick.cx + padding;
			mis->itemHeight = max(GetSystemMetrics(SM_CYMENU), max(szFolder.cy, szNick.cy) + 4);
			return TRUE;
		}

		case WM_DRAWITEM:
		{
			DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
			// ground texture preview button. Owner-drawn so we
			// can stretch a 24×24 thumbnail bitmap into it with a
			// 1 px border and focus/highlight feedback. The bitmap
			// is generated by RebuildGroundTexturePreviewBitmap()
			// whenever the active slot changes.
			if (dis->CtlType == ODT_BUTTON &&
			    dis->CtlID   == ID_GROUND_TEXTURE_PREVIEW &&
			    info != NULL)
			{
				RECT rc = dis->rcItem;
				HBITMAP hbm = info->hGroundTexturePreviewBitmap;
				if (hbm != NULL)
				{
					HDC hMem = CreateCompatibleDC(dis->hDC);
					HGDIOBJ old = SelectObject(hMem, hbm);
					BITMAP bm = {};
					GetObject(hbm, sizeof(bm), &bm);
					int w = rc.right - rc.left;
					int h = rc.bottom - rc.top;
					SetStretchBltMode(dis->hDC, HALFTONE);
					StretchBlt(dis->hDC, rc.left, rc.top, w, h,
					           hMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
					SelectObject(hMem, old);
					DeleteDC(hMem);
				}
				else
				{
					FillRect(dis->hDC, &rc,
					         (HBRUSH)(COLOR_BTNFACE + 1));
				}
				// Border + focus / pressed indication.
				HPEN pen = CreatePen(PS_SOLID, 1,
				    (dis->itemState & ODS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHT)
				                                    : RGB(0x60, 0x60, 0x60));
				HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
				HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
				Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
				SelectObject(dis->hDC, oldBrush);
				SelectObject(dis->hDC, oldPen);
				DeleteObject(pen);
				if (dis->itemState & ODS_FOCUS)
				{
					RECT fr = rc;
					InflateRect(&fr, -2, -2);
					DrawFocusRect(dis->hDC, &fr);
				}
				return TRUE;
			}
			// rework: unified Background button. Paints a flat colour
			// swatch when no skydome is active, or the current skydome
			// thumbnail otherwise. RebuildBackgroundPreviewBitmap keeps the
			// bitmap NULL in solid-colour mode so a stale thumbnail can't
			// shadow the swatch path.
			if (dis->CtlType == ODT_BUTTON &&
			    dis->CtlID   == ID_BACKGROUND_PREVIEW &&
			    info != NULL && info->engine != NULL)
			{
				RECT rc = dis->rcItem;
				const bool useSkydome =
				    info->engine->GetSkydomeSlot() != Engine::kSkydomeOffSlot;
				HBITMAP hbm = useSkydome ? info->hBackgroundPreviewBitmap : NULL;
				if (hbm != NULL)
				{
					HDC hMem = CreateCompatibleDC(dis->hDC);
					HGDIOBJ old = SelectObject(hMem, hbm);
					BITMAP bm = {};
					GetObject(hbm, sizeof(bm), &bm);
					int w = rc.right - rc.left;
					int h = rc.bottom - rc.top;
					SetStretchBltMode(dis->hDC, HALFTONE);
					StretchBlt(dis->hDC, rc.left, rc.top, w, h,
					           hMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
					SelectObject(hMem, old);
					DeleteDC(hMem);
				}
				else
				{
					HBRUSH brSwatch = CreateSolidBrush(info->engine->GetBackground());
					FillRect(dis->hDC, &rc, brSwatch);
					DeleteObject(brSwatch);
				}
				// Border + focus / pressed indication.
				HPEN penSky = CreatePen(PS_SOLID, 1,
				    (dis->itemState & ODS_SELECTED) ? GetSysColor(COLOR_HIGHLIGHT)
				                                    : RGB(0x60, 0x60, 0x60));
				HGDIOBJ oldPenSky   = SelectObject(dis->hDC, penSky);
				HGDIOBJ oldBrushSky = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
				Rectangle(dis->hDC, rc.left, rc.top, rc.right, rc.bottom);
				SelectObject(dis->hDC, oldBrushSky);
				SelectObject(dis->hDC, oldPenSky);
				DeleteObject(penSky);
				if (dis->itemState & ODS_FOCUS)
				{
					RECT fr = rc;
					InflateRect(&fr, -2, -2);
					DrawFocusRect(dis->hDC, &fr);
				}
				return TRUE;
			}
			if (dis->CtlType != ODT_MENU) break;
			if (dis->itemID < ID_MOD_FIRST || dis->itemID > ID_MOD_LAST) break;

			size_t idx = (size_t)dis->itemData;
			if (!info->modManager) break;
			const auto& mods = info->modManager->GetMods();
			if (idx >= mods.size()) break;
			const ModEntry& m = mods[idx];

			EnsureMenuFonts(info);

			bool selected = (dis->itemState & ODS_SELECTED) != 0;
			bool checked  = (dis->itemState & ODS_CHECKED)  != 0;
			bool grayed   = (dis->itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;

			// Background
			FillRect(dis->hDC, &dis->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));
			SetBkMode(dis->hDC, TRANSPARENT);
			COLORREF textColor = grayed   ? GetSysColor(COLOR_GRAYTEXT)
			                   : selected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
			                              : GetSysColor(COLOR_MENUTEXT);
			SetTextColor(dis->hDC, textColor);

			int checkW = GetSystemMetrics(SM_CXMENUCHECK);

			// Checkmark on the left if this is the selected mod
			if (checked)
			{
				RECT cr = { dis->rcItem.left, dis->rcItem.top,
				            dis->rcItem.left + checkW, dis->rcItem.bottom };
				DrawFrameControl(dis->hDC, &cr, DFC_MENU, DFCS_MENUCHECK);
			}

			// Folder name in regular weight
			HFONT hOld = (HFONT)SelectObject(dis->hDC, info->hMenuFont);
			RECT tr = { dis->rcItem.left + checkW, dis->rcItem.top,
			            dis->rcItem.right, dis->rcItem.bottom };
			DrawText(dis->hDC, m.folderName.c_str(), (int)m.folderName.size(), &tr,
			         DT_LEFT | DT_VCENTER | DT_SINGLELINE);

			// "(Nickname)" in italics, if present
			if (!m.nickname.empty())
			{
				SIZE szFolder;
				GetTextExtentPoint32(dis->hDC, m.folderName.c_str(), (int)m.folderName.size(), &szFolder);
				tr.left += szFolder.cx;

				SelectObject(dis->hDC, info->hMenuItalicFont);
				wstring nick = L" (" + m.nickname + L")";
				DrawText(dis->hDC, nick.c_str(), (int)nick.size(), &tr,
				         DT_LEFT | DT_VCENTER | DT_SINGLELINE);
			}

			SelectObject(dis->hDC, hOld);
			return TRUE;
		}

		case WM_MENURBUTTONUP:
		{
			// Right-click on a menu item. We can't show a modal dialog from
			// here — the menu's modal tracking loop is still running and a
			// dialog would either fail to appear or briefly flicker before
			// being dismissed alongside the menu. Stash the ID, dismiss the
			// menu, and queue ourselves a deferred message to show the
			// dialog after the menu finishes closing.
			HMENU hMenu = (HMENU)lParam;
			UINT  pos   = (UINT)wParam;
			UINT  id    = GetMenuItemID(hMenu, pos);
			if (FindModById(info, id) != NULL)
			{
				EndMenu();
				PostMessage(hWnd, WM_APP_SHOW_NICKNAME, id, 0);
			}
			return 0;
		}

		case WM_APP_SHOW_NICKNAME:
		{
			ModEntry* m = FindModById(info, (UINT)wParam);
			if (m != NULL)
			{
				wstring nickname;
				if (ShowNicknameDialog(hWnd, *m, nickname))
				{
					m->nickname = nickname;
					WriteModNickname(m->path, nickname);
					// Sort order is by folder name and doesn't change, but
					// rebuild so the new (nickname) label is rendered.
					RebuildModsMenu(info);
				}
			}
			return 0;
		}

		case WM_COMMAND:
			if (info != NULL)
			{
				// Menu and control notifications
				WORD code     = HIWORD(wParam);
				WORD id       = LOWORD(wParam);
				HWND hControl = (HWND)lParam;

                if (hControl == NULL)
                {
				    // Menu or accelerator
                    if (id >= ID_FILE_HISTORY_0 && id < ID_FILE_HISTORY_0 + min(9,NUM_HISTORY_ITEMS))
		            {
			            // It's a history item
                        if (DoCheckChanges(info))
                        {
			                OpenHistoryFile(info, id - ID_FILE_HISTORY_0);
                        }
		            }
                    else if (id == ID_MOD_NONE)
                    {
                        SelectMod(info, L"");
                    }
                    else if (id == ID_MOD_REFRESH)
                    {
                        if (info->modManager) info->modManager->DiscoverMods();
                        // If our currently-selected mod no longer exists, fall back to Unmodded
                        const std::wstring& sel = info->modManager ? info->modManager->GetSelectedModPath() : std::wstring();
                        bool stillExists = sel.empty();
                        if (info->modManager)
                        {
                            for (const ModEntry& m : info->modManager->GetMods())
                            {
                                if (_wcsicmp(m.path.c_str(), sel.c_str()) == 0)
                                {
                                    stillExists = true;
                                    break;
                                }
                            }
                        }
                        if (!stillExists) SelectMod(info, L"");
                        else RebuildModsMenu(info);
                    }
                    else if (id >= ID_MOD_FIRST && id <= ID_MOD_LAST)
                    {
                        ModEntry* m = FindModById(info, id);
                        if (m != NULL) SelectMod(info, m->path);
                    }
    		        else DoMenuItem(info, id);
                }
                else if (code == SN_CHANGE)
                {
                    if (hControl == info->hGroundZSpinner && info->engine != NULL)
                    {
                        float z = GetUIFloat(hWnd, ID_GROUNDZ_SPINNER);
                        info->engine->SetGroundZ(z);
                        // No WriteGroundZ — Ground Z is session-only; see the
                        // startup-init comment for the rationale.
                        RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                    }
                }
                else if (code == BN_CLICKED)
                {
                    //: user clicked the toolbar texture-preview button.
                    // Open the picker dialog; on commit, swap engine state,
                    // persist new selection + slot paths, refresh preview.
                    if (hControl == info->hGroundTexturePreview && info->engine != NULL)
                    {
                        ShowGroundTexturePicker(hWnd, info);
                        break;
                    }
                    // rework: unified Background button. Click opens the
                    // picker (slot 0 = solid colour, slots 1+ = skydomes). The
                    // picker is sticky — clicking a slot commits but leaves
                    // the dialog visible for further browsing.
                    if (hControl == info->hBackgroundBtn && info->engine != NULL)
                    {
                        ShowSkydomePicker(hWnd, info);
                        break;
                    }
                    if (hControl == info->hLeaveParticles && info->particleSystem != NULL)
                    {
                        info->particleSystem->setLeaveParticles(SendMessage(hControl, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        // Single-bool toggle: never coalesce — a "click,
                        // think, click again" sequence shouldn't collapse.
                        CaptureUndo(info, UndoStack::MakeCoalesceKey(0xFFFE, 0));
                        SetFileChanged(info, true);
                    }
                    else if (hControl == info->hToolbar)
                    {
                        DoMenuItem(info, id);
                    }
                }
             }
			break;

		case WM_NOTIFY:
		{
			NMHDR* hdr = (NMHDR*)lParam;
			switch (hdr->code)
			{
				case TTN_GETDISPINFO:
				{
					// Toolbar wants tooltips
					NMTTDISPINFO* nmdi = (NMTTDISPINFO*)hdr;
					static struct
                    {
                        UINT_PTR idFrom;
                        UINT     idStr;
                    }
                    tooltips[] =
					{
                        {ID_FILE_NEW,        IDS_TOOLTIP_FILE_NEW},
                        {ID_FILE_OPEN,       IDS_TOOLTIP_FILE_OPEN},
                        {ID_FILE_SAVE,       IDS_TOOLTIP_FILE_SAVE},
                        {ID_EDIT_UNDO,       IDS_TOOLTIP_EDIT_UNDO},
                        {ID_EDIT_REDO,       IDS_TOOLTIP_EDIT_REDO},
                        {ID_VIEW_SHOWGROUND, IDS_TOOLTIP_TOGGLE_GROUND},
                        {ID_VIEW_DEBUGHEAT,  IDS_TOOLTIP_DEBUG_HEAT},
                        {ID_VIEW_BLOOM_TOGGLE, IDS_TOOLTIP_TOGGLE_BLOOM},
                        {0}
					};

                    for (int i = 0; tooltips[i].idFrom != 0; i++)
                    {
                        if (tooltips[i].idFrom == hdr->idFrom)
                        {
                            nmdi->hinst    = (HINSTANCE)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_HINSTANCE);
					        nmdi->lpszText = MAKEINTRESOURCE(tooltips[i].idStr);
                            break;
                        }
                    }

                    break;
				}

				case TCN_SELCHANGING:
					ShowWindow(info->hTrackEditors[TabCtrl_GetCurSel(hdr->hwndFrom)], SW_HIDE);
					break;

				case TCN_SELCHANGE:
					ShowWindow(info->hTrackEditors[TabCtrl_GetCurSel(hdr->hwndFrom)], SW_SHOW);
					break;

				case ELN_LISTCHANGED:
					SetFileChanged(info, true);
					// Structural ops (add / delete / duplicate / move /
					// rename / paste / rescale) — coalesceKey 0 = never
					// fold into the previous entry.
					CaptureUndo(info, 0);
					break;

				case ELN_SELCHANGED:
                    info->selectedEmitter = EmitterList_GetSelection(info->hEmitterList);
					SetEmitterInfo(info);
					break;

				case EP_CHANGE:
                    TrackEditor_EnableTrack(info->hTrackEditors[ParticleSystem::TRACK_ROTATION_SPEED], !info->selectedEmitter->randomRotation);
                    EmitterList_SelectionChanged(info->hEmitterList);
                    SetFileChanged(info, true);
                    if (info->engine != NULL)
                    {
                        info->engine->OnParticleSystemChanged(-1);
                    }
                    // Coalesce repeated EP_CHANGE on the same emitter
                    // within the time window (e.g. spinner being held).
                    {
                        size_t selIdx = IndexOfEmitter(info->particleSystem, info->selectedEmitter);
                        WORD discriminator = (selIdx < 0xFFFF) ? (WORD)selIdx : (WORD)0xFFFF;
                        CaptureUndo(info, UndoStack::MakeCoalesceKey(EP_CHANGE, discriminator));
                    }
					break;

				case TE_CHANGE:
					// A track has changed; update the affected tracks
                    if (info->engine != NULL)
                    {
                        NMTECHANGE* nmtec = (NMTECHANGE*)lParam;
					    for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
					    {
                            if (i == nmtec->track || info->selectedEmitter->tracks[i] == &info->selectedEmitter->trackContents[nmtec->track])
						    {
							    info->engine->OnParticleSystemChanged(i);
						    }
					    }
                    }
                    SetFileChanged(info, true);
                    // Coalesce per (emitter, track). Drag-edits in the
                    // CurveEditor fire many TE_CHANGEs; the window
                    // collapses them into one undo step.
                    {
                        NMTECHANGE* nmtec = (NMTECHANGE*)lParam;
                        size_t selIdx = IndexOfEmitter(info->particleSystem, info->selectedEmitter);
                        // Pack track index (low 4 bits) and emitter index
                        // (next 12 bits) into a 16-bit discriminator. With
                        // NUM_TRACKS == 7 we have plenty of room for track,
                        // and 12 bits = 4096 emitters is far past any real
                        // particle system.
                        WORD eIdx = (selIdx < 0x0FFF) ? (WORD)selIdx : (WORD)0x0FFF;
                        WORD discriminator = (WORD)((eIdx << 4) | (nmtec->track & 0x0F));
                        CaptureUndo(info, UndoStack::MakeCoalesceKey(TE_CHANGE, discriminator));
                    }
					break;
			}
			break;
		}

        case WM_SIZING:
		{
			RECT* size = (RECT*)lParam;
			if (size->right - size->left < MIN_WINDOW_WIDTH)
			{
				if (wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT)
					size->left = size->right - MIN_WINDOW_WIDTH;
				else
					size->right = size->left + MIN_WINDOW_WIDTH;
			}
			if (size->bottom - size->top < MIN_WINDOW_HEIGHT)
			{
				if (wParam == WMSZ_BOTTOM || wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_BOTTOMRIGHT)
					size->bottom = size->top + MIN_WINDOW_HEIGHT;
				else
					size->top = size->bottom - MIN_WINDOW_HEIGHT;
			}
			break;
		}

		case WM_SIZE:
		{
			info->isMinimized = (wParam == SIZE_MINIMIZED);
			if (!info->isMinimized)
			{
				RECT props, tabs, status;

				// Get toolbar height
				GetWindowRect(info->hRebar, &props);
				int top = props.bottom - props.top;

				// Move status bar and recalculate height
				MoveWindow(info->hStatusBar, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
				GetClientRect(info->hStatusBar, &status);
				lParam = MAKELONG(LOWORD(lParam), HIWORD(lParam) - status.bottom - top);

				GetClientRect(info->hPropertyTabs, &props);
				GetClientRect(info->hTrackTabs,    &tabs);

				// Move property
				MoveWindow(info->hEmitterList,  4, top + 4, props.right, HIWORD(lParam) - props.bottom - 8, TRUE);
				MoveWindow(info->hPropertyTabs, 4, top + HIWORD(lParam) - props.bottom, props.right, props.bottom, TRUE);

                // Move top bar
                RECT checkbox;
                RECT label;
                RECT groundLabel;
                RECT groundTexLabel;
                GetClientRect(info->hLeaveParticles, &checkbox);
                GetClientRect(info->hBackgroundLabel, &label);
                GetClientRect(info->hGroundZLabel, &groundLabel);
                GetClientRect(info->hGroundTextureLabel, &groundTexLabel);
                int height = max(max(24, checkbox.bottom), label.bottom);
                const int GROUND_SPINNER_W = 80;
                const int GROUND_SPINNER_H = 20;
                const int GROUND_COMBO_W   = 80;
                const int GROUND_COMBO_H   = 20;
                MoveWindow(info->hLeaveParticles, props.right + 8, top + 4 + (height - checkbox.bottom) / 2, checkbox.right, label.bottom, TRUE);
                // Right side of the header strip, anchored from the right edge:
                //   [Ground tex label] [Ground tex preview]  [Ground Z label] [Ground Z spinner]  [Background label] [Background btn]
                // Background button is 4 px from the right edge; the
                // Ground Z group sits 16 px to its left; the Ground
                // texture group sits 16 px to the left of that. Same
                // anchoring pattern; narrow-window overflow degrades
                // gracefully (spinner widths are fixed; labels truncate
                // via Win32 default).
                //
                // rework: removed the standalone skydome preview slot.
                // Skydome selection is now reached through slot 0+ of the
                // picker opened by the Background button.
                const int GT_PREVIEW_SIZE  = 24;
                int bgLabelX    = LOWORD(lParam) - 32 - label.right;
                int gzSpinnerX  = bgLabelX - 16 - GROUND_SPINNER_W;
                int gzLabelX    = gzSpinnerX - 4 - groundLabel.right;
                int gtPreviewX  = gzLabelX - 16 - GT_PREVIEW_SIZE;
                int gtLabelX    = gtPreviewX - 4 - groundTexLabel.right;
				MoveWindow(info->hBackgroundBtn,   LOWORD(lParam) - 28, top + 4 + (height - 24) / 2, 24, 24, TRUE);
				MoveWindow(info->hBackgroundLabel, bgLabelX, top + 4 + (height - label.bottom) / 2, label.right, label.bottom, TRUE);
                MoveWindow(info->hGroundZSpinner,  gzSpinnerX, top + 4 + (height - GROUND_SPINNER_H) / 2, GROUND_SPINNER_W,  GROUND_SPINNER_H,   TRUE);
                MoveWindow(info->hGroundZLabel,    gzLabelX,   top + 4 + (height - groundLabel.bottom) / 2, groundLabel.right, groundLabel.bottom, TRUE);
                MoveWindow(info->hGroundTexturePreview, gtPreviewX, top + 4 + (height - GT_PREVIEW_SIZE) / 2, GT_PREVIEW_SIZE, GT_PREVIEW_SIZE, TRUE);
                MoveWindow(info->hGroundTextureLabel,   gtLabelX,   top + 4 + (height - groundTexLabel.bottom) / 2, groundTexLabel.right, groundTexLabel.bottom, TRUE);

				// Move render window
				MoveWindow(info->hRenderWnd, props.right + 8, top + 32, LOWORD(lParam) - (props.right + 8), HIWORD(lParam) - tabs.bottom - 36, TRUE);

				// Move track tabs
				tabs.right = LOWORD(lParam) - (props.right + 8);
				MoveWindow(info->hTrackTabs, props.right + 8, top + HIWORD(lParam) - tabs.bottom, tabs.right, tabs.bottom, TRUE);
				TabCtrl_AdjustRect(info->hTrackTabs, FALSE, &tabs);
				for (int i = 0; i < N_TRACKS; i++)
				{
					MoveWindow(info->hTrackEditors[i], tabs.left, tabs.top, tabs.right - tabs.left, tabs.bottom - tabs.top, TRUE);
				}

                // Multi-select overlay (). Top-level layered popup
                // sized to the bounding rect of the property tabs +
                // track tabs, then shaped with SetWindowRgn so it
                // ONLY covers those two panels — the gap between
                // them (where the 3D viewport lives) stays clear.
                if (info->hMultiSelectOverlay != NULL)
                {
                    RECT pr, tr;
                    GetWindowRect(info->hPropertyTabs, &pr);
                    GetWindowRect(info->hTrackTabs,    &tr);
                    int x = pr.left;
                    int y = (pr.top < tr.top) ? pr.top : tr.top;
                    int w = tr.right - pr.left;
                    int h = ((pr.bottom > tr.bottom) ? pr.bottom : tr.bottom) - y;
                    SetWindowPos(info->hMultiSelectOverlay, HWND_TOP,
                                  x, y, w, h,
                                  SWP_NOACTIVATE);

                    // Shape the overlay as the union of the two panel
                    // rects (in overlay-local coords). Viewport gap
                    // between them is excluded from the region, so
                    // the overlay window doesn't paint there at all.
                    HRGN rgnProps  = CreateRectRgn(pr.left - x, pr.top - y,
                                                    pr.right - x, pr.bottom - y);
                    HRGN rgnTracks = CreateRectRgn(tr.left - x, tr.top - y,
                                                    tr.right - x, tr.bottom - y);
                    CombineRgn(rgnProps, rgnProps, rgnTracks, RGN_OR);
                    DeleteObject(rgnTracks);
                    // SetWindowRgn takes ownership of rgnProps.
                    SetWindowRgn(info->hMultiSelectOverlay, rgnProps, TRUE);
                }
			}
            else if (info->hMultiSelectOverlay != NULL)
            {
                // Main window minimised — hide overlay too.
                ShowWindow(info->hMultiSelectOverlay, SW_HIDE);
            }
			return 0;
        }

        case WM_MOVE:
            // Top-level overlay sits in screen coords, so when the
            // main window moves we have to follow. Recompute from
            // the (now-moved) property/track tab positions.
            if (info != NULL && info->hMultiSelectOverlay != NULL
                && IsWindowVisible(info->hMultiSelectOverlay))
            {
                RECT pr, tr;
                GetWindowRect(info->hPropertyTabs, &pr);
                GetWindowRect(info->hTrackTabs,    &tr);
                int x = pr.left;
                int y = (pr.top < tr.top) ? pr.top : tr.top;
                int w = tr.right - pr.left;
                int h = ((pr.bottom > tr.bottom) ? pr.bottom : tr.bottom) - y;
                SetWindowPos(info->hMultiSelectOverlay, HWND_TOP,
                              x, y, w, h,
                              SWP_NOACTIVATE);
            }
            break;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// GetCursorPos3D moved to src/MouseCursor.h (included near top of file).
// Legacy callers below (WM_KEYDOWN VK_SHIFT, WM_MOUSEMOVE) resolve to the
// header'd inline.

static LRESULT CALLBACK RenderWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	APPLICATION_INFO* info = (APPLICATION_INFO*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	switch (uMsg)
	{
		case WM_CREATE:
		{
			CREATESTRUCT* pcs = (CREATESTRUCT*)lParam;
			info = (APPLICATION_INFO*)pcs->lpCreateParams;
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)info);
			break;
		}

		case WM_PAINT:
		{
			// F12: pair Render with BeginPaint/EndPaint so the
			// update region is validated. Pre-fix the case just called
			// Render(info), leaving the update region marked dirty so
			// Windows kept re-posting WM_PAINT (paint storm + spurious CPU).
			PAINTSTRUCT ps;
			BeginPaint(hWnd, &ps);
			Render(info);
			EndPaint(hWnd, &ps);
			return 0;
		}

		case WM_LBUTTONUP:
			if (info->attachedParticleSystem != NULL)
			{
				// We've placed the system here
				info->engine->DetachParticleSystem(info->attachedParticleSystem);
				info->attachedParticleSystem = NULL;
			}

        case WM_RBUTTONUP:
			// Stop dragging
			info->dragmode = APPLICATION_INFO::NONE;
			ReleaseCapture();
			break;

		case WM_LBUTTONDOWN:
			if (info->attachedParticleSystem != NULL)
    		{
                info->dragmode = APPLICATION_INFO::OBJECT_Z;
                info->dragStartPosition = info->attachedParticleSystem->GetRelativePosition();
            }
            else info->dragmode = (wParam & MK_CONTROL) ? APPLICATION_INFO::ZOOM : APPLICATION_INFO::MOVE;

		case WM_RBUTTONDOWN:
            if (uMsg == WM_RBUTTONDOWN)
            {
    			info->dragmode = (wParam & MK_CONTROL) ? APPLICATION_INFO::ZOOM : APPLICATION_INFO::ROTATE;
            }

            // Start dragging, remember start settings
			info->startCam = info->engine->GetCamera();
			info->xstart   = LOWORD(lParam);
			info->ystart   = HIWORD(lParam);
			SetCapture(hWnd);
			SetFocus(hWnd);
			break;

		case WM_KEYUP:
			if (wParam == VK_SHIFT && info->attachedParticleSystem != NULL)
			{
				// Shift released. Remove cursor-bound system
				info->engine->KillParticleSystem(info->attachedParticleSystem);
				info->attachedParticleSystem = NULL;
			}
			break;

		case WM_KEYDOWN:
			// Only react to Shift being pressed initially
			if (wParam == VK_SHIFT && (~lParam & 0x40000000) && info->particleSystem != NULL && info->attachedParticleSystem == NULL)
			{
				// Spawn cursor-bound particle system
				D3DXVECTOR3 position;
				GetCursorPos3D(info->engine, (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam), position);
				info->attachedParticleSystem = info->engine->SpawnParticleSystem(*info->particleSystem, &info->mouseCursor);

                // Clear statusbar hint
                SendMessage(info->hStatusBar, SB_SETTEXT, 4, (LPARAM)L"");
			}
			break;

		case WM_MOUSEMOVE:
        {
            D3DXVECTOR3 cursor;
            if (info->dragmode == APPLICATION_INFO::OBJECT_Z)
			{
                // Move the attached object up or down
				long  y   = (short)HIWORD(lParam) - info->ystart;
                float len = D3DXVec3Length(&(info->startCam.Target - info->startCam.Position));
                cursor = info->mouseCursor.GetPosition();
                cursor.z = -y * len / 1000;
                info->mouseCursor.SetPosition(cursor);
        	    Render(info);
            }
            else
            {
				// Move cursor-bound particle system
				GetCursorPos3D(info->engine, (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam), cursor);
				info->mouseCursor.SetPosition(cursor);

                if (info->dragmode != APPLICATION_INFO::NONE)
			    {
				    // Yay, math time!
				    long x = (short)LOWORD(lParam) - info->xstart;
				    long y = (short)HIWORD(lParam) - info->ystart;

				    Engine::Camera camera = info->startCam;
				    D3DXVECTOR3    orthVec, diff = info->startCam.Position - info->startCam.Target;
    				
				    // Get the orthogonal vector
				    D3DXVec3Cross( &orthVec, &diff, &camera.Up );
				    D3DXVec3Normalize( &orthVec, &orthVec );

				    if (info->dragmode == APPLICATION_INFO::ROTATE)
				    {
					    // Lets rotate
					    D3DXMATRIX rotateXY, rotateZ, rotate;
					    D3DXMatrixRotationZ( &rotateZ, -D3DXToRadian(x / 2.0f) );
					    D3DXMatrixRotationAxis( &rotateXY, &orthVec, D3DXToRadian(y / 2.0f) );
					    D3DXMatrixMultiply( &rotate, &rotateXY, &rotateZ );
					    D3DXVec3TransformCoord( &camera.Position, &diff, &rotate );
					    camera.Position += camera.Target;
				    }
				    else if (info->dragmode == APPLICATION_INFO::MOVE)
				    {
					    // Lets translate
					    D3DXVECTOR3 Up;
					    D3DXVec3Cross( &Up, &orthVec, &diff );
					    D3DXVec3Normalize( &Up, &Up );
    					
					    // The distance we move depends on the distance from the object
					    // Large distance: move a lot, small distance: move a little
					    float multiplier = D3DXVec3Length( &diff ) / 1000;

					    camera.Target  += (float)x * multiplier * orthVec;
					    camera.Target  += (float)y * multiplier * Up;
					    camera.Position = diff + camera.Target;
				    }
				    else if (info->dragmode == APPLICATION_INFO::ZOOM)
				    {
					    // Lets zoom
					    // The amount we scroll in and out depends on the distance.
					    float olddist = D3DXVec3Length( &diff );
					    float newdist = max(1.0f, olddist - sqrt(olddist) * -y);
					    D3DXVec3Scale( &camera.Position, &diff, newdist / olddist );
					    camera.Position += camera.Target;
				    }
				    info->dragged = true;
				    info->engine->SetCamera( camera );
    			    Render(info);
			    }
            }

            // Update statusbar
            SendMessage(info->hStatusBar, SB_SETTEXT, 3, (LPARAM)LoadString(IDS_STATUS_MOUSE, cursor.x, cursor.y, cursor.z).c_str());
            break;
        }

		case WM_MOUSEWHEEL:
			if (info->dragmode == APPLICATION_INFO::NONE)
			{
				Engine::Camera camera = info->engine->GetCamera();

				// The amount we scroll in and out depends on the distance.
				D3DXVECTOR3 diff = camera.Position - camera.Target;
				float olddist = D3DXVec3Length( &diff );
				float newdist = max(1.0f, olddist - sqrt(olddist) * (SHORT)HIWORD(wParam) / WHEEL_DELTA);
				D3DXVec3Scale( &camera.Position, &diff, newdist / olddist );
				camera.Position += camera.Target;

				info->engine->SetCamera(camera);
				Render(info);
			}
			break;

		case WM_SIZE:
			if (info->engine != NULL)
			{
				info->engine->Reset();
			}
			break;

		case WM_SETFOCUS:
			// Yes, we want focus please
			return 0;

        case WM_DROPFILES:
		{
			// User dropped a filename on the window
			HDROP hDrop = (HDROP)wParam;
			UINT nFiles = DragQueryFile(hDrop, -1, NULL, 0);
			for (UINT i = 0; i < nFiles; i++)
			{
				UINT size = DragQueryFile(hDrop, i, NULL, 0);
				wstring filename(size,L' ');
				DragQueryFile(hDrop, i, (LPTSTR)filename.c_str(), size + 1);
				if (LoadFile(info, filename))
                {
                    break;
                }
			}
			DragFinish(hDrop);
			break;
		}
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// Returns the install path by querying the registry, or an empty string when failed.
static void getGamePath_Reg(vector<wstring>& strings)
{
	const TCHAR* paths[] = {
		L"Software\\LucasArts\\Star Wars Empire at War Forces of Corruption\\1.0",
		L"Software\\LucasArts\\Star Wars Empire at War Forces of Corruption Demo\\1.0",
		L"Software\\LucasArts\\Star Wars Empire at War\\1.0",
		L""
	};

	for (int i = 0; paths[i][0] != '\0'; i++)
	{
		HKEY hKey;
		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, paths[i], 0, KEY_QUERY_VALUE, &hKey ) == ERROR_SUCCESS)
		{
			DWORD type, size = MAX_PATH;
			TCHAR path[MAX_PATH];
			if (RegQueryValueEx(hKey, L"ExePath", NULL, &type, (LPBYTE)path, &size) == ERROR_SUCCESS)
			{
				wstring str = path;
				size_t pos = str.find_last_of(L"\\");
				if (pos != string::npos)
				{
					str = str.substr(0, pos);
				}
				strings.push_back(str);
			}
			RegCloseKey(hKey);
		}
	}
}

// Adds the %PROGRAMFILES%\LucasArts\Star Wars Empire at War\GameData paths to the vector
static void getGamePath_Shell(vector<wstring>& strings)
{
	TCHAR path[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_PROGRAM_FILES, NULL, SHGFP_TYPE_CURRENT, path )))
	{
		MessageBox(NULL, path, NULL, MB_OK );
		wstring str = path;
		if (*str.rbegin() != L'\\') str += L'\\';
		
		strings.push_back(str + L"LucasArts\\Star Wars Empire at War Forces of Corruption");
		strings.push_back(str + L"LucasArts\\Star Wars Empire at War Forces of Corruption Demo");
		strings.push_back(str + L"LucasArts\\Star Wars Empire at War\\GameData");
	}
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

static COLORREF ReadBackgroundColor(COLORREF defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value, type, size = sizeof(value);
        if (RegQueryValueEx(hKey, L"BackgroundColor", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        {
            RegCloseKey(hKey);
            return (COLORREF)value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteBackgroundColor(COLORREF color)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD value = (DWORD)color;
        RegSetValueEx(hKey, L"BackgroundColor", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
    }
}

static bool ReadShowGround(bool defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value, type, size = sizeof(value);
        if (RegQueryValueEx(hKey, L"ShowGround", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        {
            RegCloseKey(hKey);
            return value != 0;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteShowGround(bool show)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD value = show ? 1 : 0;
        RegSetValueEx(hKey, L"ShowGround", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
    }
}

// Ground-plane Z offset. REG_BINARY of sizeof(float) — sidesteps the
// DWORD-bit-pattern ambiguity and stays readable in regedit's Modify
// Binary Data view. NaN / Inf round-trip rejected so a corrupt blob
// can't push the plane into "where did my ground go" territory.
static float ReadGroundZ(float defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        float value;
        DWORD type, size = sizeof(value);
        if (RegQueryValueEx(hKey, L"GroundZ", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == sizeof(value) && std::isfinite(value))
        {
            RegCloseKey(hKey);
            return value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteGroundZ(float z)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hKey, L"GroundZ", 0, REG_BINARY, (const BYTE*)&z, sizeof(z));
        RegCloseKey(hKey);
    }
}

// ground-texture index (0..kGroundTextureCount-1). REG_DWORD;
// out-of-range or wrong-type values silently fall back to default
// so a hand-edited or corrupted registry can't crash the editor.
static int ReadGroundTexture(int defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0,
                     KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value;
        DWORD type, size = sizeof(value);
        if (RegQueryValueEx(hKey, L"GroundTexture", NULL, &type,
                            (LPBYTE)&value, &size) == ERROR_SUCCESS
            && type == REG_DWORD && size == sizeof(value)
            && value < (DWORD)Engine::kGroundTextureCount)
        {
            RegCloseKey(hKey);
            return (int)value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteGroundTexture(int index)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL)
        == ERROR_SUCCESS)
    {
        DWORD value = (DWORD)index;
        RegSetValueEx(hKey, L"GroundTexture", 0, REG_DWORD,
                      (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
    }
}

// per-slot custom texture file path. REG_SZ; empty / missing
// values are treated as "use bundled default" (slots 0-5) or
// "slot is empty" (slots 6-11).
static std::wstring ReadGroundSlotPath(int slot)
{
    if (slot < 0 || slot >= Engine::kGroundTextureCount) return L"";
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0,
                     KEY_READ, &hKey) != ERROR_SUCCESS)
        return L"";
    wchar_t valueName[32];
    swprintf(valueName, 32, L"GroundTextureSlot%d", slot);
    // Two-pass read: first query size, then allocate, then read.
    DWORD type = 0, cbData = 0;
    LSTATUS s = RegQueryValueEx(hKey, valueName, NULL, &type, NULL, &cbData);
    if (s != ERROR_SUCCESS || type != REG_SZ || cbData < sizeof(wchar_t))
    {
        RegCloseKey(hKey);
        return L"";
    }
    std::vector<wchar_t> buf(cbData / sizeof(wchar_t) + 1, 0);
    s = RegQueryValueEx(hKey, valueName, NULL, &type, (LPBYTE)buf.data(), &cbData);
    RegCloseKey(hKey);
    if (s != ERROR_SUCCESS) return L"";
    // Ensure NUL termination (registry may or may not include it).
    buf.back() = 0;
    return std::wstring(buf.data());
}

static void WriteGroundSlotPath(int slot, const std::wstring& path)
{
    if (slot < 0 || slot >= Engine::kGroundTextureCount) return;
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL)
        != ERROR_SUCCESS) return;
    wchar_t valueName[32];
    swprintf(valueName, 32, L"GroundTextureSlot%d", slot);
    if (path.empty())
    {
        RegDeleteValue(hKey, valueName);
    }
    else
    {
        // Length in bytes includes trailing NUL.
        DWORD cbData = (DWORD)((path.size() + 1) * sizeof(wchar_t));
        RegSetValueEx(hKey, valueName, 0, REG_SZ, (const BYTE*)path.c_str(), cbData);
    }
    RegCloseKey(hKey);
}

static void DeleteAllGroundSlotPaths()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0,
                     KEY_WRITE, &hKey) != ERROR_SUCCESS) return;
    // Iterate one wider than the current slot count so a user who
    // saved paths under a previous (12-slot) build doesn't end up
    // with orphan entries after a Reset. Pure over-cleanup; harmless
    // when those keys don't exist.
    const int kCleanupBound = 16;
    for (int slot = 0; slot < kCleanupBound; ++slot)
    {
        wchar_t valueName[32];
        swprintf(valueName, 32, L"GroundTextureSlot%d", slot);
        RegDeleteValue(hKey, valueName);
    }
    // Reset-all also wipes the solid-colour value so slot 4 reverts
    // to the flat-grey default on next read.
    RegDeleteValue(hKey, L"GroundSolidColor");
    RegCloseKey(hKey);
}

// solid-colour slot (slot 4). REG_DWORD storing the COLORREF
// directly. Default is RGB(128,128,128) — flat grey.
static COLORREF ReadGroundSolidColor(COLORREF defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0,
                     KEY_READ, &hKey) != ERROR_SUCCESS)
        return defaultValue;
    DWORD value, type, size = sizeof(value);
    LSTATUS s = RegQueryValueEx(hKey, L"GroundSolidColor", NULL, &type,
                                  (LPBYTE)&value, &size);
    RegCloseKey(hKey);
    if (s != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value))
        return defaultValue;
    return (COLORREF)value;
}

static void WriteGroundSolidColor(COLORREF color)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL)
        != ERROR_SUCCESS) return;
    DWORD value = (DWORD)color;
    RegSetValueEx(hKey, L"GroundSolidColor", 0, REG_DWORD,
                  (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);
}

// thumbnail generation. Returns a fresh 32-bit HBITMAP of the
// given size showing the slot's content. Caller owns the bitmap and
// must DeleteObject() it when done.
//
// Resolution order:
//   1. customPath (if non-empty): D3DXCreateTextureFromFileEx
//   2. bundled RCDATA resource for the slot: D3DXCreateTextureFromFileInMemoryEx
//   3. fallback: a light-grey square with an outline (placeholder for
//      empty slots, or for slots whose source failed to load)
//
// Both D3DX paths downsample to (size×size) directly during creation,
// so no separate scale step is needed.
static HBITMAP MakeGroundSlotThumbnail(IDirect3DDevice9*    pDevice,
                                        int                  slot,
                                        int                  size,
                                        const std::wstring&  customPath,
                                        COLORREF             solidColor)
{
    // Helper lambda to construct an empty/error placeholder HBITMAP.
    auto MakePlaceholder = [&](bool empty) -> HBITMAP {
        HDC     hScreen = GetDC(NULL);
        BITMAPINFO bmi   = {};
        bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth       = size;
        bmi.bmiHeader.biHeight      = -size;  // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* pBits = NULL;
        HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hScreen);
        if (hbm == NULL) return NULL;
        // Fill with a light grey, then put a "+" or "?" depending on
        // whether this is an empty (legal) slot vs a load-failed one.
        HDC hMem = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(hMem, hbm);
        RECT r = { 0, 0, size, size };
        HBRUSH bg = CreateSolidBrush(empty ? RGB(0xE8, 0xE8, 0xE8) : RGB(0xC8, 0xA0, 0xA0));
        FillRect(hMem, &r, bg);
        DeleteObject(bg);
        // Border.
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x80, 0x80, 0x80));
        HGDIOBJ oldPen = SelectObject(hMem, pen);
        HGDIOBJ oldBrush = SelectObject(hMem, GetStockObject(NULL_BRUSH));
        Rectangle(hMem, 0, 0, size, size);
        SelectObject(hMem, oldBrush);
        SelectObject(hMem, oldPen);
        DeleteObject(pen);
        // Glyph: "+" for empty, "?" for failure.
        HFONT hFont = CreateFont(size / 2, 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                  L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hMem, hFont);
        SetBkMode(hMem, TRANSPARENT);
        SetTextColor(hMem, RGB(0x80, 0x80, 0x80));
        DrawText(hMem, empty ? L"+" : L"?", -1, &r,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hMem, oldFont);
        DeleteObject(hFont);
        SelectObject(hMem, old);
        DeleteDC(hMem);
        return hbm;
    };

    // Solid-color slot — short-circuit the D3D path entirely and
    // paint a flat-coloured square via GDI. `solidColor` is the
    // engine's m_groundSolidColor passed in by the caller.
    if (slot == Engine::kGroundSolidColorSlot)
    {
        COLORREF c = solidColor;
        (void)customPath;   // unused for this slot
        HDC hScreen = GetDC(NULL);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth       = size;
        bmi.bmiHeader.biHeight      = -size;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* pBits = NULL;
        HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hScreen);
        if (hbm == NULL) return MakePlaceholder(false);
        HDC hMem = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(hMem, hbm);
        RECT r = { 0, 0, size, size };
        HBRUSH br = CreateSolidBrush(c);
        FillRect(hMem, &r, br);
        DeleteObject(br);
        // 1px outline so a near-white colour still has a visible
        // boundary on the white dialog background.
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x60, 0x60, 0x60));
        HGDIOBJ oldPen = SelectObject(hMem, pen);
        HGDIOBJ oldBrush = SelectObject(hMem, GetStockObject(NULL_BRUSH));
        Rectangle(hMem, 0, 0, size, size);
        SelectObject(hMem, oldBrush);
        SelectObject(hMem, oldPen);
        DeleteObject(pen);
        SelectObject(hMem, old);
        DeleteDC(hMem);
        return hbm;
    }

    // Without a valid D3D device, we can only produce a placeholder.
    if (pDevice == NULL) return MakePlaceholder(true);

    // Helper lambda to read a D3D texture's level 0 surface into a
    // newly-allocated HBITMAP. Used by both the file and resource paths.
    auto TextureToHBitmap = [&](IDirect3DTexture9* pTex) -> HBITMAP {
        if (pTex == NULL) return NULL;
        IDirect3DSurface9* pSurf = NULL;
        if (FAILED(pTex->GetSurfaceLevel(0, &pSurf))) return NULL;
        D3DLOCKED_RECT lr;
        if (FAILED(pSurf->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        {
            pSurf->Release();
            return NULL;
        }
        BITMAPINFO bmi   = {};
        bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth       = size;
        bmi.bmiHeader.biHeight      = -size;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC hScreen = GetDC(NULL);
        void* pBits = NULL;
        HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hScreen);
        if (hbm != NULL && pBits != NULL)
        {
            // D3DFMT_A8R8G8B8 row stride may differ from
            // size*4 — copy row-by-row using lr.Pitch.
            const uint8_t* src = (const uint8_t*)lr.pBits;
            uint8_t*       dst = (uint8_t*)pBits;
            const int rowBytes = size * 4;
            for (int y = 0; y < size; ++y)
                memcpy(dst + y * rowBytes, src + y * lr.Pitch, rowBytes);
        }
        pSurf->UnlockRect();
        pSurf->Release();
        return hbm;
    };

    IDirect3DTexture9* pTex = NULL;
    HRESULT hr = E_FAIL;

    // 1. Custom path.
    if (!customPath.empty())
    {
        hr = D3DXCreateTextureFromFileExW(
            pDevice, customPath.c_str(),
            size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
            D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex);
    }
    // 2. Bundled fallback.
    if (FAILED(hr))
    {
        // Mirror the engine's bundled-resource lookup table. Slot
        // values of 0 mean "no bundled default" (slot 4 — solid
        // color, handled specially above — and slots 5..7 which
        // are user-customisable only).
        static const UINT kIds[Engine::kGroundTextureCount] = {
            IDB_GROUND, IDB_GROUND_GRASS, IDB_GROUND_SAND,
            IDB_GROUND_SNOW, 0, 0, 0, 0,
        };
        if (slot >= 0 && slot < Engine::kGroundTextureCount && kIds[slot] != 0)
        {
            HMODULE  hMod  = GetModuleHandle(NULL);
            HRSRC    hRes  = FindResource(hMod, MAKEINTRESOURCE(kIds[slot]), RT_RCDATA);
            HGLOBAL  hData = (hRes != NULL) ? LoadResource(hMod, hRes) : NULL;
            void*    pData = (hData != NULL) ? LockResource(hData)     : NULL;
            DWORD    dwSize = (hRes != NULL) ? SizeofResource(hMod, hRes) : 0;
            if (pData != NULL && dwSize > 0)
            {
                hr = D3DXCreateTextureFromFileInMemoryEx(
                    pDevice, pData, dwSize,
                    size, size, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
                    D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex);
            }
        }
    }

    HBITMAP hbm = NULL;
    if (SUCCEEDED(hr) && pTex != NULL)
        hbm = TextureToHBitmap(pTex);
    if (pTex != NULL) pTex->Release();
    if (hbm == NULL)
    {
        // Couldn't produce a thumbnail. If the slot has any source
        // (custom path or bundled RCDATA), this is a load failure
        // → show "?"; if no source, this is a legitimately empty
        // slot → show "+".
        //
        // "Has bundled" = slot is in the bundled range AND has a
        // non-zero resource ID (slot 4 = Solid Color has bundled
        // index but no RCDATA — handled earlier as an early return,
        // so it shouldn't reach this branch, but check defensively).
        const bool slotHasBundled =
            (slot >= 0 &&
             slot < Engine::kGroundTextureBundledCount &&
             slot != Engine::kGroundSolidColorSlot);
        const bool slotHasSource = (!customPath.empty()) || slotHasBundled;
        hbm = MakePlaceholder(!slotHasSource);
    }
    return hbm;
}

static void RebuildGroundTexturePreviewBitmap(APPLICATION_INFO* info)
{
    if (info == NULL || info->engine == NULL) return;
    HBITMAP hbmOld = info->hGroundTexturePreviewBitmap;
    int slot = info->engine->GetGroundTexture();
    // Toolbar preview is 24×24; build at that size for sharp paint
    // (no GDI stretchblt cost at draw time).
    info->hGroundTexturePreviewBitmap = MakeGroundSlotThumbnail(
        info->engine->GetDevice(), slot, 24,
        info->engine->GetGroundSlotCustomPath(slot),
        info->engine->GetGroundSolidColor());
    if (hbmOld != NULL) DeleteObject(hbmOld);
}

// picker dialog state. Passed via DialogBoxParam's lParam at open,
// stored on the dialog via DWLP_USER, and freed at WM_DESTROY.
struct GroundTexturePickerData
{
    APPLICATION_INFO* info;
    HIMAGELIST        hImageList;
    HWND              hToolTip;        // attached to IDC_GROUND_TEXTURE_PATH_LABEL; shows full path on hover
    int               originalSlot;    // engine selection at dialog open; for Cancel revert
};

// Build the display label for a slot in the picker dialog. Bundled
// slots get the localised name (Dirt / Grass / ...). Custom slots get
// the filename basename if a path is set, else the "Custom N" label.
static std::wstring GroundSlotDisplayName(int slot, const std::wstring& customPath)
{
    if (slot < 0 || slot >= Engine::kGroundTextureCount) return L"";
    if (slot < Engine::kGroundTextureBundledCount)
    {
        if (!customPath.empty() && slot != Engine::kGroundSolidColorSlot)
        {
            // Bundled slot with custom override — show filename.
            // Excludes the solid-color slot (slot 4) which never has
            // a customPath; user changes its colour via colour picker.
            size_t sep = customPath.find_last_of(L"\\/");
            return (sep == std::wstring::npos)
                ? customPath : customPath.substr(sep + 1);
        }
        // Bundled default name.  IDS_GROUND_GREY now reads
        // "Solid Color" — slot 4 uses it.
        static const int kIds[Engine::kGroundTextureBundledCount] = {
            IDS_GROUND_DIRT, IDS_GROUND_GRASS, IDS_GROUND_SAND,
            IDS_GROUND_SNOW, IDS_GROUND_GREY,
        };
        return LoadString(kIds[slot]);
    }
    // User slot (6..11).
    if (!customPath.empty())
    {
        size_t sep = customPath.find_last_of(L"\\/");
        return (sep == std::wstring::npos)
            ? customPath : customPath.substr(sep + 1);
    }
    return LoadString(IDS_GROUND_CUSTOM_BASE + (slot - Engine::kGroundTextureBundledCount));
}

// Rebuild the picker dialog's ListView entirely — image list, item
// labels, selection. Called after any slot-path mutation (set custom,
// reset bundled, clear custom, reset-all).
// Picker-specific thumbnail size — bigger than the toolbar's 64 px
// preview thumbnail and matched to the per-cell tile width so the
// thumbnail fills the cell edge-to-edge (no horizontal padding).
static const int kGroundPickerThumbSize = 192;

// Subclass plumbing — needed because the ListView's native paint
// keeps bleeding through CDRF_SKIPDEFAULT in subtle ways (per-item
// hot-track border with LVS_EX_TRACKSELECT, native-selection blue
// label text). The cleanest fix is to take over WM_PAINT entirely
// in a subclass: native paint never runs, so it can't leak.
static WNDPROC s_groundLVOriginalProc = NULL;
static int     s_groundLVHoverIdx     = -1;
static LRESULT CALLBACK GroundLVSubclassProc(HWND hList, UINT msg,
                                              WPARAM wParam, LPARAM lParam);

static void GroundTexturePicker_RefreshList(HWND hDlg, GroundTexturePickerData* data)
{
    HWND hList = GetDlgItem(hDlg, IDC_GROUND_TEXTURE_LIST);
    int  prevSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(hList);
    if (data->hImageList != NULL) ImageList_Destroy(data->hImageList);
    // Create the picker's imagelist at the bigger size. MakeGroundSlotThumbnail
    // accepts arbitrary sizes and re-renders the slot's source texture
    // (or placeholder) at that resolution.
    data->hImageList = ImageList_Create(kGroundPickerThumbSize,
                                         kGroundPickerThumbSize,
                                         ILC_COLOR32, Engine::kGroundTextureCount, 0);
    ListView_SetImageList(hList, data->hImageList, LVSIL_NORMAL);
    for (int slot = 0; slot < Engine::kGroundTextureCount; ++slot)
    {
        const std::wstring& path = data->info->engine->GetGroundSlotCustomPath(slot);
        HBITMAP hThumb = MakeGroundSlotThumbnail(
            data->info->engine->GetDevice(),
            slot, kGroundPickerThumbSize, path,
            data->info->engine->GetGroundSolidColor());
        int imgIdx = ImageList_Add(data->hImageList, hThumb, NULL);
        if (hThumb != NULL) DeleteObject(hThumb);
        std::wstring label = GroundSlotDisplayName(slot, path);
        LVITEM item = {};
        item.mask     = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem    = slot;
        item.iSubItem = 0;
        item.iImage   = imgIdx;
        item.lParam   = slot;
        item.pszText  = const_cast<LPWSTR>(label.c_str());
        ListView_InsertItem(hList, &item);
    }
    // Restore selection: prefer prevSel (if any), else the engine's
    // current slot.
    int sel = (prevSel >= 0) ? prevSel : data->info->engine->GetGroundTexture();
    if (sel >= 0 && sel < Engine::kGroundTextureCount)
    {
        ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, sel, FALSE);
    }
}

// Update the dialog's path label + tooltip to reflect `path`. Tooltip
// shows the path verbatim; label shows the SS_PATHELLIPSIS-truncated
// form. When `path` is empty, both clear (the label becomes blank and
// the tooltip becomes hidden because TOOLINFO's text is empty).
static void GroundTexturePicker_SetPathDisplay(HWND hDlg,
                                                 GroundTexturePickerData* data,
                                                 const std::wstring& path)
{
    SetDlgItemText(hDlg, IDC_GROUND_TEXTURE_PATH_LABEL,
                    path.empty() ? L"" : path.c_str());
    if (data->hToolTip != NULL)
    {
        TOOLINFOW ti = {};
        // Match the V2 size used at TTM_ADDTOOL time — ComCtl32 v5
        // matches tools by the (hwnd, uId) pair and rejects update
        // calls whose cbSize doesn't match the registered tool.
        ti.cbSize   = TTTOOLINFOW_V2_SIZE;
        ti.uFlags   = TTF_IDISHWND;
        ti.hwnd     = hDlg;
        ti.uId      = (UINT_PTR)GetDlgItem(hDlg, IDC_GROUND_TEXTURE_PATH_LABEL);
        // Empty text suppresses the tooltip popup entirely.
        ti.lpszText = (LPWSTR)path.c_str();
        SendMessage(data->hToolTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

// Helper: open ChooseColor for the solid-colour slot. Returns true if
// the user picked a colour; engine + registry + ListView refreshed in
// either case (no-op on cancel, update on OK).
static bool GroundTexturePicker_PickSolidColor(HWND hDlg,
                                                 GroundTexturePickerData* data)
{
    static COLORREF s_custom[16] = {0};
    CHOOSECOLOR cc = {};
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = hDlg;
    cc.lpCustColors = s_custom;
    cc.rgbResult    = data->info->engine->GetGroundSolidColor();
    cc.Flags        = CC_ANYCOLOR | CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColor(&cc)) return false;
    data->info->engine->SetGroundSolidColor(cc.rgbResult);
    WriteGroundSolidColor(cc.rgbResult);
    // Refresh the list (thumbnail will pick up the new colour) and
    // the toolbar preview if this slot is currently selected.
    GroundTexturePicker_RefreshList(hDlg, data);
    if (data->info->engine->GetGroundTexture() == Engine::kGroundSolidColorSlot)
    {
        RebuildGroundTexturePreviewBitmap(data->info);
        InvalidateRect(data->info->hGroundTexturePreview, NULL, TRUE);
        RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_UPDATENOW);
    }
    return true;
}

// Helper: open a file-picker dialog and assign the result (if any) to
// `slot`'s custom path. Updates engine + registry + ListView. Returns
// true if a file was assigned.
static bool GroundTexturePicker_PickCustomFile(HWND hDlg,
                                                 GroundTexturePickerData* data,
                                                 int slot)
{
    if (slot < 0 || slot >= Engine::kGroundTextureCount) return false;
    wchar_t buf[1024] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hDlg;
    ofn.lpstrFilter  = L"Texture Files (*.bmp;*.dds;*.tga;*.png;*.jpg;*.jpeg)\0*.bmp;*.dds;*.tga;*.png;*.jpg;*.jpeg\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile    = buf;
    ofn.nMaxFile     = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle   = L"Choose Ground Texture File";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::wstring path = buf;
    data->info->engine->SetGroundSlotCustomPath(slot, path);
    WriteGroundSlotPath(slot, path);
    // Refresh the dialog list to reflect the new thumbnail.
    GroundTexturePicker_RefreshList(hDlg, data);
    // Also refresh the toolbar preview if this slot is currently selected.
    if (data->info->engine->GetGroundTexture() == slot)
    {
        RebuildGroundTexturePreviewBitmap(data->info);
        InvalidateRect(data->info->hGroundTexturePreview, NULL, TRUE);
        RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_UPDATENOW);
    }
    return true;
}

static INT_PTR CALLBACK GroundTexturePickerProc(HWND hDlg, UINT uMsg,
                                                  WPARAM wParam, LPARAM lParam)
{
    GroundTexturePickerData* data =
        (GroundTexturePickerData*)(LONG_PTR)GetWindowLongPtr(hDlg, DWLP_USER);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        data = (GroundTexturePickerData*)lParam;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)data);
        data->hImageList = NULL;
        data->hToolTip   = NULL;
        HWND hList = GetDlgItem(hDlg, IDC_GROUND_TEXTURE_LIST);
        // LVS_EX_DOUBLEBUFFER — flicker-free repaint on cursor motion.
        // LVS_EX_TRACKSELECT deliberately NOT set: it draws hot-track
        // chrome the per-item custom-draw can't cleanly suppress; my
        // subclass paints hover state itself based on cursor hit-test.
        ListView_SetExtendedListViewStyle(hList, LVS_EX_DOUBLEBUFFER);
        // Layout: 192×192 thumb (matches kGroundPickerThumbSize).
        // Spacing 208×232 keeps a 16 px gap between cells and ~40 px
        // below the thumb for the filename label.
        // 4 × 208 = 832 px fits in the widened ~840 px listview;
        // 2 × 232 = 464 px fits in the ~470 px listview height.
        ListView_SetIconSpacing(hList, 208, 232);
        // Subclass the ListView so we own WM_PAINT entirely. The
        // native ListView's selection / focus / hot-track painting
        // can't bleed through if it never runs.
        if (s_groundLVOriginalProc == NULL)
        {
            s_groundLVOriginalProc = (WNDPROC)SetWindowLongPtr(
                hList, GWLP_WNDPROC, (LONG_PTR)GroundLVSubclassProc);
        }
        else
        {
            // Re-attach for subsequent show cycles (the modeless dialog
            // keeps the same HWND, but defensively re-install in case).
            SetWindowLongPtr(hList, GWLP_WNDPROC, (LONG_PTR)GroundLVSubclassProc);
        }
        s_groundLVHoverIdx = -1;
        // Hide the bottom path label — slot labels (Dirt / Grass /
        // filename basename for custom) already convey what each slot
        // is. The full-path duplication isn't pulling its weight.
        HWND hPath = GetDlgItem(hDlg, IDC_GROUND_TEXTURE_PATH_LABEL);
        if (hPath != NULL) ShowWindow(hPath, SW_HIDE);
        // Build the list — populates thumbnails, labels, selection.
        GroundTexturePicker_RefreshList(hDlg, data);
        // Attach a tooltip to the path label so the user can see
        // the full path verbatim when the SS_PATHELLIPSIS-truncated
        // label is hovered. TTF_SUBCLASS routes the label's mouse
        // events to the tooltip control automatically.
        HWND hLabel = GetDlgItem(hDlg, IDC_GROUND_TEXTURE_PATH_LABEL);
        data->hToolTip = CreateWindowExW(0, TOOLTIPS_CLASS, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hDlg, NULL, GetModuleHandle(NULL), NULL);
        if (data->hToolTip != NULL && hLabel != NULL)
        {
            // ComCtl32 v5 (the editor has no manifest opting into v6)
            // rejects the modern sizeof(TOOLINFOW) which includes
            // lpReserved — TTM_ADDTOOL returns FALSE and no tooltip
            // appears. Use the V2 size (60 bytes) which is the
            // largest tooltip-struct size v5 accepts. Fields past
            // V2 aren't used here, so this is a pure compatibility
            // downgrade with no functional cost.
            TOOLINFOW ti = {};
            ti.cbSize   = TTTOOLINFOW_V2_SIZE;
            ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd     = hDlg;
            ti.uId      = (UINT_PTR)hLabel;
            ti.lpszText = (LPWSTR)L"";   // populated when selection changes
            SendMessage(data->hToolTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            // Allow long paths and multi-line wrapping if needed.
            SendMessage(data->hToolTip, TTM_SETMAXTIPWIDTH, 0, 600);
            // Shorten initial hover delay so the tooltip feels
            // responsive — default is ~500 ms.
            SendMessage(data->hToolTip, TTM_SETDELAYTIME, TTDT_INITIAL, 250);
        }
        // Initial LVN_ITEMCHANGED fired before the tooltip existed,
        // so the tooltip text wasn't populated yet. Sync once now
        // with the currently-selected slot's path.
        {
            int slot = data->info->engine->GetGroundTexture();
            const std::wstring& path =
                data->info->engine->GetGroundSlotCustomPath(slot);
            GroundTexturePicker_SetPathDisplay(hDlg, data, path);
        }
        return TRUE;
    }
    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom != IDC_GROUND_TEXTURE_LIST) break;
        // Match the texture-palette popup's blue hover/selection
        // frame styling. The ListView's native LVS_EX_TRACKSELECT
        // gives a theme-dependent hot-track highlight; this overlay
        // adds a deliberate blue frame on top so the two popups
        // read the same way.
        if (hdr->code == NM_CUSTOMDRAW)
        {
            LPNMLVCUSTOMDRAW lpc = (LPNMLVCUSTOMDRAW)lParam;
            switch (lpc->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;
            case CDDS_ITEMPREPAINT:
            {
                // Take over the entire item paint so the cell looks
                // pixel-identical to the texture-palette popup's cells:
                //   - blue cell bg on hover
                //   - blue frame around the thumbnail (3 px hover,
                //     2 px selected, 1 px default)
                //   - pin badge in top-right on hover (filled if this
                //     slot is currently active, hollow otherwise)
                //   - filename label below the thumbnail, centred
                // CDRF_SKIPDEFAULT at the end suppresses native paint.
                HWND hList = lpc->nmcd.hdr.hwndFrom;
                int  iItem = (int)lpc->nmcd.dwItemSpec;
                HDC  hdc   = lpc->nmcd.hdc;

                // Hover detection: prefer the cursor hit-test (works
                // even if LVS_EX_TRACKSELECT's internal hot-item state
                // isn't updated mid-paint). Falls back to GetHotItem.
                POINT cursor;
                GetCursorPos(&cursor);
                ScreenToClient(hList, &cursor);
                LVHITTESTINFO ht = {};
                ht.pt = cursor;
                int hotIdx = ListView_HitTest(hList, &ht);
                if (hotIdx < 0) hotIdx = ListView_GetHotItem(hList);

                const bool selected = (lpc->nmcd.uItemState & CDIS_SELECTED) != 0;
                const bool hovered  = (hotIdx == iItem);

                RECT bounds, iconRc, labelRc;
                ListView_GetItemRect(hList, iItem, &bounds,  LVIR_BOUNDS);
                ListView_GetItemRect(hList, iItem, &iconRc,  LVIR_ICON);
                ListView_GetItemRect(hList, iItem, &labelRc, LVIR_LABEL);

                // 1. Cell background — match the palette popup exactly:
                //    blue on hover, light grey (button-face) otherwise.
                const COLORREF bgCol = hovered
                    ? RGB(160, 200, 250)
                    : RGB(240, 240, 240);
                HBRUSH bg = CreateSolidBrush(bgCol);
                FillRect(hdc, &bounds, bg);
                DeleteObject(bg);

                // 2. Thumbnail centred in the icon rect.
                HIMAGELIST hIL = ListView_GetImageList(hList, LVSIL_NORMAL);
                LVITEMW item = {};
                item.mask  = LVIF_IMAGE;
                item.iItem = iItem;
                ListView_GetItem(hList, &item);
                if (hIL != NULL && item.iImage >= 0)
                {
                    IMAGEINFO ii = {};
                    ImageList_GetImageInfo(hIL, item.iImage, &ii);
                    const int imgW = ii.rcImage.right  - ii.rcImage.left;
                    const int imgH = ii.rcImage.bottom - ii.rcImage.top;
                    const int ix = iconRc.left
                                 + (iconRc.right - iconRc.left - imgW) / 2;
                    const int iy = iconRc.top
                                 + (iconRc.bottom - iconRc.top - imgH) / 2;
                    ImageList_Draw(hIL, item.iImage, hdc, ix, iy, ILD_NORMAL);
                }

                // 3. Frame.
                HPEN pen;
                if (selected)
                    pen = CreatePen(PS_SOLID, 2, RGB(40, 100, 220));
                else if (hovered)
                    pen = CreatePen(PS_SOLID, 3, RGB(70, 150, 240));
                else
                    pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
                HGDIOBJ oldP = SelectObject(hdc, pen);
                HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc,
                          iconRc.left + 1, iconRc.top + 1,
                          iconRc.right - 1, iconRc.bottom - 1);
                SelectObject(hdc, oldP);
                SelectObject(hdc, oldB);
                DeleteObject(pen);

                // 4. Pin badge in the top-right corner on hover.
                //    Filled (red) if this slot is the engine's current
                //    selection; hollow otherwise. Visual match with
                //    the palette popup — for ground the badge has no
                //    independent function (right-click is still the
                //    slot-management entry point).
                if (hovered)
                {
                    static HBITMAP s_pinBadge = NULL;
                    if (s_pinBadge == NULL)
                    {
                        s_pinBadge = (HBITMAP)LoadImageW(GetModuleHandle(NULL),
                            MAKEINTRESOURCEW(IDB_PIN_BADGE),
                            IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
                    }
                    if (s_pinBadge != NULL)
                    {
                        const int badgePx = 24;
                        const int inset   = 4;
                        const int bx = iconRc.right - inset - badgePx;
                        const int by = iconRc.top   + inset;
                        const bool isCurrent =
                            (iItem == data->info->engine->GetGroundTexture());
                        const int srcY = isCurrent ? badgePx : 0;
                        HDC hMem = CreateCompatibleDC(hdc);
                        HGDIOBJ oldBm = SelectObject(hMem, s_pinBadge);
                        BitBlt(hdc, bx, by, badgePx, badgePx, hMem, 0, srcY, SRCCOPY);
                        SelectObject(hMem, oldBm);
                        DeleteDC(hMem);
                    }
                }

                // 5. Filename label below the thumbnail.
                HFONT hFont = (HFONT)SendMessage(hDlg, WM_GETFONT, 0, 0);
                if (hFont == NULL) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                HFONT oldFont = (HFONT)SelectObject(hdc, hFont);

                WCHAR labelBuf[256] = L"";
                ListView_GetItemText(hList, iItem, 0, labelBuf,
                                     (int)_countof(labelBuf));
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(40, 40, 40));
                DrawTextW(hdc, labelBuf, -1, &labelRc,
                          DT_SINGLELINE | DT_CENTER | DT_VCENTER
                          | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(hdc, oldFont);

                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
            }
            SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
            return TRUE;
        }
        if (hdr->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* nlv = (NMLISTVIEW*)lParam;
            if ((nlv->uNewState & LVIS_SELECTED) && !(nlv->uOldState & LVIS_SELECTED))
            {
                int slot = nlv->iItem;
                // Live-select: switch engine + persist + refresh
                // toolbar. Skip if slot is empty — the user gets
                // a file picker on click instead (handled below).
                if (slot >= 0 && slot < Engine::kGroundTextureCount &&
                    !data->info->engine->IsGroundSlotEmpty(slot))
                {
                    data->info->engine->SetGroundTexture(slot);
                    int actual = data->info->engine->GetGroundTexture();
                    WriteGroundTexture(actual);
                    RebuildGroundTexturePreviewBitmap(data->info);
                    InvalidateRect(data->info->hGroundTexturePreview, NULL, TRUE);
                    RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                                 RDW_INVALIDATE | RDW_UPDATENOW);
                    // Update bottom label + tooltip with the file
                    // path (or empty for non-custom slots).
                    const std::wstring& path =
                        data->info->engine->GetGroundSlotCustomPath(slot);
                    GroundTexturePicker_SetPathDisplay(hDlg, data, path);
                }
            }
        }
        else if (hdr->code == NM_CLICK || hdr->code == NM_DBLCLK)
        {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
            int slot = nia->iItem;
            if (slot < 0 || slot >= Engine::kGroundTextureCount) break;
            // Solid-colour slot: any click opens the colour picker.
            // Select-first so the engine swap happens before the
            // modal so the user can see the current colour on
            // the ground while ChooseColor is open.
            if (slot == Engine::kGroundSolidColorSlot)
            {
                HWND hList = GetDlgItem(hDlg, IDC_GROUND_TEXTURE_LIST);
                ListView_SetItemState(hList, slot,
                    LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                GroundTexturePicker_PickSolidColor(hDlg, data);
            }
            // Single-click on empty slot opens the file picker.
            else if (data->info->engine->IsGroundSlotEmpty(slot))
            {
                GroundTexturePicker_PickCustomFile(hDlg, data, slot);
                // After assignment, select the slot so the engine
                // switches to the freshly-loaded texture.
                if (!data->info->engine->IsGroundSlotEmpty(slot))
                {
                    HWND hList = GetDlgItem(hDlg, IDC_GROUND_TEXTURE_LIST);
                    ListView_SetItemState(hList, slot,
                        LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
            else
            {
                // Populated, non-solid-colour slot: any click commits
                // and closes. The LVN_ITEMCHANGED handler already
                // swapped the engine texture as the user clicked
                // (live-select), so closing here just dismisses the
                // popup without an extra OK round-trip. Matches the
                // texture-palette popup's commit-and-close behaviour.
                {
                    RECT r; GetWindowRect(hDlg, &r);
                    WriteGroundPickerPos(r.left, r.top);
                    ShowWindow(hDlg, SW_HIDE);
                }
            }
        }
        else if (hdr->code == NM_RCLICK)
        {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
            int slot = nia->iItem;
            if (slot < 0 || slot >= Engine::kGroundTextureCount) break;
            // Build context menu in-place.
            HMENU hMenu = CreatePopupMenu();
            const bool isBundled = (slot < Engine::kGroundTextureBundledCount);
            const bool isSolidColor = (slot == Engine::kGroundSolidColorSlot);
            const std::wstring& path =
                data->info->engine->GetGroundSlotCustomPath(slot);
            const bool hasCustom = !path.empty();
            if (isSolidColor)
            {
                // Solid-colour slot has only one menu action.
                AppendMenuW(hMenu, MF_STRING, ID_GROUND_SLOT_SET_CUSTOM,
                            L"Change color...");
            }
            else
            {
                AppendMenuW(hMenu, MF_STRING, ID_GROUND_SLOT_SET_CUSTOM,
                            L"Set custom texture...");
                if (isBundled && hasCustom)
                {
                    AppendMenuW(hMenu, MF_STRING, ID_GROUND_SLOT_RESET_BUNDLED,
                                L"Reset to bundled default");
                }
                if (!isBundled && hasCustom)
                {
                    AppendMenuW(hMenu, MF_STRING, ID_GROUND_SLOT_CLEAR_CUSTOM,
                                L"Clear slot");
                }
            }
            POINT pt;
            GetCursorPos(&pt);
            INT cmd = TrackPopupMenu(hMenu,
                                      TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RETURNCMD | TPM_NONOTIFY,
                                      pt.x, pt.y, 0, hDlg, NULL);
            DestroyMenu(hMenu);
            if (cmd == ID_GROUND_SLOT_SET_CUSTOM)
            {
                if (isSolidColor)
                    GroundTexturePicker_PickSolidColor(hDlg, data);
                else
                    GroundTexturePicker_PickCustomFile(hDlg, data, slot);
            }
            else if (cmd == ID_GROUND_SLOT_RESET_BUNDLED ||
                     cmd == ID_GROUND_SLOT_CLEAR_CUSTOM)
            {
                // Both commands wipe the slot's custom path. Engine
                // re-derives the slot's source (bundled for 0-5,
                // empty for 6-11).
                data->info->engine->SetGroundSlotCustomPath(slot, L"");
                WriteGroundSlotPath(slot, L"");
                GroundTexturePicker_RefreshList(hDlg, data);
                if (data->info->engine->GetGroundTexture() == slot)
                {
                    RebuildGroundTexturePreviewBitmap(data->info);
                    InvalidateRect(data->info->hGroundTexturePreview, NULL, TRUE);
                    RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                                 RDW_INVALIDATE | RDW_UPDATENOW);
                }
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_GROUND_TEXTURE_RESET_ALL:
        {
            if (MessageBox(hDlg,
                           L"Reset every ground texture slot to defaults? "
                           L"Slots 1-6 (Dirt..Grey) revert to their bundled "
                           L"defaults. Slots 7-12 become empty. Custom file "
                           L"assignments will be lost — but the files "
                           L"themselves are not deleted.",
                           L"Reset All Slots",
                           MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
            // Clear every slot's custom path.
            for (int slot = 0; slot < Engine::kGroundTextureCount; ++slot)
            {
                data->info->engine->SetGroundSlotCustomPath(slot, L"");
            }
            DeleteAllGroundSlotPaths();
            GroundTexturePicker_RefreshList(hDlg, data);
            RebuildGroundTexturePreviewBitmap(data->info);
            InvalidateRect(data->info->hGroundTexturePreview, NULL, TRUE);
            RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                         RDW_INVALIDATE | RDW_UPDATENOW);
            return TRUE;
        }
        case IDOK:
        {
            // Live selection has already been committed; hide and
            // persist position. (Modeless — the dialog instance is
            // kept alive for the next ShowGroundTexturePicker call.)
            RECT r; GetWindowRect(hDlg, &r);
            WriteGroundPickerPos(r.left, r.top);
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        case IDCANCEL:
        {
            // Revert engine selection to whatever was active at the
            // start of this show session. Slot-path mutations stay
            // (they're "data"). Then hide + persist position.
            if (data->info->engine->GetGroundTexture() != data->originalSlot)
            {
                data->info->engine->SetGroundTexture(data->originalSlot);
                WriteGroundTexture(data->info->engine->GetGroundTexture());
                RebuildGroundTexturePreviewBitmap(data->info);
                InvalidateRect(data->info->hGroundTexturePreview, NULL, TRUE);
                RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                             RDW_INVALIDATE | RDW_UPDATENOW);
            }
            RECT r; GetWindowRect(hDlg, &r);
            WriteGroundPickerPos(r.left, r.top);
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        }
        break;
    case WM_CLOSE:
        // Title-bar X button. Treat as OK (live changes already
        // committed) rather than Cancel — matches palette popup
        // behaviour. The user has explicit Cancel button if they
        // want to revert.
        {
            RECT r; GetWindowRect(hDlg, &r);
            WriteGroundPickerPos(r.left, r.top);
            ShowWindow(hDlg, SW_HIDE);
        }
        return TRUE;
    case WM_KEYDOWN:
        // Esc: dismiss without reverting (matches palette popup).
        if (wParam == VK_ESCAPE)
        {
            RECT r; GetWindowRect(hDlg, &r);
            WriteGroundPickerPos(r.left, r.top);
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (data != NULL && data->hImageList != NULL)
        {
            ImageList_Destroy(data->hImageList);
            data->hImageList = NULL;
        }
        break;
    }
    return FALSE;
}

// Custom paint for the ground-texture picker's ListView. Mirrors the
// texture-palette popup's DrawCell so the two popups look identical:
// hover blue tint, blue frame (3 px hover / 2 px selected / 1 px
// default), thumbnail centred in icon rect, pin badge top-right on
// hover, filename below.
//
// Double-buffered. Pulls the engine's current ground slot from
// GroundTexturePickerData stored in the parent dialog's DWLP_USER.
static void GroundLV_PaintAll(HWND hList, HDC hdcScreen, const RECT& rcClient)
{
    HWND hDlg = GetParent(hList);
    GroundTexturePickerData* data =
        (GroundTexturePickerData*)(LONG_PTR)GetWindowLongPtr(hDlg, DWLP_USER);

    // Off-screen buffer.
    HDC     hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen,
                                            rcClient.right, rcClient.bottom);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, hbmMem);
    HDC     hdc    = hdcMem;

    // Background — light grey, same constant as the palette popup's
    // non-hovered cell colour.
    HBRUSH bgBrush = CreateSolidBrush(RGB(240, 240, 240));
    FillRect(hdc, &rcClient, bgBrush);
    DeleteObject(bgBrush);

    if (data == NULL || data->info == NULL || data->info->engine == NULL)
    {
        BitBlt(hdcScreen, 0, 0, rcClient.right, rcClient.bottom,
               hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        return;
    }

    // Dialog font for the filename labels.
    HFONT hFont = (HFONT)SendMessage(hDlg, WM_GETFONT, 0, 0);
    if (hFont == NULL) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    const int currentSlot = data->info->engine->GetGroundTexture();
    const int itemCount   = ListView_GetItemCount(hList);
    HIMAGELIST hIL        = ListView_GetImageList(hList, LVSIL_NORMAL);

    for (int i = 0; i < itemCount; ++i)
    {
        RECT bounds, iconRc, labelRc;
        ListView_GetItemRect(hList, i, &bounds,  LVIR_BOUNDS);
        ListView_GetItemRect(hList, i, &iconRc,  LVIR_ICON);
        ListView_GetItemRect(hList, i, &labelRc, LVIR_LABEL);

        const bool hovered  = (s_groundLVHoverIdx == i);
        const bool selected = (i == currentSlot);

        // Cell background.
        const COLORREF bgCol = hovered
            ? RGB(160, 200, 250)
            : RGB(240, 240, 240);
        HBRUSH bg = CreateSolidBrush(bgCol);
        FillRect(hdc, &bounds, bg);
        DeleteObject(bg);

        // Thumbnail.
        if (hIL != NULL)
        {
            LVITEMW item = {};
            item.mask  = LVIF_IMAGE;
            item.iItem = i;
            ListView_GetItem(hList, &item);
            if (item.iImage >= 0)
            {
                IMAGEINFO ii = {};
                ImageList_GetImageInfo(hIL, item.iImage, &ii);
                const int imgW = ii.rcImage.right  - ii.rcImage.left;
                const int imgH = ii.rcImage.bottom - ii.rcImage.top;
                const int ix = iconRc.left
                             + (iconRc.right - iconRc.left - imgW) / 2;
                const int iy = iconRc.top
                             + (iconRc.bottom - iconRc.top - imgH) / 2;
                ImageList_Draw(hIL, item.iImage, hdc, ix, iy, ILD_NORMAL);
            }
        }

        // Frame.
        HPEN pen;
        if (selected)
            pen = CreatePen(PS_SOLID, 2, RGB(40, 100, 220));
        else if (hovered)
            pen = CreatePen(PS_SOLID, 3, RGB(70, 150, 240));
        else
            pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
        HGDIOBJ oldP = SelectObject(hdc, pen);
        HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc,
                  iconRc.left + 1, iconRc.top + 1,
                  iconRc.right - 1, iconRc.bottom - 1);
        SelectObject(hdc, oldP);
        SelectObject(hdc, oldB);
        DeleteObject(pen);

        // (No pin badge for ground slots — they're fixed engine slots,
        // not "pinnable"; the badge would be decorative-only and
        // confused the workflow per user feedback.)

        // Filename label.
        WCHAR labelBuf[256] = L"";
        ListView_GetItemText(hList, i, 0, labelBuf, (int)_countof(labelBuf));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(40, 40, 40));
        DrawTextW(hdc, labelBuf, -1, &labelRc,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER
                  | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SelectObject(hdc, hOldFont);

    // Flush to screen.
    BitBlt(hdcScreen, 0, 0, rcClient.right, rcClient.bottom,
           hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

static LRESULT CALLBACK GroundLVSubclassProc(HWND hList, UINT msg,
                                              WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;   // we handle the entire bg in WM_PAINT
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hList, &ps);
        RECT rcClient;
        GetClientRect(hList, &rcClient);
        GroundLV_PaintAll(hList, hdc, rcClient);
        EndPaint(hList, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        // Update hover via cursor hit-test. Same approach as the
        // palette popup's WM_MOUSEMOVE.
        POINT cur = { (LONG)(short)LOWORD(lParam),
                      (LONG)(short)HIWORD(lParam) };
        LVHITTESTINFO ht = {};
        ht.pt = cur;
        const int newHot = ListView_HitTest(hList, &ht);
        if (newHot != s_groundLVHoverIdx)
        {
            s_groundLVHoverIdx = newHot;
            InvalidateRect(hList, NULL, FALSE);
            TRACKMOUSEEVENT tme = {};
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hList;
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (s_groundLVHoverIdx != -1)
        {
            s_groundLVHoverIdx = -1;
            InvalidateRect(hList, NULL, FALSE);
        }
        break;
    }
    return CallWindowProc(s_groundLVOriginalProc, hList, msg, wParam, lParam);
}

// Position persistence for the modeless ground-texture picker.
// Stored as two DWORDs to match the existing HKCU\Software\AloParticleEditor
// pattern. Both keys must be present; otherwise the caller falls back
// to the dialog's default centering.
struct GroundPickerPos { int x; int y; bool valid; };
static GroundPickerPos ReadGroundPickerPos()
{
    GroundPickerPos p = { 0, 0, false };
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD x = 0, y = 0, type = 0, size = sizeof(DWORD);
        BOOL gotX = (RegQueryValueEx(hKey, L"GroundPickerX", NULL, &type, (LPBYTE)&x, &size) == ERROR_SUCCESS && type == REG_DWORD);
        size = sizeof(DWORD);
        BOOL gotY = (RegQueryValueEx(hKey, L"GroundPickerY", NULL, &type, (LPBYTE)&y, &size) == ERROR_SUCCESS && type == REG_DWORD);
        RegCloseKey(hKey);
        if (gotX && gotY) { p.x = (int)x; p.y = (int)y; p.valid = true; }
    }
    return p;
}
static void WriteGroundPickerPos(int x, int y)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD dx = (DWORD)x, dy = (DWORD)y;
        RegSetValueEx(hKey, L"GroundPickerX", 0, REG_DWORD, (BYTE*)&dx, sizeof(dx));
        RegSetValueEx(hKey, L"GroundPickerY", 0, REG_DWORD, (BYTE*)&dy, sizeof(dy));
        RegCloseKey(hKey);
    }
}

// Modeless ground-texture picker.
//
// Created lazily on first call; subsequent calls toggle visibility.
// Carries WS_EX_TOOLWINDOW to get the slim tool-window title bar, and
// the data struct is static so it survives across show/hide cycles.
// First show after launch reads the saved window position from the
// registry; hide writes it back.
//
// Cancel reverts to the slot active at the start of the most recent
// show session (data->originalSlot is re-recorded on every show).
static void ShowGroundTexturePicker(HWND hParent, APPLICATION_INFO* info)
{
    if (info == NULL || info->engine == NULL) return;
    static HWND s_hPicker = NULL;
    static GroundTexturePickerData s_data;
    s_data.info         = info;
    s_data.hImageList   = NULL;
    s_data.originalSlot = info->engine->GetGroundTexture();

    if (s_hPicker != NULL && IsWindowVisible(s_hPicker))
    {
        // Already visible — toggle hide, persist position.
        RECT r; GetWindowRect(s_hPicker, &r);
        WriteGroundPickerPos(r.left, r.top);
        ShowWindow(s_hPicker, SW_HIDE);
        return;
    }
    if (s_hPicker == NULL)
    {
        s_hPicker = CreateDialogParamW(GetModuleHandle(NULL),
                                        MAKEINTRESOURCEW(IDD_GROUND_TEXTURE_PICKER),
                                        hParent, GroundTexturePickerProc,
                                        (LPARAM)&s_data);
        if (s_hPicker == NULL) return;
        info->hGroundPicker = s_hPicker;
        // Apply tool-window title bar styling. Has to be after creation
        // because WS_EX_TOOLWINDOW isn't in the dialog template's
        // EXSTYLE; SWP_FRAMECHANGED forces the non-client area to
        // repaint with the new style.
        LONG_PTR ex = GetWindowLongPtr(s_hPicker, GWL_EXSTYLE);
        SetWindowLongPtr(s_hPicker, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
        SetWindowPos(s_hPicker, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        // Restore saved position if any.
        GroundPickerPos pos = ReadGroundPickerPos();
        if (pos.valid)
        {
            SetWindowPos(s_hPicker, NULL, pos.x, pos.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    else
    {
        // Existing instance — refresh in case the engine state changed
        // while hidden (custom paths added via the right-click menu in
        // another popup, etc.).
        GroundTexturePicker_RefreshList(s_hPicker, &s_data);
    }
    ShowWindow(s_hPicker, SW_SHOW);
    SetForegroundWindow(s_hPicker);
}

// ---------------------------------------------------------------------------
//  Skydome picker dialog + toolbar preview
// ---------------------------------------------------------------------------

// MakeSkydomeSlotThumbnail — produces a sizePx×sizePx HBITMAP for `slot`.
//
//  Slot 0        (Off)         flat bgColor square with centred "✕" glyph.
//  Slots 1-8     (bundled)     decoded from RCDATA via D3DX into a DIB.
//  Slots 9-11    (custom)      loaded from customPath if non-empty;
//                              falls back to "+" placeholder if empty or
//                              load fails.
//
// Mirrors MakeGroundSlotThumbnail's LockRect + CreateDIBSection pattern.
static HBITMAP MakeSkydomeSlotThumbnail(IDirect3DDevice9*    pDevice,
                                         int                  slot,
                                         int                  sizePx,
                                         const std::wstring&  customPath,
                                         COLORREF             bgColor,
                                         IFileManager*        fileManager)
{
    // Helper: create a placeholder HBITMAP. `empty`=true → "+" glyph on
    // light grey; `empty`=false → "?" on muted red (load failure).
    auto MakePlaceholder = [&](bool empty) -> HBITMAP {
        HDC hScreen = GetDC(NULL);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth       = sizePx;
        bmi.bmiHeader.biHeight      = -sizePx;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* pBits = NULL;
        HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hScreen);
        if (hbm == NULL) return NULL;
        HDC hMem = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(hMem, hbm);
        RECT r = { 0, 0, sizePx, sizePx };
        HBRUSH bg = CreateSolidBrush(empty ? RGB(0xE8, 0xE8, 0xE8) : RGB(0xC8, 0xA0, 0xA0));
        FillRect(hMem, &r, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x80, 0x80, 0x80));
        HGDIOBJ oldPen   = SelectObject(hMem, pen);
        HGDIOBJ oldBrush = SelectObject(hMem, GetStockObject(NULL_BRUSH));
        Rectangle(hMem, 0, 0, sizePx, sizePx);
        SelectObject(hMem, oldBrush);
        SelectObject(hMem, oldPen);
        DeleteObject(pen);
        HFONT hFont = CreateFont(sizePx / 2, 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hMem, hFont);
        SetBkMode(hMem, TRANSPARENT);
        SetTextColor(hMem, RGB(0x80, 0x80, 0x80));
        DrawText(hMem, empty ? L"+" : L"?", -1, &r,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hMem, oldFont);
        DeleteObject(hFont);
        SelectObject(hMem, old);
        DeleteDC(hMem);
        return hbm;
    };

    // Slot 0 — Solid colour: flat fill of the user's background colour
    // with a 1px outline. The swatch IS the affordance ("this is the
    // colour the viewport will show"); no glyph needed after the
    // rework that folded the Background button into the picker.
    if (slot == Engine::kSkydomeOffSlot)
    {
        HDC hScreen = GetDC(NULL);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth       = sizePx;
        bmi.bmiHeader.biHeight      = -sizePx;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* pBits = NULL;
        HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hScreen);
        if (hbm == NULL) return MakePlaceholder(true);
        HDC hMem = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(hMem, hbm);
        RECT r = { 0, 0, sizePx, sizePx };
        HBRUSH br = CreateSolidBrush(bgColor);
        FillRect(hMem, &r, br);
        DeleteObject(br);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x60, 0x60, 0x60));
        HGDIOBJ oldPen   = SelectObject(hMem, pen);
        HGDIOBJ oldBrush = SelectObject(hMem, GetStockObject(NULL_BRUSH));
        Rectangle(hMem, 0, 0, sizePx, sizePx);
        SelectObject(hMem, oldBrush);
        SelectObject(hMem, oldPen);
        DeleteObject(pen);
        SelectObject(hMem, old);
        DeleteDC(hMem);
        return hbm;
    }

    // Without a D3D device we can only produce a placeholder.
    if (pDevice == NULL) return MakePlaceholder(true);

    // Helper: copy level-0 surface of a D3D texture into a new HBITMAP.
    auto TextureToHBitmap = [&](IDirect3DTexture9* pTex) -> HBITMAP {
        if (pTex == NULL) return NULL;
        IDirect3DSurface9* pSurf = NULL;
        if (FAILED(pTex->GetSurfaceLevel(0, &pSurf))) return NULL;
        D3DLOCKED_RECT lr;
        if (FAILED(pSurf->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        {
            pSurf->Release();
            return NULL;
        }
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth       = sizePx;
        bmi.bmiHeader.biHeight      = -sizePx;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC hScreen = GetDC(NULL);
        void* pBits = NULL;
        HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
        ReleaseDC(NULL, hScreen);
        if (hbm != NULL && pBits != NULL)
        {
            const uint8_t* src  = (const uint8_t*)lr.pBits;
            uint8_t*       dst  = (uint8_t*)pBits;
            const int      rowB = sizePx * 4;
            for (int y = 0; y < sizePx; ++y)
                memcpy(dst + y * rowB, src + y * lr.Pitch, rowB);
        }
        pSurf->UnlockRect();
        pSurf->Release();
        return hbm;
    };

    IDirect3DTexture9* pTex = NULL;
    HRESULT hr = E_FAIL;

    // follow-up helper: load via FileManager bytes into a sized texture.
    // Keeps the thumbnail builder's resolution chain in sync with the
    // engine's ReloadSkydomeTexture so the picker thumbnail can't disagree
    // with what the viewport will render.
    auto LoadFromFileManagerBytes = [&](const std::string& path) -> HRESULT {
        if (fileManager == NULL) return E_FAIL;
        IFile* file = fileManager->getFile(path);
        if (file == NULL) return E_FAIL;
        const unsigned long sz = file->size();
        if (sz == 0) { file->Release(); return E_FAIL; }
        char* buf = new char[sz];
        file->read(buf, sz);
        file->Release();
        HRESULT lr = D3DXCreateTextureFromFileInMemoryEx(
            pDevice, buf, sz,
            sizePx, sizePx, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
            D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex);
        delete[] buf;
        return lr;
    };

    // Custom path has highest priority (applies to all slot types, though
    // in practice only slots 9-11 carry custom paths). Try FileManager
    // first so a path like "DATA\\ART\\TEXTURES\\foo.dds" resolves via the
    // active mod / base-game chain; fall back to direct file I/O for
    // legacy absolute-path custom slots.
    if (!customPath.empty())
    {
        hr = LoadFromFileManagerBytes(WideToAnsi(customPath));
        if (FAILED(hr))
        {
            hr = D3DXCreateTextureFromFileExW(
                pDevice, customPath.c_str(),
                sizePx, sizePx, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
                D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex);
        }
    }

    // Bundled slot 1-8: try the curated in-archive path first (so the
    // picker thumbnail reflects the real game texture / mod override),
    // then fall back to the RCDATA placeholder if FileManager can't
    // resolve it.
    if (FAILED(hr) && slot >= 1 && slot < Engine::kSkydomeBundledCount)
    {
        const char* const* gamePaths = Engine::GetSkydomeBundledGamePaths();
        if (gamePaths != NULL && gamePaths[slot] != NULL)
        {
            hr = LoadFromFileManagerBytes(gamePaths[slot]);
        }
    }
    if (FAILED(hr) && slot >= 1 && slot < Engine::kSkydomeBundledCount)
    {
        const int* ids = Engine::GetSkydomeBundledResources();
        int resId = ids[slot];
        if (resId != 0)
        {
            HMODULE hMod  = GetModuleHandle(NULL);
            HRSRC   hRes  = FindResource(hMod, MAKEINTRESOURCE(resId), RT_RCDATA);
            HGLOBAL hData = (hRes != NULL) ? LoadResource(hMod, hRes) : NULL;
            void*   pData = (hData != NULL) ? LockResource(hData)     : NULL;
            DWORD   dwSz  = (hRes  != NULL) ? SizeofResource(hMod, hRes) : 0;
            if (pData != NULL && dwSz > 0)
            {
                hr = D3DXCreateTextureFromFileInMemoryEx(
                    pDevice, pData, dwSz,
                    sizePx, sizePx, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
                    D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex);
            }
        }
    }

    HBITMAP hbm = NULL;
    if (SUCCEEDED(hr) && pTex != NULL)
        hbm = TextureToHBitmap(pTex);
    if (pTex != NULL) pTex->Release();

    if (hbm == NULL)
    {
        // Determine whether the slot has ANY configured source.
        const bool hasBundled =
            (slot >= 1 && slot < Engine::kSkydomeBundledCount);
        const bool slotHasSource = !customPath.empty() || hasBundled;
        hbm = MakePlaceholder(!slotHasSource);
    }
    return hbm;
}

// RebuildBackgroundPreviewBitmap — regenerates the 24×24 toolbar thumbnail
// from the current engine skydome slot and stores it in info->hBackgroundPreviewBitmap.
// Releases the previous bitmap before replacing (no double-free).
// Called at startup, after every slot change, and after Reset View Settings.
static void RebuildBackgroundPreviewBitmap(APPLICATION_INFO* info)
{
    if (info == NULL || info->engine == NULL) return;
    const int slot = info->engine->GetSkydomeSlot();
    // rework: when no skydome is active, the owner-draw paints a flat
    // colour swatch read directly from engine state. Keep the cached
    // thumbnail NULL in that case so the owner-draw's NULL-check selects
    // the swatch path and a stale thumbnail can't ghost through.
    if (slot == Engine::kSkydomeOffSlot)
    {
        if (info->hBackgroundPreviewBitmap != NULL)
        {
            DeleteObject(info->hBackgroundPreviewBitmap);
            info->hBackgroundPreviewBitmap = NULL;
        }
        if (info->hBackgroundBtn != NULL)
            InvalidateRect(info->hBackgroundBtn, NULL, TRUE);
        return;
    }
    HBITMAP hbmOld = info->hBackgroundPreviewBitmap;
    std::wstring customPath;
    if (slot >= Engine::kSkydomeFirstCustomSlot && slot < Engine::kSkydomeSlotCount)
        customPath = info->engine->GetSkydomeCustomPath(slot);
    info->hBackgroundPreviewBitmap = MakeSkydomeSlotThumbnail(
        info->engine->GetDevice(), slot, 24,
        customPath, info->engine->GetBackground(),
        info->fileManager);
    if (hbmOld != NULL) DeleteObject(hbmOld);
    if (info->hBackgroundBtn != NULL)
        InvalidateRect(info->hBackgroundBtn, NULL, TRUE);
}

// ---------------------------------------------------------------------------
// Skydome picker dialog state
// ---------------------------------------------------------------------------
struct SkydomePickerData
{
    APPLICATION_INFO* info;
    HIMAGELIST        hImageList;
    int               originalSlot;   // slot active at dialog open; for Cancel revert
};

// Slot display name — matches the ground picker's GroundSlotDisplayName pattern.
static std::wstring SkydomeSlotDisplayName(int slot, const std::wstring& customPath)
{
    if (slot < 0 || slot >= Engine::kSkydomeSlotCount) return L"";
    // Bundled range (0-8): use string table; Off and named skydomes.
    if (slot < Engine::kSkydomeBundledCount)
    {
        static const int kIds[Engine::kSkydomeBundledCount] = {
            IDS_SKYDOME_OFF,
            IDS_SKYDOME_SPACE,
            IDS_SKYDOME_ATMOSPHERE,
            IDS_SKYDOME_SUNSET,
            IDS_SKYDOME_DAWN,
            IDS_SKYDOME_NIGHT,
            IDS_SKYDOME_OVERCAST,
            IDS_SKYDOME_STUDIO,
            IDS_SKYDOME_INDOOR,
        };
        return LoadString(kIds[slot]);
    }
    // Custom slots (9-11).
    if (!customPath.empty())
    {
        size_t sep = customPath.find_last_of(L"\\/");
        return (sep == std::wstring::npos) ? customPath : customPath.substr(sep + 1);
    }
    return LoadString(IDS_SKYDOME_CUSTOM_BASE + (slot - Engine::kSkydomeFirstCustomSlot));
}

// Picker-specific thumbnail size — 192 px, matching the ground picker.
static const int kSkydomePickerThumbSize = 192;

// ListView subclass state for the skydome picker.
static WNDPROC s_skyLVOriginalProc = NULL;
static int     s_skyLVHoverIdx     = -1;

static void SkydomeLV_PaintAll(HWND hList, HDC hdcScreen, const RECT& rcClient);
static LRESULT CALLBACK SkydomeLVSubclassProc(HWND hList, UINT msg,
                                               WPARAM wParam, LPARAM lParam);

// Rebuild the skydome picker's ListView — image list, labels, selection.
static void SkydomePicker_RefreshList(HWND hDlg, SkydomePickerData* data)
{
    HWND hList = GetDlgItem(hDlg, IDC_SKYDOME_PICKER_LIST);
    int  prevSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(hList);
    if (data->hImageList != NULL) ImageList_Destroy(data->hImageList);

    data->hImageList = ImageList_Create(kSkydomePickerThumbSize,
                                         kSkydomePickerThumbSize,
                                         ILC_COLOR32, Engine::kSkydomeSlotCount, 0);
    ListView_SetImageList(hList, data->hImageList, LVSIL_NORMAL);

    for (int slot = 0; slot < Engine::kSkydomeSlotCount; ++slot)
    {
        std::wstring path;
        if (slot >= Engine::kSkydomeFirstCustomSlot)
            path = data->info->engine->GetSkydomeCustomPath(slot);
        HBITMAP hThumb = MakeSkydomeSlotThumbnail(
            data->info->engine->GetDevice(),
            slot, kSkydomePickerThumbSize, path,
            data->info->engine->GetBackground(),
            data->info->fileManager);
        int imgIdx = ImageList_Add(data->hImageList, hThumb, NULL);
        if (hThumb != NULL) DeleteObject(hThumb);

        std::wstring label = SkydomeSlotDisplayName(slot, path);
        LVITEM item = {};
        item.mask     = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem    = slot;
        item.iSubItem = 0;
        item.iImage   = imgIdx;
        item.lParam   = slot;
        item.pszText  = const_cast<LPWSTR>(label.c_str());
        ListView_InsertItem(hList, &item);
    }

    // Restore selection: prefer prevSel, else the engine's current slot.
    int sel = (prevSel >= 0) ? prevSel : data->info->engine->GetSkydomeSlot();
    if (sel >= 0 && sel < Engine::kSkydomeSlotCount)
    {
        ListView_SetItemState(hList, sel, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(hList, sel, FALSE);
    }
}

// rework: open ChooseColor for slot 0 (solid colour). Mirrors the
// ground-picker analog (GroundTexturePicker_PickSolidColor) but seeds
// the 16-slot custom palette from the ColorButton library's shared state
// (used by the Lighting dialog) so palette additions made here propagate
// to the other ColorButton instances and survive a restart via the
// WriteCustomColors registry write. Returns true if a colour was picked.
static bool BackgroundPicker_PickSolidColor(HWND hDlg, SkydomePickerData* data)
{
    COLORREF custom[16];
    ColorButton_GetCustomColors(custom);
    CHOOSECOLOR cc = {};
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = hDlg;
    cc.lpCustColors = custom;
    cc.rgbResult    = data->info->engine->GetBackground();
    cc.Flags        = CC_ANYCOLOR | CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColor(&cc)) return false;
    data->info->engine->SetBackground(cc.rgbResult);
    WriteBackgroundColor(cc.rgbResult);
    // Push the (possibly enlarged) custom palette back into the ColorButton
    // library so the Lighting dialog's colour fields see the new entries,
    // then persist so it survives an editor restart.
    ColorButton_SetCustomColors(custom);
    WriteCustomColors(custom);
    // Refresh the picker thumbnail for slot 0 (the swatch follows the
    // background colour) and the toolbar preview if we're currently in
    // solid-colour mode.
    SkydomePicker_RefreshList(hDlg, data);
    if (data->info->engine->GetSkydomeSlot() == Engine::kSkydomeOffSlot)
    {
        RebuildBackgroundPreviewBitmap(data->info);
        RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_UPDATENOW);
    }
    return true;
}

// File picker for a custom skydome slot. Returns true if a file was assigned.
static bool SkydomePicker_PickCustomFile(HWND hDlg, SkydomePickerData* data, int slot)
{
    if (slot < Engine::kSkydomeFirstCustomSlot || slot >= Engine::kSkydomeSlotCount)
        return false;
    wchar_t buf[1024] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hDlg;
    ofn.lpstrFilter = L"Texture Files (*.dds;*.tga;*.bmp;*.png;*.jpg;*.jpeg)\0*.dds;*.tga;*.bmp;*.png;*.jpg;*.jpeg\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle  = L"Choose Skydome Texture File";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    std::wstring path = buf;
    data->info->engine->SetSkydomeCustomPath(slot, path);
    WriteSkydomeCustomPath(slot, path);
    SkydomePicker_RefreshList(hDlg, data);
    if (data->info->engine->GetSkydomeSlot() == slot)
    {
        RebuildBackgroundPreviewBitmap(data->info);
        RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_UPDATENOW);
    }
    return true;
}

static INT_PTR CALLBACK SkydomePickerProc(HWND hDlg, UINT uMsg,
                                           WPARAM wParam, LPARAM lParam)
{
    SkydomePickerData* data =
        (SkydomePickerData*)(LONG_PTR)GetWindowLongPtr(hDlg, DWLP_USER);
    switch (uMsg)
    {
    case WM_INITDIALOG:
    {
        data = (SkydomePickerData*)lParam;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)data);
        data->hImageList = NULL;
        HWND hList = GetDlgItem(hDlg, IDC_SKYDOME_PICKER_LIST);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_DOUBLEBUFFER);
        // 192×192 thumb, 208×232 spacing — matches the ground picker
        // so the two dialogs look identical.
        ListView_SetIconSpacing(hList, 208, 232);
        // Subclass for full custom paint.
        if (s_skyLVOriginalProc == NULL)
        {
            s_skyLVOriginalProc = (WNDPROC)SetWindowLongPtr(
                hList, GWLP_WNDPROC, (LONG_PTR)SkydomeLVSubclassProc);
        }
        else
        {
            SetWindowLongPtr(hList, GWLP_WNDPROC, (LONG_PTR)SkydomeLVSubclassProc);
        }
        s_skyLVHoverIdx = -1;
        // Hide the path label — slot labels already tell the story.
        HWND hPath = GetDlgItem(hDlg, IDC_SKYDOME_PICKER_PATH_LABEL);
        if (hPath != NULL) ShowWindow(hPath, SW_HIDE);
        SkydomePicker_RefreshList(hDlg, data);
        return TRUE;
    }
    case WM_USER:
        // Re-seed from engine state (e.g. after Reset View Settings).
        if (data != NULL)
        {
            HWND hList = GetDlgItem(hDlg, IDC_SKYDOME_PICKER_LIST);
            int slot = data->info->engine->GetSkydomeSlot();
            if (slot >= 0 && slot < Engine::kSkydomeSlotCount)
            {
                ListView_SetItemState(hList, slot,
                    LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(hList, slot, FALSE);
            }
            SkydomePicker_RefreshList(hDlg, data);
        }
        return TRUE;
    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->idFrom != IDC_SKYDOME_PICKER_LIST) break;

        if (hdr->code == NM_CUSTOMDRAW)
        {
            // Delegate to our subclassed WM_PAINT; suppress native draw.
            LPNMLVCUSTOMDRAW lpc = (LPNMLVCUSTOMDRAW)lParam;
            if (lpc->nmcd.dwDrawStage == CDDS_PREPAINT)
            {
                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                return TRUE;
            }
            if (lpc->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
            {
                SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
            SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
            return TRUE;
        }

        if (hdr->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* nlv = (NMLISTVIEW*)lParam;
            if ((nlv->uNewState & LVIS_SELECTED) && !(nlv->uOldState & LVIS_SELECTED))
            {
                int slot = nlv->iItem;
                if (slot >= 0 && slot < Engine::kSkydomeSlotCount &&
                    !data->info->engine->IsSkydomeSlotEmpty(slot))
                {
                    data->info->engine->SetSkydomeSlot(slot);
                    WriteSkydomeIndex(slot);
                    RebuildBackgroundPreviewBitmap(data->info);
                    RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                                 RDW_INVALIDATE | RDW_UPDATENOW);
                }
            }
        }
        else if (hdr->code == NM_CLICK || hdr->code == NM_DBLCLK)
        {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
            int slot = nia->iItem;
            if (slot < 0 || slot >= Engine::kSkydomeSlotCount) break;
            // rework: slot 0 is the unified "Solid colour" entry.
            // LVN_ITEMCHANGED already committed SkydomeSlot=0; clicking
            // additionally opens ChooseColor. Cancel keeps the existing
            // background colour (BackgroundPicker_PickSolidColor returns
            // false without touching engine state).
            if (slot == Engine::kSkydomeOffSlot)
            {
                BackgroundPicker_PickSolidColor(hDlg, data);
                break;
            }
            // Empty custom slot: single click opens file picker.
            if (data->info->engine->IsSkydomeSlotEmpty(slot))
            {
                SkydomePicker_PickCustomFile(hDlg, data, slot);
                if (!data->info->engine->IsSkydomeSlotEmpty(slot))
                {
                    HWND hList = GetDlgItem(hDlg, IDC_SKYDOME_PICKER_LIST);
                    ListView_SetItemState(hList, slot,
                        LVIS_SELECTED | LVIS_FOCUSED,
                        LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
            // rework: sticky picker. Populated slots commit via
            // LVN_ITEMCHANGED above; the dialog stays open so the user
            // can browse other skydomes interactively. Close via the
            // window X button or by toggling the Background button.
        }
        else if (hdr->code == NM_RCLICK)
        {
            NMITEMACTIVATE* nia = (NMITEMACTIVATE*)lParam;
            int slot = nia->iItem;
            if (slot < 0 || slot >= Engine::kSkydomeSlotCount) break;
            // Only custom slots get a context menu.
            if (slot < Engine::kSkydomeFirstCustomSlot) break;
            const std::wstring& path = data->info->engine->GetSkydomeCustomPath(slot);
            const bool hasCustom = !path.empty();
            HMENU hMenu = CreatePopupMenu();
            if (hasCustom)
            {
                AppendMenuW(hMenu, MF_STRING, ID_SKYDOME_SLOT_CHANGE_CUSTOM,
                            L"Change skydome...");
                AppendMenuW(hMenu, MF_STRING, ID_SKYDOME_SLOT_CLEAR_CUSTOM,
                            L"Clear slot");
            }
            else
            {
                AppendMenuW(hMenu, MF_STRING, ID_SKYDOME_SLOT_SET_CUSTOM,
                            L"Set custom skydome...");
            }
            POINT pt; GetCursorPos(&pt);
            INT cmd = TrackPopupMenu(hMenu,
                                      TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RETURNCMD | TPM_NONOTIFY,
                                      pt.x, pt.y, 0, hDlg, NULL);
            DestroyMenu(hMenu);
            if (cmd == ID_SKYDOME_SLOT_SET_CUSTOM ||
                cmd == ID_SKYDOME_SLOT_CHANGE_CUSTOM)
            {
                SkydomePicker_PickCustomFile(hDlg, data, slot);
            }
            else if (cmd == ID_SKYDOME_SLOT_CLEAR_CUSTOM)
            {
                data->info->engine->SetSkydomeCustomPath(slot, L"");
                WriteSkydomeCustomPath(slot, L"");
                // If the active slot was cleared, fall back to Off.
                if (data->info->engine->GetSkydomeSlot() == slot)
                {
                    data->info->engine->SetSkydomeSlot(Engine::kSkydomeOffSlot);
                    WriteSkydomeIndex(Engine::kSkydomeOffSlot);
                }
                SkydomePicker_RefreshList(hDlg, data);
                RebuildBackgroundPreviewBitmap(data->info);
                RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                             RDW_INVALIDATE | RDW_UPDATENOW);
            }
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SKYDOME_PICKER_RESET_CUSTOM:
        {
            if (MessageBox(hDlg,
                           L"Reset all custom skydome slots to empty? "
                           L"Custom file assignments will be lost — "
                           L"but the files themselves are not deleted.",
                           L"Reset Custom Slots",
                           MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
            // Wipe all 3 custom slots.
            for (int s = Engine::kSkydomeFirstCustomSlot;
                 s < Engine::kSkydomeSlotCount; ++s)
            {
                data->info->engine->SetSkydomeCustomPath(s, L"");
                WriteSkydomeCustomPath(s, L"");
            }
            // If the active slot was a custom, fall back to Off.
            int cur = data->info->engine->GetSkydomeSlot();
            if (cur >= Engine::kSkydomeFirstCustomSlot)
            {
                data->info->engine->SetSkydomeSlot(Engine::kSkydomeOffSlot);
                WriteSkydomeIndex(Engine::kSkydomeOffSlot);
            }
            SkydomePicker_RefreshList(hDlg, data);
            RebuildBackgroundPreviewBitmap(data->info);
            RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                         RDW_INVALIDATE | RDW_UPDATENOW);
            return TRUE;
        }
        case IDOK:
        {
            RECT r; GetWindowRect(hDlg, &r);
            WriteSkydomePickerPos(r);
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        case IDCANCEL:
        {
            // Revert slot selection; slot-path mutations stay.
            if (data->info->engine->GetSkydomeSlot() != data->originalSlot)
            {
                data->info->engine->SetSkydomeSlot(data->originalSlot);
                WriteSkydomeIndex(data->originalSlot);
                RebuildBackgroundPreviewBitmap(data->info);
                RedrawWindow(data->info->hRenderWnd, NULL, NULL,
                             RDW_INVALIDATE | RDW_UPDATENOW);
            }
            RECT r; GetWindowRect(hDlg, &r);
            WriteSkydomePickerPos(r);
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        }
        break;
    case WM_CLOSE:
    {
        RECT r; GetWindowRect(hDlg, &r);
        WriteSkydomePickerPos(r);
        ShowWindow(hDlg, SW_HIDE);
        return TRUE;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            RECT r; GetWindowRect(hDlg, &r);
            WriteSkydomePickerPos(r);
            ShowWindow(hDlg, SW_HIDE);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (data != NULL && data->hImageList != NULL)
        {
            ImageList_Destroy(data->hImageList);
            data->hImageList = NULL;
        }
        break;
    }
    return FALSE;
}

// Off-screen paint for the skydome picker's ListView — mirrors
// GroundLV_PaintAll so the two dialogs look visually identical.
static void SkydomeLV_PaintAll(HWND hList, HDC hdcScreen, const RECT& rcClient)
{
    HWND hDlg = GetParent(hList);
    SkydomePickerData* data =
        (SkydomePickerData*)(LONG_PTR)GetWindowLongPtr(hDlg, DWLP_USER);

    HDC     hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen,
                                            rcClient.right, rcClient.bottom);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, hbmMem);
    HDC     hdc = hdcMem;

    HBRUSH bgBrush = CreateSolidBrush(RGB(240, 240, 240));
    FillRect(hdc, &rcClient, bgBrush);
    DeleteObject(bgBrush);

    if (data == NULL || data->info == NULL || data->info->engine == NULL)
    {
        BitBlt(hdcScreen, 0, 0, rcClient.right, rcClient.bottom,
               hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hOldBmp);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);
        return;
    }

    HFONT hFont = (HFONT)SendMessage(hDlg, WM_GETFONT, 0, 0);
    if (hFont == NULL) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    const int currentSlot = data->info->engine->GetSkydomeSlot();
    const int itemCount   = ListView_GetItemCount(hList);
    HIMAGELIST hIL        = ListView_GetImageList(hList, LVSIL_NORMAL);

    for (int i = 0; i < itemCount; ++i)
    {
        RECT bounds, iconRc, labelRc;
        ListView_GetItemRect(hList, i, &bounds,  LVIR_BOUNDS);
        ListView_GetItemRect(hList, i, &iconRc,  LVIR_ICON);
        ListView_GetItemRect(hList, i, &labelRc, LVIR_LABEL);

        const bool hovered  = (s_skyLVHoverIdx == i);
        const bool selected = (i == currentSlot);

        const COLORREF bgCol = hovered ? RGB(160, 200, 250) : RGB(240, 240, 240);
        HBRUSH bg = CreateSolidBrush(bgCol);
        FillRect(hdc, &bounds, bg);
        DeleteObject(bg);

        if (hIL != NULL)
        {
            LVITEMW item = {};
            item.mask  = LVIF_IMAGE;
            item.iItem = i;
            ListView_GetItem(hList, &item);
            if (item.iImage >= 0)
            {
                IMAGEINFO ii = {};
                ImageList_GetImageInfo(hIL, item.iImage, &ii);
                const int imgW = ii.rcImage.right  - ii.rcImage.left;
                const int imgH = ii.rcImage.bottom - ii.rcImage.top;
                const int ix = iconRc.left + (iconRc.right  - iconRc.left - imgW) / 2;
                const int iy = iconRc.top  + (iconRc.bottom - iconRc.top  - imgH) / 2;
                ImageList_Draw(hIL, item.iImage, hdc, ix, iy, ILD_NORMAL);
            }
        }

        HPEN pen;
        if (selected)
            pen = CreatePen(PS_SOLID, 2, RGB(40, 100, 220));
        else if (hovered)
            pen = CreatePen(PS_SOLID, 3, RGB(70, 150, 240));
        else
            pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
        HGDIOBJ oldP = SelectObject(hdc, pen);
        HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc,
                  iconRc.left + 1, iconRc.top + 1,
                  iconRc.right - 1, iconRc.bottom - 1);
        SelectObject(hdc, oldP);
        SelectObject(hdc, oldB);
        DeleteObject(pen);

        WCHAR labelBuf[256] = L"";
        ListView_GetItemText(hList, i, 0, labelBuf, (int)_countof(labelBuf));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(40, 40, 40));
        DrawTextW(hdc, labelBuf, -1, &labelRc,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER
                  | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    SelectObject(hdc, hOldFont);
    BitBlt(hdcScreen, 0, 0, rcClient.right, rcClient.bottom, hdcMem, 0, 0, SRCCOPY);
    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
}

static LRESULT CALLBACK SkydomeLVSubclassProc(HWND hList, UINT msg,
                                               WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hList, &ps);
        RECT rcClient;
        GetClientRect(hList, &rcClient);
        SkydomeLV_PaintAll(hList, hdc, rcClient);
        EndPaint(hList, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT cur = { (LONG)(short)LOWORD(lParam), (LONG)(short)HIWORD(lParam) };
        LVHITTESTINFO ht = {};
        ht.pt = cur;
        const int newHot = ListView_HitTest(hList, &ht);
        if (newHot != s_skyLVHoverIdx)
        {
            s_skyLVHoverIdx = newHot;
            InvalidateRect(hList, NULL, FALSE);
            TRACKMOUSEEVENT tme = {};
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hList;
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (s_skyLVHoverIdx != -1)
        {
            s_skyLVHoverIdx = -1;
            InvalidateRect(hList, NULL, FALSE);
        }
        break;
    }
    return CallWindowProc(s_skyLVOriginalProc, hList, msg, wParam, lParam);
}

// Modeless skydome picker. Lazy-create on first toggle; subsequent calls
// show/hide. Restores saved window position from registry.
static void ShowSkydomePicker(HWND hParent, APPLICATION_INFO* info)
{
    if (info == NULL || info->engine == NULL) return;
    static HWND              s_hPicker = NULL;
    static SkydomePickerData s_data;
    s_data.info         = info;
    s_data.hImageList   = NULL;
    s_data.originalSlot = info->engine->GetSkydomeSlot();

    if (s_hPicker != NULL && IsWindowVisible(s_hPicker))
    {
        // Already visible — toggle hide.
        RECT r; GetWindowRect(s_hPicker, &r);
        WriteSkydomePickerPos(r);
        ShowWindow(s_hPicker, SW_HIDE);
        return;
    }

    if (s_hPicker == NULL)
    {
        s_hPicker = CreateDialogParamW(GetModuleHandle(NULL),
                                        MAKEINTRESOURCEW(IDD_SKYDOME_PICKER),
                                        hParent, SkydomePickerProc,
                                        (LPARAM)&s_data);
        if (s_hPicker == NULL) return;
        info->hSkydomePicker = s_hPicker;
        // Slim tool-window title bar — applied after creation because
        // WS_EX_TOOLWINDOW isn't in the dialog template EXSTYLE.
        LONG_PTR ex = GetWindowLongPtr(s_hPicker, GWL_EXSTYLE);
        SetWindowLongPtr(s_hPicker, GWL_EXSTYLE, ex | WS_EX_TOOLWINDOW);
        SetWindowPos(s_hPicker, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        // Restore saved position if any; validate that it's on-screen.
        RECT pos;
        if (ReadSkydomePickerPos(pos))
        {
            POINT pt = { pos.left, pos.top };
            if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) != NULL)
            {
                SetWindowPos(s_hPicker, NULL, pos.left, pos.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    else
    {
        // Existing hidden instance — refresh in case engine state changed.
        SkydomePicker_RefreshList(s_hPicker, &s_data);
    }
    ShowWindow(s_hPicker, SW_SHOW);
    SetForegroundWindow(s_hPicker);
}

// Bloom config persistence. Master enable as DWORD (matches ShowGround
// pattern); strength / cutoff / size as REG_BINARY floats sharing a
// single helper since they all behave the same. NaN / Inf rejected
// so a corrupt blob can't drive bloom into a silly state.
static bool ReadBloomEnabled(bool defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value, type, size = sizeof(value);
        if (RegQueryValueEx(hKey, L"BloomEnabled", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        {
            RegCloseKey(hKey);
            return value != 0;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteBloomEnabled(bool enabled)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD value = enabled ? 1 : 0;
        RegSetValueEx(hKey, L"BloomEnabled", 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
    }
}

static float ReadBloomFloat(const wchar_t* name, float defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        float value;
        DWORD type, size = sizeof(value);
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == sizeof(value) && std::isfinite(value))
        {
            RegCloseKey(hKey);
            return value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteBloomFloat(const wchar_t* name, float value)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hKey, name, 0, REG_BINARY, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
    }
}

// Skydome state persistence (). Index as DWORD, custom slot paths as
// REG_SZ, picker dialog position as RECT (REG_BINARY).
static int ReadSkydomeIndex(int defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value, type, size = sizeof(value);
        if (RegQueryValueEx(hKey, L"SkydomeIndex", NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS
            && type == REG_DWORD && (int)value >= 0 && (int)value < Engine::kSkydomeSlotCount)
        {
            RegCloseKey(hKey);
            return (int)value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteSkydomeIndex(int value)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD v = (DWORD)value;
        RegSetValueEx(hKey, L"SkydomeIndex", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(hKey);
    }
}

// Custom slot paths use names SkydomeCustomSlot9, SkydomeCustomSlot10, SkydomeCustomSlot11
static std::wstring ReadSkydomeCustomPath(int slot)
{
    if (slot < Engine::kSkydomeFirstCustomSlot || slot >= Engine::kSkydomeSlotCount) return L"";
    HKEY hKey;
    std::wstring out;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t buf[MAX_PATH];
        DWORD size = sizeof(buf);
        DWORD type;
        wchar_t name[64];
        swprintf_s(name, L"SkydomeCustomSlot%d", slot);
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
        {
            out = buf;
        }
        RegCloseKey(hKey);
    }
    return out;
}

static void WriteSkydomeCustomPath(int slot, const std::wstring& path)
{
    if (slot < Engine::kSkydomeFirstCustomSlot || slot >= Engine::kSkydomeSlotCount) return;
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        wchar_t name[64];
        swprintf_s(name, L"SkydomeCustomSlot%d", slot);
        if (path.empty())
        {
            RegDeleteValue(hKey, name);
        }
        else
        {
            RegSetValueEx(hKey, name, 0, REG_SZ, (const BYTE*)path.c_str(),
                          DWORD((path.size() + 1) * sizeof(wchar_t)));
        }
        RegCloseKey(hKey);
    }
}

static bool ReadSkydomePickerPos(RECT& out)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type, size = sizeof(out);
        if (RegQueryValueEx(hKey, L"SkydomePickerPos", NULL, &type, (LPBYTE)&out, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == sizeof(out))
        {
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

static void WriteSkydomePickerPos(const RECT& in)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hKey, L"SkydomePickerPos", 0, REG_BINARY, (const BYTE*)&in, sizeof(in));
        RegCloseKey(hKey);
    }
}

// `out` must point to a 16-element COLORREF buffer. On miss, leaves the
// buffer untouched and returns false so the caller can decide whether
// to seed defaults.
static bool ReadCustomColors(COLORREF out[16])
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type, size = 16 * sizeof(COLORREF);
        if (RegQueryValueEx(hKey, L"CustomColors", NULL, &type, (LPBYTE)out, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == 16 * sizeof(COLORREF))
        {
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

static void WriteCustomColors(const COLORREF in[16])
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hKey, L"CustomColors", 0, REG_BINARY, (const BYTE*)in, 16 * sizeof(COLORREF));
        RegCloseKey(hKey);
    }
}

// Drops all view-settings keys. Used by View → Reset View Settings.
// Missing values are not an error — silently skipped.
static void ResetViewSettings()
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        RegDeleteValue(hKey, L"BackgroundColor");
        RegDeleteValue(hKey, L"ShowGround");
        RegDeleteValue(hKey, L"GroundZ");
        RegDeleteValue(hKey, L"GroundTexture");   //
        RegDeleteValue(hKey, L"BloomEnabled");
        RegDeleteValue(hKey, L"BloomStrength");
        RegDeleteValue(hKey, L"BloomCutoff");
        RegDeleteValue(hKey, L"BloomSize");
        RegDeleteValue(hKey, L"BloomDialogPos");
        RegDeleteValue(hKey, L"CustomColors");
        RegDeleteValue(hKey, L"SpawnerConfig");
        RegDeleteValue(hKey, L"SpawnerDialogPos");
        RegDeleteValue(hKey, L"SkydomeIndex");    //
        RegDeleteValue(hKey, L"SkydomePickerPos");
        // NOTE: SkydomeCustomSlot* paths are user data, not view settings — NOT cleared here.
        RegCloseKey(hKey);
    }
}

// Spawner config is intentionally session-only (defaults restored on
// every launch). Only the dialog window position survives across
// sessions — see Read/WriteSpawnerDialogPos. ResetViewSettings still
// drops a stale SpawnerConfig REG_BINARY value left behind by an
// earlier build that did persist it.
static bool ReadSpawnerDialogPos(RECT& out)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type, size = sizeof(out);
        if (RegQueryValueEx(hKey, L"SpawnerDialogPos", NULL, &type, (LPBYTE)&out, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == sizeof(out))
        {
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

static void WriteSpawnerDialogPos(const RECT& in)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hKey, L"SpawnerDialogPos", 0, REG_BINARY, (const BYTE*)&in, sizeof(in));
        RegCloseKey(hKey);
    }
}

static bool ReadBloomDialogPos(RECT& out)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type, size = sizeof(out);
        if (RegQueryValueEx(hKey, L"BloomDialogPos", NULL, &type, (LPBYTE)&out, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == sizeof(out))
        {
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

// ============================================================================
// Spawner dialog
// ============================================================================

// Helper to configure a Spinner control with float range / value.
static void ConfigureFloatSpinner(HWND hDlg, int ctrlId, float minV, float maxV, float incr, float value)
{
    SPINNER_INFO si = {0};
    si.IsFloat = true;
    si.Mask    = SPIF_ALL;
    si.f.MinValue  = minV;
    si.f.MaxValue  = maxV;
    si.f.Increment = incr;
    si.f.Value     = value;
    Spinner_SetInfo(GetDlgItem(hDlg, ctrlId), &si);
}

static float GetFloatSpinner(HWND hDlg, int ctrlId)
{
    SPINNER_INFO si = {0};
    si.Mask = SPIF_VALUE;
    Spinner_GetInfo(GetDlgItem(hDlg, ctrlId), &si);
    return si.f.Value;
}

// Show / hide the controls that are mutually exclusive between Manual
// and Auto modes. Called whenever the mode changes (and once at
// dialog init).
static void SpawnerDlg_SyncModeVisibility(HWND hDlg, SpawnerConfig::Mode mode)
{
    bool isManual = (mode == SpawnerConfig::Mode::Manual);

    // Manual-only controls.
    ShowWindow(GetDlgItem(hDlg, IDC_SPAWNER_TRIGGER_BTN), isManual ? SW_SHOW : SW_HIDE);

    // Auto-only controls.
    int autoShow = isManual ? SW_HIDE : SW_SHOW;
    ShowWindow(GetDlgItem(hDlg, IDC_SPAWNER_ENABLE),         autoShow);
    ShowWindow(GetDlgItem(hDlg, IDC_SPAWNER_INTERVAL),       autoShow);
    ShowWindow(GetDlgItem(hDlg, IDC_SPAWNER_INTERVAL_LABEL), autoShow);
    ShowWindow(GetDlgItem(hDlg, IDC_SPAWNER_BURSTS_PER_SEC), autoShow);
}

// Update the read-only "Bursts/s: X.X" label in Auto mode.
// Update the read-only "Bursts/s: X.X" label in Auto mode. Math lives
// in SpawnerDriver::ComputeBurstsPerSec — single source of truth.
static void SpawnerDlg_UpdateBurstsPerSec(HWND hDlg, const SpawnerConfig& cfg)
{
    if (cfg.mode != SpawnerConfig::Mode::Auto) return;
    wchar_t buf[64];
    swprintf_s(buf, 64, L"Bursts/s: %.2f", SpawnerDriver::ComputeBurstsPerSec(cfg));
    SetDlgItemText(hDlg, IDC_SPAWNER_BURSTS_PER_SEC, buf);
}

// Push a SpawnerConfig into all the dialog controls.
static void SpawnerDlg_LoadFromConfig(HWND hDlg, const SpawnerConfig& cfg)
{
    CheckDlgButton(hDlg, IDC_SPAWNER_MODE_MANUAL, cfg.mode == SpawnerConfig::Mode::Manual ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_SPAWNER_MODE_AUTO,   cfg.mode == SpawnerConfig::Mode::Auto   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_SPAWNER_ENABLE,      cfg.enabled                              ? BST_CHECKED : BST_UNCHECKED);

    // Burst spinners.
    {
        SPINNER_INFO si = {0};
        si.IsFloat     = false;
        si.Mask        = SPIF_ALL;
        si.i.MinValue  = 1;
        si.i.MaxValue  = SpawnerDriver::MAX_BURST_SIZE;
        si.i.Increment = 1;
        si.i.Value     = cfg.burstSize;
        Spinner_SetInfo(GetDlgItem(hDlg, IDC_SPAWNER_BURST_SIZE), &si);
    }
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_SPACING,  0.0f, SpawnerDriver::MAX_SPACING_SEC,  0.05f, cfg.spacingSec);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_INTERVAL, 0.0f, SpawnerDriver::MAX_INTERVAL_SEC, 0.1f,  cfg.intervalSec);

    const float posLim = SpawnerDriver::JITTER_MAX;
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_POS_X, -posLim, posLim, 1.0f, cfg.position.x);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_POS_Y, -posLim, posLim, 1.0f, cfg.position.y);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_POS_Z, -posLim, posLim, 1.0f, cfg.position.z);

    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_VEL_X, -posLim, posLim, 1.0f, cfg.velocity.x);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_VEL_Y, -posLim, posLim, 1.0f, cfg.velocity.y);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_VEL_Z, -posLim, posLim, 1.0f, cfg.velocity.z);

    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_LIFETIME, 0.0f, SpawnerDriver::MAX_LIFETIME_SEC, 0.5f, cfg.maxLifetimeSec);

    //: legacy dialog exposes spawn-point jitter only. Path-shaping
    // (arc / squiggle) is React-UI-only; legacy is slated for removal
    // (), so the new path controls aren't wired here.
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_X, 0.0f, posLim, 0.5f, cfg.jitterPosition.x);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_Y, 0.0f, posLim, 0.5f, cfg.jitterPosition.y);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_Z, 0.0f, posLim, 0.5f, cfg.jitterPosition.z);

    SpawnerDlg_SyncModeVisibility(hDlg, cfg.mode);
    SpawnerDlg_UpdateBurstsPerSec(hDlg, cfg);
}

// Pull the current dialog control values into a SpawnerConfig.
static SpawnerConfig SpawnerDlg_ReadIntoConfig(HWND hDlg)
{
    SpawnerConfig cfg;
    cfg.mode    = (IsDlgButtonChecked(hDlg, IDC_SPAWNER_MODE_MANUAL) == BST_CHECKED)
                  ? SpawnerConfig::Mode::Manual
                  : SpawnerConfig::Mode::Auto;
    cfg.enabled = (IsDlgButtonChecked(hDlg, IDC_SPAWNER_ENABLE) == BST_CHECKED);

    {
        SPINNER_INFO si = {0};
        si.Mask = SPIF_VALUE;
        Spinner_GetInfo(GetDlgItem(hDlg, IDC_SPAWNER_BURST_SIZE), &si);
        cfg.burstSize = si.i.Value;
    }
    cfg.spacingSec  = GetFloatSpinner(hDlg, IDC_SPAWNER_SPACING);
    cfg.intervalSec = GetFloatSpinner(hDlg, IDC_SPAWNER_INTERVAL);

    cfg.position     = D3DXVECTOR3(GetFloatSpinner(hDlg, IDC_SPAWNER_POS_X),
                                   GetFloatSpinner(hDlg, IDC_SPAWNER_POS_Y),
                                   GetFloatSpinner(hDlg, IDC_SPAWNER_POS_Z));
    cfg.velocity     = D3DXVECTOR3(GetFloatSpinner(hDlg, IDC_SPAWNER_VEL_X),
                                   GetFloatSpinner(hDlg, IDC_SPAWNER_VEL_Y),
                                   GetFloatSpinner(hDlg, IDC_SPAWNER_VEL_Z));
    cfg.maxLifetimeSec = GetFloatSpinner(hDlg, IDC_SPAWNER_LIFETIME);
    cfg.jitterPosition = D3DXVECTOR3(GetFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_X),
                                     GetFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_Y),
                                     GetFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_Z));
    //: acceleration/squiggle path-shaping not editable in legacy.
    ClampSpawnerConfig(cfg);
    return cfg;
}

static INT_PTR CALLBACK SpawnerDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    APPLICATION_INFO* info = (APPLICATION_INFO*)(LONG_PTR)GetWindowLongPtr(hDlg, GWLP_USERDATA);

    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            info = (APPLICATION_INFO*)lParam;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)info);
            if (info != NULL && info->spawner != NULL)
            {
                SpawnerDlg_LoadFromConfig(hDlg, info->spawner->GetConfig());
            }
            return TRUE;
        }

        case WM_COMMAND:
        case WM_NOTIFY:
        {
            // Spawn-now button → fire a manual burst without re-reading
            // the whole config (the button click isn't itself a config
            // change).
            if (uMsg == WM_COMMAND && LOWORD(wParam) == IDC_SPAWNER_TRIGGER_BTN
                && info != NULL && info->spawner != NULL)
            {
                info->spawner->Trigger(info->particleSystem, info->engine);
                return TRUE;
            }

            // Any other control change → re-read all values, push to
            // driver. Spawner config is session-only; not written to
            // registry. SN_CHANGE comes through as WM_COMMAND from
            // the Spinner control; checkboxes / radios fire BN_CLICKED.
            if (info != NULL && info->spawner != NULL)
            {
                SpawnerConfig cfg = SpawnerDlg_ReadIntoConfig(hDlg);
                info->spawner->SetConfig(cfg);

                // Mode-radio toggles need a visibility sync because the
                // Manual / Auto control sets are mutually exclusive.
                if (uMsg == WM_COMMAND
                    && (LOWORD(wParam) == IDC_SPAWNER_MODE_MANUAL
                        || LOWORD(wParam) == IDC_SPAWNER_MODE_AUTO))
                {
                    SpawnerDlg_SyncModeVisibility(hDlg, cfg.mode);
                }
                SpawnerDlg_UpdateBurstsPerSec(hDlg, cfg);
            }
            return FALSE;
        }

        case WM_CLOSE:
        {
            // Hide rather than destroy. State (focus, in-progress edits)
            // survives a hide/show cycle.
            if (info != NULL)
            {
                GetWindowRect(hDlg, &info->spawnerWindowRect);
                WriteSpawnerDialogPos(info->spawnerWindowRect);
                ShowWindow(hDlg, SW_HIDE);
                info->spawnerVisible = false;
                CheckMenuItem(GetMenu(info->hMainWnd), ID_EMITTER_SPAWNER, MF_BYCOMMAND | MF_UNCHECKED);
            }
            return TRUE;
        }
    }
    return FALSE;
}

static void ToggleSpawnerDialog(APPLICATION_INFO* info)
{
    if (info == NULL || info->hMainWnd == NULL) return;

    if (info->hSpawnerDlg == NULL)
    {
        // Lazy create on first request.
        info->hSpawnerDlg = CreateDialogParam(
            info->hInstance,
            MAKEINTRESOURCE(IDD_SPAWNER),
            info->hMainWnd,
            SpawnerDlgProc,
            (LPARAM)info);

        if (info->hSpawnerDlg == NULL) return;

        // Try to restore prior position. Validate against virtual screen
        // bounds — if the saved RECT is fully off-screen (e.g. monitor
        // disconnected), fall back to system default placement.
        if (info->spawnerWindowRect.right > info->spawnerWindowRect.left)
        {
            RECT r = info->spawnerWindowRect;
            HMONITOR hm = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
            if (hm != NULL)
            {
                SetWindowPos(info->hSpawnerDlg, NULL, r.left, r.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    if (info->spawnerVisible)
    {
        // Visible → hide. Capture position first so re-show restores it.
        GetWindowRect(info->hSpawnerDlg, &info->spawnerWindowRect);
        WriteSpawnerDialogPos(info->spawnerWindowRect);
        ShowWindow(info->hSpawnerDlg, SW_HIDE);
        info->spawnerVisible = false;
    }
    else
    {
        ShowWindow(info->hSpawnerDlg, SW_SHOW);
        SetForegroundWindow(info->hSpawnerDlg);
        info->spawnerVisible = true;
    }

    CheckMenuItem(GetMenu(info->hMainWnd), ID_EMITTER_SPAWNER,
                  MF_BYCOMMAND | (info->spawnerVisible ? MF_CHECKED : MF_UNCHECKED));
}

//
// Bloom config dialog. Modeless toggle pattern, same skeleton as the
// Spawner dialog. The dialog reads engine state on open and pushes
// every control change live; values persist to the registry on each
// edit so closing the editor mid-drag never loses configuration.
//

// Configure a bloom spinner with the standard 0.05 step.
static void ConfigureBloomSpinner(HWND hDlg, int id, float lo, float hi, float value)
{
    SPINNER_INFO si = {0};
    si.Mask        = SPIF_ALL;
    si.IsFloat     = true;
    si.f.MinValue  = lo;
    si.f.MaxValue  = hi;
    si.f.Increment = 0.05f;
    si.f.Value     = value;
    Spinner_SetInfo(GetDlgItem(hDlg, id), &si);
}

// Re-seed all bloom dialog controls from the engine's current state.
// Used at WM_INITDIALOG and after Reset View Settings via a WM_USER
// nudge from the main window.
static void BloomDlg_Load(HWND hDlg, APPLICATION_INFO* info)
{
    if (info == NULL || info->engine == NULL) return;

    CheckDlgButton(hDlg, IDC_BLOOM_ENABLE,
                   info->engine->GetBloom() ? BST_CHECKED : BST_UNCHECKED);
    ConfigureBloomSpinner(hDlg, IDC_BLOOM_STRENGTH, 0.0f, 1.0f, info->engine->GetBloomStrength());
    ConfigureBloomSpinner(hDlg, IDC_BLOOM_CUTOFF,   0.0f, 2.0f, info->engine->GetBloomCutoff());
    ConfigureBloomSpinner(hDlg, IDC_BLOOM_SIZE,     0.0f, 2.0f, info->engine->GetBloomSize());

    // Disable controls when the bloom shader isn't actually usable
    // (file missing / parameters don't match). The dialog still
    // opens so the user can see *why* nothing's happening.
    BOOL avail = info->engine->IsBloomAvailable() ? TRUE : FALSE;
    EnableWindow(GetDlgItem(hDlg, IDC_BLOOM_ENABLE),   avail);
    EnableWindow(GetDlgItem(hDlg, IDC_BLOOM_STRENGTH), avail);
    EnableWindow(GetDlgItem(hDlg, IDC_BLOOM_CUTOFF),   avail);
    EnableWindow(GetDlgItem(hDlg, IDC_BLOOM_SIZE),     avail);
}

static INT_PTR CALLBACK BloomDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    APPLICATION_INFO* info = (APPLICATION_INFO*)(LONG_PTR)GetWindowLongPtr(hDlg, GWLP_USERDATA);

    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            info = (APPLICATION_INFO*)lParam;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)info);
            BloomDlg_Load(hDlg, info);
            return TRUE;
        }

        // Reset View Settings posts WM_USER to re-seed our controls
        // from the engine after the engine values have changed.
        case WM_USER:
            BloomDlg_Load(hDlg, info);
            return TRUE;

        case WM_COMMAND:
        {
            if (info == NULL || info->engine == NULL) return FALSE;
            const WORD code = HIWORD(wParam);
            const WORD id   = LOWORD(wParam);

            if (code == BN_CLICKED && id == IDC_BLOOM_ENABLE)
            {
                bool enabled = (IsDlgButtonChecked(hDlg, IDC_BLOOM_ENABLE) == BST_CHECKED);
                info->engine->SetBloom(enabled);
                WriteBloomEnabled(enabled);
                // Sync the toolbar's bloom-toggle button so all three
                // entry points (toolbar button, dialog checkbox, menu)
                // agree on the current state.
                SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_BLOOM_TOGGLE,
                            MAKELONG(enabled ? TRUE : FALSE, 0));
                RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return TRUE;
            }

            if (code == SN_CHANGE)
            {
                SPINNER_INFO si = {0};
                si.Mask    = SPIF_VALUE;
                si.IsFloat = true;
                Spinner_GetInfo(GetDlgItem(hDlg, id), &si);
                const float v = si.f.Value;

                switch (id)
                {
                    case IDC_BLOOM_STRENGTH:
                        info->engine->SetBloomStrength(v);
                        WriteBloomFloat(L"BloomStrength", v);
                        break;
                    case IDC_BLOOM_CUTOFF:
                        info->engine->SetBloomCutoff(v);
                        WriteBloomFloat(L"BloomCutoff", v);
                        break;
                    case IDC_BLOOM_SIZE:
                        info->engine->SetBloomSize(v);
                        WriteBloomFloat(L"BloomSize", v);
                        break;
                    default:
                        return FALSE;
                }
                RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CLOSE:
        {
            if (info != NULL)
            {
                GetWindowRect(hDlg, &info->bloomDlgRect);
                HKEY hKey;
                if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                                   REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
                {
                    RegSetValueEx(hKey, L"BloomDialogPos", 0, REG_BINARY,
                                  (const BYTE*)&info->bloomDlgRect, sizeof(info->bloomDlgRect));
                    RegCloseKey(hKey);
                }
                ShowWindow(hDlg, SW_HIDE);
                info->bloomDlgVisible = false;
                CheckMenuItem(GetMenu(info->hMainWnd), ID_VIEW_BLOOM, MF_BYCOMMAND | MF_UNCHECKED);
            }
            return TRUE;
        }
    }
    return FALSE;
}

static void ToggleBloomDialog(APPLICATION_INFO* info)
{
    if (info == NULL || info->hMainWnd == NULL) return;

    if (info->hBloomDlg == NULL)
    {
        info->hBloomDlg = CreateDialogParam(
            info->hInstance,
            MAKEINTRESOURCE(IDD_BLOOM),
            info->hMainWnd,
            BloomDlgProc,
            (LPARAM)info);

        if (info->hBloomDlg == NULL) return;

        // Try to restore prior position; ignore stale off-screen rects.
        if (info->bloomDlgRect.right > info->bloomDlgRect.left)
        {
            RECT r = info->bloomDlgRect;
            HMONITOR hm = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
            if (hm != NULL)
            {
                SetWindowPos(info->hBloomDlg, NULL, r.left, r.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    if (info->bloomDlgVisible)
    {
        GetWindowRect(info->hBloomDlg, &info->bloomDlgRect);
        HKEY hKey;
        if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                           REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
        {
            RegSetValueEx(hKey, L"BloomDialogPos", 0, REG_BINARY,
                          (const BYTE*)&info->bloomDlgRect, sizeof(info->bloomDlgRect));
            RegCloseKey(hKey);
        }
        ShowWindow(info->hBloomDlg, SW_HIDE);
        info->bloomDlgVisible = false;
    }
    else
    {
        ShowWindow(info->hBloomDlg, SW_SHOW);
        SetForegroundWindow(info->hBloomDlg);
        info->bloomDlgVisible = true;
    }

    CheckMenuItem(GetMenu(info->hMainWnd), ID_VIEW_BLOOM,
                  MF_BYCOMMAND | (info->bloomDlgVisible ? MF_CHECKED : MF_UNCHECKED));
}

// ============================================================================
// — Lighting dialog (View → Lighting…)
// ============================================================================
//
// Mirrors the Petroglyph map editor's Sun / Fill panel: per-light
// intensity + Z angle + tilt angle, four sun-only color pickers
// (Ambient, Specular, Diffuse, Shadow), Force Fill Light Alignment
// checkbox, Mirror Sun one-shot button. Same modeless lifecycle as
// BloomDlg above.
//
// The dialog owns the UI representation (R,G,B + intensity + degrees);
// the engine owns the rendering representation (D3DXVECTOR4 Diffuse /
// Specular / Position). Conversion happens here on every write.
// Registry holds the UI representation so re-opening the dialog after
// a restart round-trips losslessly.

// Defaults — eyeballed from the supplied map-editor screenshot. The
// table is the single source of truth for "what does the editor look
// like on a fresh install", referenced by both InitializeLightingFromRegistry
// (default-on-miss) and ApplyLightingDefaults (Reset to defaults).
static const float    kLightSunIntensityDefault   = 0.50f;
static const float    kLightSunZAngleDefault      = 0.0f;
static const float    kLightSunTiltDefault        = 45.0f;
static const COLORREF kLightSunAmbientDefault     = RGB( 40,  40,  50);
static const COLORREF kLightSunSpecularDefault    = RGB(190, 190, 200);
static const COLORREF kLightSunDiffuseDefault     = RGB(180, 180, 190);
static const COLORREF kLightSunShadowDefault      = RGB(100, 100, 110);
static const bool     kLightForceAlignDefault     = true;
static const float    kLightFill1IntensityDefault = 0.50f;
static const float    kLightFill1ZAngleDefault    = 120.0f;
static const float    kLightFill1TiltDefault      = -10.0f;
static const COLORREF kLightFill1DiffuseDefault   = RGB( 60,  80, 160);
static const float    kLightFill2IntensityDefault = 0.50f;
static const float    kLightFill2ZAngleDefault    = 210.0f;
static const float    kLightFill2TiltDefault      = -10.0f;
static const COLORREF kLightFill2DiffuseDefault   = RGB( 60,  80, 160);

// Force-align computes fill angles from Sun Z. Tilt is fixed regardless
// of sun position — classic 3-light setup where the fills come from
// below-flanks to wash shadows from a top-down key light.
static const float kForceAlignFillTilt    = -10.0f;
static const float kForceAlignFill1Offset = 120.0f;
static const float kForceAlignFill2Offset = 210.0f;

// (Z angle, tilt) → unit direction vector. Convention: Z=azimuth in the
// XY plane measured from +X, tilt=elevation above the XY plane. This
// engine's "up" axis is +Z (see Engine::m_eye.Up = (0,0,1)). The
// resulting vector is fed into Light.Position; Engine::SetLight
// normalizes and derives Direction internally.
static D3DXVECTOR4 DirectionFromZTilt(float z_deg, float tilt_deg)
{
    const float zRad    = D3DXToRadian(z_deg);
    const float tiltRad = D3DXToRadian(tilt_deg);
    const float c       = cosf(tiltRad);
    return D3DXVECTOR4(c * cosf(zRad), c * sinf(zRad), sinf(tiltRad), 0.0f);
}

// Build an Engine::Light from UI values. intensity scales both diffuse
// and specular (the screenshot's panel has only one intensity per
// light). Fills pass specularColor = RGB(0,0,0) so their Specular vec4
// comes out zero, matching the map editor's "fills are diffuse-only"
// behavior.
static Engine::Light MakeLight(float z_deg, float tilt_deg,
                               COLORREF diffuseColor, COLORREF specularColor,
                               float intensity)
{
    Engine::Light L = {};
    L.Position = DirectionFromZTilt(z_deg, tilt_deg);
    L.Direction = D3DXVECTOR4(0,0,0,0); // SetLight overwrites this

    const float dR = GetRValue(diffuseColor)  / 255.0f * intensity;
    const float dG = GetGValue(diffuseColor)  / 255.0f * intensity;
    const float dB = GetBValue(diffuseColor)  / 255.0f * intensity;
    L.Diffuse = D3DXVECTOR4(dR, dG, dB, 1.0f);

    const float sR = GetRValue(specularColor) / 255.0f * intensity;
    const float sG = GetGValue(specularColor) / 255.0f * intensity;
    const float sB = GetBValue(specularColor) / 255.0f * intensity;
    L.Specular = D3DXVECTOR4(sR, sG, sB, 1.0f);

    return L;
}

// COLORREF → (R/255, G/255, B/255, 0) for scene-global shadow. m_shadow.xyz
// drives the reference-model shadow darken tint (Engine::RenderReferenceShadows),
// so it IS sampled at render time. The alpha (w) is unused by the darken — keep w=0.
static D3DXVECTOR4 ColorToVec4(COLORREF c)
{
    return D3DXVECTOR4(GetRValue(c) / 255.0f,
                       GetGValue(c) / 255.0f,
                       GetBValue(c) / 255.0f,
                       0.0f);
}

// COLORREF → (R/255, G/255, B/255, 1) for scene-global ambient — game-faithful.
// The engine folds ambient into its SPH lighting as ambient.xyz * ambient.w
// (src/SphericalHarmonics.cpp:76); production Mesh*/RSkin* shaders light ambient
// only via that SPH path, so w=1 lights the mesh ambient floor the game uses.
// The host restore (HostWindow.cpp) and React ambientToVec4 push the same w=1.
static D3DXVECTOR4 AmbientToVec4(COLORREF c)
{
    return D3DXVECTOR4(GetRValue(c) / 255.0f,
                       GetGValue(c) / 255.0f,
                       GetBValue(c) / 255.0f,
                       1.0f);
}

// Registry helpers — same shape as ReadBloomFloat / WriteBloomFloat,
// scoped to the lighting key space. All under
// HKCU\Software\AloParticleEditor.
static float ReadLightingFloat(const wchar_t* name, float defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        float value;
        DWORD type, size = sizeof(value);
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == sizeof(value) && std::isfinite(value))
        {
            RegCloseKey(hKey);
            return value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteLightingFloat(const wchar_t* name, float value)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueEx(hKey, name, 0, REG_BINARY, (const BYTE*)&value, sizeof(value));
        RegCloseKey(hKey);
    }
}

static COLORREF ReadLightingColor(const wchar_t* name, COLORREF defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value, type, size = sizeof(value);
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        {
            RegCloseKey(hKey);
            return (COLORREF)value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteLightingColor(const wchar_t* name, COLORREF value)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD v = (DWORD)value;
        RegSetValueEx(hKey, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(hKey);
    }
}

static bool ReadLightingBool(const wchar_t* name, bool defaultValue)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD value, type, size = sizeof(value);
        if (RegQueryValueEx(hKey, name, NULL, &type, (LPBYTE)&value, &size) == ERROR_SUCCESS && type == REG_DWORD)
        {
            RegCloseKey(hKey);
            return value != 0;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

static void WriteLightingBool(const wchar_t* name, bool value)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                       REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        DWORD v = value ? 1 : 0;
        RegSetValueEx(hKey, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(hKey);
    }
}

static bool ReadLightingDialogPos(RECT& out)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type, size = sizeof(out);
        if (RegQueryValueEx(hKey, L"LightingDialogPos", NULL, &type, (LPBYTE)&out, &size) == ERROR_SUCCESS
            && type == REG_BINARY && size == sizeof(out))
        {
            RegCloseKey(hKey);
            return true;
        }
        RegCloseKey(hKey);
    }
    return false;
}

// Names of all 17 lighting registry keys, in one place so the Reset
// View Settings handler can sweep them with a single loop.
static const wchar_t* const kLightingRegistryKeys[] = {
    L"LightSunIntensity",     L"LightSunZAngle",         L"LightSunTilt",
    L"LightSunAmbientColor",  L"LightSunSpecularColor",  L"LightSunDiffuseColor",
    L"LightSunShadowColor",
    L"LightingForceFillAlignment",
    L"LightFill1Intensity",   L"LightFill1ZAngle",       L"LightFill1Tilt",
    L"LightFill1DiffuseColor",
    L"LightFill2Intensity",   L"LightFill2ZAngle",       L"LightFill2Tilt",
    L"LightFill2DiffuseColor",
    L"LightingDialogPos",
};

// Apply the persisted (or default-on-miss) UI values to the engine.
// Called once at startup and after every Reset to defaults. The
// dialog's WM_INITDIALOG / WM_USER handler re-reads the same registry
// values directly into the controls — both flows agree on the values
// because both go through Read*().
static void PushLightingToEngine(Engine* engine)
{
    if (engine == NULL) return;

    const float sunIntensity     = ReadLightingFloat(L"LightSunIntensity",     kLightSunIntensityDefault);
    const float sunZ             = ReadLightingFloat(L"LightSunZAngle",        kLightSunZAngleDefault);
    const float sunTilt          = ReadLightingFloat(L"LightSunTilt",          kLightSunTiltDefault);
    const COLORREF sunAmbient    = ReadLightingColor(L"LightSunAmbientColor",  kLightSunAmbientDefault);
    const COLORREF sunSpecular   = ReadLightingColor(L"LightSunSpecularColor", kLightSunSpecularDefault);
    const COLORREF sunDiffuse    = ReadLightingColor(L"LightSunDiffuseColor",  kLightSunDiffuseDefault);
    const COLORREF sunShadow     = ReadLightingColor(L"LightSunShadowColor",   kLightSunShadowDefault);
    const bool  forceAlign       = ReadLightingBool (L"LightingForceFillAlignment", kLightForceAlignDefault);
    const float fill1Intensity   = ReadLightingFloat(L"LightFill1Intensity",   kLightFill1IntensityDefault);
    const float fill1Z_persisted = ReadLightingFloat(L"LightFill1ZAngle",      kLightFill1ZAngleDefault);
    const float fill1Tilt_persisted = ReadLightingFloat(L"LightFill1Tilt",     kLightFill1TiltDefault);
    const COLORREF fill1Diffuse  = ReadLightingColor(L"LightFill1DiffuseColor", kLightFill1DiffuseDefault);
    const float fill2Intensity   = ReadLightingFloat(L"LightFill2Intensity",   kLightFill2IntensityDefault);
    const float fill2Z_persisted = ReadLightingFloat(L"LightFill2ZAngle",      kLightFill2ZAngleDefault);
    const float fill2Tilt_persisted = ReadLightingFloat(L"LightFill2Tilt",     kLightFill2TiltDefault);
    const COLORREF fill2Diffuse  = ReadLightingColor(L"LightFill2DiffuseColor", kLightFill2DiffuseDefault);

    // Force-align: when ON, fill angles are computed from sun and the
    // persisted free-edit values are NOT used. When OFF, persisted
    // values feed the engine directly.
    const float fill1Z    = forceAlign ? (sunZ + kForceAlignFill1Offset) : fill1Z_persisted;
    const float fill1Tilt = forceAlign ?  kForceAlignFillTilt            : fill1Tilt_persisted;
    const float fill2Z    = forceAlign ? (sunZ + kForceAlignFill2Offset) : fill2Z_persisted;
    const float fill2Tilt = forceAlign ?  kForceAlignFillTilt            : fill2Tilt_persisted;

    engine->SetLight(Engine::LT_SUN,   MakeLight(sunZ,    sunTilt,    sunDiffuse,  sunSpecular,        sunIntensity));
    engine->SetLight(Engine::LT_FILL1, MakeLight(fill1Z,  fill1Tilt,  fill1Diffuse, RGB(0,0,0),         fill1Intensity));
    engine->SetLight(Engine::LT_FILL2, MakeLight(fill2Z,  fill2Tilt,  fill2Diffuse, RGB(0,0,0),         fill2Intensity));
    engine->SetAmbient(AmbientToVec4(sunAmbient));
    engine->SetShadow (ColorToVec4(sunShadow));
}

// One-stop entry point used by both the in-panel Reset button and View
// → Reset View Settings. Wipes all 17 keys, then pushes defaults into
// the engine (PushLightingToEngine re-reads with default-on-miss).
// Sends WM_USER to the dialog if open so its controls reseed.
static void ApplyLightingDefaults(APPLICATION_INFO* info)
{
    if (info == NULL) return;

    HKEY hKey;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        for (size_t i = 0; i < _countof(kLightingRegistryKeys); ++i)
        {
            RegDeleteValue(hKey, kLightingRegistryKeys[i]);
        }
        RegCloseKey(hKey);
    }

    PushLightingToEngine(info->engine);

    if (info->hLightingDlg != NULL)
    {
        SendMessage(info->hLightingDlg, WM_USER, 0, 0);
    }
    if (info->hRenderWnd != NULL)
    {
        RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

// Configure a spinner for one of the lighting numeric fields.
static void ConfigureLightingSpinner(HWND hDlg, int id, float lo, float hi, float incr, float value)
{
    SPINNER_INFO si = {0};
    si.Mask        = SPIF_ALL;
    si.IsFloat     = true;
    si.f.MinValue  = lo;
    si.f.MaxValue  = hi;
    si.f.Increment = incr;
    si.f.Value     = value;
    Spinner_SetInfo(GetDlgItem(hDlg, id), &si);
}

// Reflect the Force Fill Light Alignment checkbox state in the UI:
// fill-angle spinners go read-only (values stay readable, input is
// blocked) and the Mirror Sun button greys out (Mirror Sun is
// undefined while alignment is enforced — keep the controls
// orthogonal). Called from WM_INITDIALOG / WM_USER reseed and from
// the checkbox click handler.
static void UpdateForceAlignEnableState(HWND hDlg, bool forceAlignOn)
{
    Spinner_SetReadOnly(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_ZANGLE), forceAlignOn);
    Spinner_SetReadOnly(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_TILT),   forceAlignOn);
    Spinner_SetReadOnly(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_ZANGLE), forceAlignOn);
    Spinner_SetReadOnly(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_TILT),   forceAlignOn);
    EnableWindow(GetDlgItem(hDlg, IDC_LIGHTING_MIRROR_SUN), forceAlignOn ? FALSE : TRUE);
}

// Re-seed every control in the dialog from the registry. Used at
// WM_INITDIALOG and after Reset View Settings via WM_USER. We read
// from registry rather than from the engine because the dialog is the
// authoritative source for the UI representation (R,G,B + intensity +
// degrees), which the engine doesn't preserve losslessly.
static void LightingDlg_Load(HWND hDlg, APPLICATION_INFO* info)
{
    if (info == NULL) return;

    const float    sunIntensity   = ReadLightingFloat(L"LightSunIntensity",     kLightSunIntensityDefault);
    const float    sunZ           = ReadLightingFloat(L"LightSunZAngle",        kLightSunZAngleDefault);
    const float    sunTilt        = ReadLightingFloat(L"LightSunTilt",          kLightSunTiltDefault);
    const COLORREF sunAmbient     = ReadLightingColor(L"LightSunAmbientColor",  kLightSunAmbientDefault);
    const COLORREF sunSpecular    = ReadLightingColor(L"LightSunSpecularColor", kLightSunSpecularDefault);
    const COLORREF sunDiffuse     = ReadLightingColor(L"LightSunDiffuseColor",  kLightSunDiffuseDefault);
    const COLORREF sunShadow      = ReadLightingColor(L"LightSunShadowColor",   kLightSunShadowDefault);
    const bool     forceAlign     = ReadLightingBool (L"LightingForceFillAlignment", kLightForceAlignDefault);
    const float    fill1Intensity = ReadLightingFloat(L"LightFill1Intensity",   kLightFill1IntensityDefault);
    const float    fill1Z         = ReadLightingFloat(L"LightFill1ZAngle",      kLightFill1ZAngleDefault);
    const float    fill1Tilt      = ReadLightingFloat(L"LightFill1Tilt",        kLightFill1TiltDefault);
    const COLORREF fill1Diffuse   = ReadLightingColor(L"LightFill1DiffuseColor", kLightFill1DiffuseDefault);
    const float    fill2Intensity = ReadLightingFloat(L"LightFill2Intensity",   kLightFill2IntensityDefault);
    const float    fill2Z         = ReadLightingFloat(L"LightFill2ZAngle",      kLightFill2ZAngleDefault);
    const float    fill2Tilt      = ReadLightingFloat(L"LightFill2Tilt",        kLightFill2TiltDefault);
    const COLORREF fill2Diffuse   = ReadLightingColor(L"LightFill2DiffuseColor", kLightFill2DiffuseDefault);

    // When force-align is ON, the fill-angle spinners show computed
    // values rather than the persisted "last free-edit" values. This
    // way the visible numbers match what's actually being rendered.
    const float fill1Z_shown    = forceAlign ? (sunZ + kForceAlignFill1Offset) : fill1Z;
    const float fill1Tilt_shown = forceAlign ?  kForceAlignFillTilt            : fill1Tilt;
    const float fill2Z_shown    = forceAlign ? (sunZ + kForceAlignFill2Offset) : fill2Z;
    const float fill2Tilt_shown = forceAlign ?  kForceAlignFillTilt            : fill2Tilt;

    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_SUN_INTENSITY,   0.0f, 3.0f,  0.05f, sunIntensity);
    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_SUN_ZANGLE,      0.0f, 360.0f, 1.0f, sunZ);
    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_SUN_TILT,      -90.0f, 90.0f,  1.0f, sunTilt);
    ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_AMBIENT),  sunAmbient);
    ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_SPECULAR), sunSpecular);
    ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_DIFFUSE),  sunDiffuse);
    ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_SHADOW),   sunShadow);
    CheckDlgButton(hDlg, IDC_LIGHTING_FORCE_ALIGN, forceAlign ? BST_CHECKED : BST_UNCHECKED);

    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_FILL1_INTENSITY, 0.0f, 3.0f,   0.05f, fill1Intensity);
    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_FILL1_ZANGLE,    0.0f, 360.0f, 1.0f,  fill1Z_shown);
    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_FILL1_TILT,    -90.0f, 90.0f,  1.0f,  fill1Tilt_shown);
    ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_DIFFUSE), fill1Diffuse);

    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_FILL2_INTENSITY, 0.0f, 3.0f,   0.05f, fill2Intensity);
    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_FILL2_ZANGLE,    0.0f, 360.0f, 1.0f,  fill2Z_shown);
    ConfigureLightingSpinner(hDlg, IDC_LIGHTING_FILL2_TILT,    -90.0f, 90.0f,  1.0f,  fill2Tilt_shown);
    ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_DIFFUSE), fill2Diffuse);

    UpdateForceAlignEnableState(hDlg, forceAlign);
}

// Recompute and push to the engine when any UI value changes. Same
// shape as PushLightingToEngine but reads from the dialog's current
// control values rather than from registry. Writes the engine; the
// per-change WM_COMMAND case writes the specific changed registry key.
static void LightingDlg_PushAll(HWND hDlg, Engine* engine)
{
    if (engine == NULL) return;

    SPINNER_INFO si = {0};
    si.Mask = SPIF_VALUE;
    si.IsFloat = true;

    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_SUN_INTENSITY), &si); const float sunIntensity = si.f.Value;
    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_SUN_ZANGLE),    &si); const float sunZ         = si.f.Value;
    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_SUN_TILT),      &si); const float sunTilt      = si.f.Value;
    const COLORREF sunAmbient  = ColorButton_GetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_AMBIENT));
    const COLORREF sunSpecular = ColorButton_GetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_SPECULAR));
    const COLORREF sunDiffuse  = ColorButton_GetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_DIFFUSE));
    const COLORREF sunShadow   = ColorButton_GetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_SHADOW));

    const bool forceAlign = (IsDlgButtonChecked(hDlg, IDC_LIGHTING_FORCE_ALIGN) == BST_CHECKED);

    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_INTENSITY), &si); const float fill1Intensity = si.f.Value;
    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_ZANGLE),    &si); const float fill1Z_shown   = si.f.Value;
    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_TILT),      &si); const float fill1Tilt_shown = si.f.Value;
    const COLORREF fill1Diffuse = ColorButton_GetColor(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_DIFFUSE));

    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_INTENSITY), &si); const float fill2Intensity = si.f.Value;
    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_ZANGLE),    &si); const float fill2Z_shown   = si.f.Value;
    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_TILT),      &si); const float fill2Tilt_shown = si.f.Value;
    const COLORREF fill2Diffuse = ColorButton_GetColor(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_DIFFUSE));

    // When force-align is on, the fill spinner values are themselves
    // computed; use them directly (they were just set by the Sun-Z
    // change handler) rather than recomputing.
    engine->SetLight(Engine::LT_SUN,   MakeLight(sunZ,         sunTilt,         sunDiffuse,  sunSpecular,  sunIntensity));
    engine->SetLight(Engine::LT_FILL1, MakeLight(fill1Z_shown, fill1Tilt_shown, fill1Diffuse, RGB(0,0,0),  fill1Intensity));
    engine->SetLight(Engine::LT_FILL2, MakeLight(fill2Z_shown, fill2Tilt_shown, fill2Diffuse, RGB(0,0,0),  fill2Intensity));
    engine->SetAmbient(AmbientToVec4(sunAmbient));
    engine->SetShadow (ColorToVec4(sunShadow));
}

// When Sun Z changes and force-align is ON, update the fill spinners
// (visible values) to track. Pushes the new fill positions to the
// engine but does NOT touch the persisted fill Z/Tilt registry values
// (those represent the user's last free-edit state, restored when
// alignment is unchecked).
static void LightingDlg_RealignFills(HWND hDlg, Engine* engine)
{
    SPINNER_INFO si = {0};
    si.Mask = SPIF_VALUE;
    si.IsFloat = true;
    Spinner_GetInfo(GetDlgItem(hDlg, IDC_LIGHTING_SUN_ZANGLE), &si);
    const float sunZ = si.f.Value;

    si.Mask = SPIF_ALL;
    si.IsFloat = true;
    si.f.MinValue  = 0.0f;
    si.f.MaxValue  = 360.0f;
    si.f.Increment = 1.0f;
    si.f.Value     = sunZ + kForceAlignFill1Offset;
    Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_ZANGLE), &si);
    si.f.Value     = sunZ + kForceAlignFill2Offset;
    Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_ZANGLE), &si);

    si.f.MinValue  = -90.0f;
    si.f.MaxValue  =  90.0f;
    si.f.Value     = kForceAlignFillTilt;
    Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_TILT), &si);
    Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_TILT), &si);

    LightingDlg_PushAll(hDlg, engine);
}

static INT_PTR CALLBACK LightingDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    APPLICATION_INFO* info = (APPLICATION_INFO*)(LONG_PTR)GetWindowLongPtr(hDlg, GWLP_USERDATA);

    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            info = (APPLICATION_INFO*)lParam;
            SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)info);
            LightingDlg_Load(hDlg, info);
            return TRUE;
        }

        case WM_USER:
            // Reseed-from-registry after Reset View Settings / Reset
            // to defaults. The caller has already updated the engine
            // and registry; we just refresh the visible controls.
            LightingDlg_Load(hDlg, info);
            return TRUE;

        case WM_COMMAND:
        {
            if (info == NULL || info->engine == NULL) return FALSE;
            const WORD code = HIWORD(wParam);
            const WORD id   = LOWORD(wParam);

            // Esc / Cancel route.
            if (id == IDCANCEL && code == BN_CLICKED)
            {
                SendMessage(hDlg, WM_CLOSE, 0, 0);
                return TRUE;
            }

            // Force Fill Light Alignment toggle.
            if (id == IDC_LIGHTING_FORCE_ALIGN && code == BN_CLICKED)
            {
                const bool nowOn = (IsDlgButtonChecked(hDlg, IDC_LIGHTING_FORCE_ALIGN) == BST_CHECKED);
                WriteLightingBool(L"LightingForceFillAlignment", nowOn);
                if (nowOn)
                {
                    // Snap fill spinners to computed values; push to
                    // engine. Persisted fill Z/Tilt registry keys are
                    // left untouched (they hold the user's last
                    // free-edit values for when alignment turns off).
                    LightingDlg_RealignFills(hDlg, info->engine);
                }
                else
                {
                    // Restore persisted free-edit values into the
                    // spinners and re-push to engine.
                    const float fill1Z    = ReadLightingFloat(L"LightFill1ZAngle", kLightFill1ZAngleDefault);
                    const float fill1Tilt = ReadLightingFloat(L"LightFill1Tilt",   kLightFill1TiltDefault);
                    const float fill2Z    = ReadLightingFloat(L"LightFill2ZAngle", kLightFill2ZAngleDefault);
                    const float fill2Tilt = ReadLightingFloat(L"LightFill2Tilt",   kLightFill2TiltDefault);
                    SPINNER_INFO si = {0};
                    si.Mask = SPIF_VALUE;
                    si.IsFloat = true;
                    si.f.Value = fill1Z;    Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_ZANGLE), &si);
                    si.f.Value = fill1Tilt; Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_TILT),   &si);
                    si.f.Value = fill2Z;    Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_ZANGLE), &si);
                    si.f.Value = fill2Tilt; Spinner_SetInfo(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_TILT),   &si);
                    LightingDlg_PushAll(hDlg, info->engine);
                }
                UpdateForceAlignEnableState(hDlg, nowOn);
                RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return TRUE;
            }

            // Mirror Sun: copy sun diffuse to both fills' diffuse.
            if (id == IDC_LIGHTING_MIRROR_SUN && code == BN_CLICKED)
            {
                const COLORREF sunDiffuse = ColorButton_GetColor(GetDlgItem(hDlg, IDC_LIGHTING_SUN_DIFFUSE));
                ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_FILL1_DIFFUSE), sunDiffuse);
                ColorButton_SetColor(GetDlgItem(hDlg, IDC_LIGHTING_FILL2_DIFFUSE), sunDiffuse);
                WriteLightingColor(L"LightFill1DiffuseColor", sunDiffuse);
                WriteLightingColor(L"LightFill2DiffuseColor", sunDiffuse);
                LightingDlg_PushAll(hDlg, info->engine);
                RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return TRUE;
            }

            // Reset to defaults.
            if (id == IDC_LIGHTING_RESET && code == BN_CLICKED)
            {
                if (MessageBox(hDlg, L"Reset all lighting to defaults?", L"Reset Lighting",
                               MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    ApplyLightingDefaults(info);
                }
                return TRUE;
            }

            // Spinner change.
            if (code == SN_CHANGE)
            {
                SPINNER_INFO si = {0};
                si.Mask    = SPIF_VALUE;
                si.IsFloat = true;
                Spinner_GetInfo(GetDlgItem(hDlg, id), &si);
                const float v = si.f.Value;

                switch (id)
                {
                    case IDC_LIGHTING_SUN_INTENSITY: WriteLightingFloat(L"LightSunIntensity", v); break;
                    case IDC_LIGHTING_SUN_ZANGLE:
                        WriteLightingFloat(L"LightSunZAngle", v);
                        // Sun-Z change with force-align ON cascades to
                        // the fill spinners + engine.
                        if (IsDlgButtonChecked(hDlg, IDC_LIGHTING_FORCE_ALIGN) == BST_CHECKED)
                        {
                            LightingDlg_RealignFills(hDlg, info->engine);
                            RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                            return TRUE;
                        }
                        break;
                    case IDC_LIGHTING_SUN_TILT:        WriteLightingFloat(L"LightSunTilt",        v); break;
                    case IDC_LIGHTING_FILL1_INTENSITY: WriteLightingFloat(L"LightFill1Intensity", v); break;
                    case IDC_LIGHTING_FILL1_ZANGLE:    WriteLightingFloat(L"LightFill1ZAngle",    v); break;
                    case IDC_LIGHTING_FILL1_TILT:      WriteLightingFloat(L"LightFill1Tilt",      v); break;
                    case IDC_LIGHTING_FILL2_INTENSITY: WriteLightingFloat(L"LightFill2Intensity", v); break;
                    case IDC_LIGHTING_FILL2_ZANGLE:    WriteLightingFloat(L"LightFill2ZAngle",    v); break;
                    case IDC_LIGHTING_FILL2_TILT:      WriteLightingFloat(L"LightFill2Tilt",      v); break;
                    default: return FALSE;
                }
                LightingDlg_PushAll(hDlg, info->engine);
                RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return TRUE;
            }

            // ColorButton change.
            if (code == CBN_CHANGE)
            {
                const COLORREF c = ColorButton_GetColor(GetDlgItem(hDlg, id));
                switch (id)
                {
                    case IDC_LIGHTING_SUN_AMBIENT:    WriteLightingColor(L"LightSunAmbientColor",   c); break;
                    case IDC_LIGHTING_SUN_SPECULAR:   WriteLightingColor(L"LightSunSpecularColor",  c); break;
                    case IDC_LIGHTING_SUN_DIFFUSE:    WriteLightingColor(L"LightSunDiffuseColor",   c); break;
                    case IDC_LIGHTING_SUN_SHADOW:     WriteLightingColor(L"LightSunShadowColor",    c); break;
                    case IDC_LIGHTING_FILL1_DIFFUSE:  WriteLightingColor(L"LightFill1DiffuseColor", c); break;
                    case IDC_LIGHTING_FILL2_DIFFUSE:  WriteLightingColor(L"LightFill2DiffuseColor", c); break;
                    default: return FALSE;
                }
                LightingDlg_PushAll(hDlg, info->engine);
                RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CLOSE:
        {
            if (info != NULL)
            {
                GetWindowRect(hDlg, &info->lightingDlgRect);
                HKEY hKey;
                if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                                   REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
                {
                    RegSetValueEx(hKey, L"LightingDialogPos", 0, REG_BINARY,
                                  (const BYTE*)&info->lightingDlgRect, sizeof(info->lightingDlgRect));
                    RegCloseKey(hKey);
                }
                ShowWindow(hDlg, SW_HIDE);
                info->lightingDlgVisible = false;
                CheckMenuItem(GetMenu(info->hMainWnd), ID_VIEW_LIGHTING, MF_BYCOMMAND | MF_UNCHECKED);
            }
            return TRUE;
        }
    }
    return FALSE;
}

static void ToggleLightingDialog(APPLICATION_INFO* info)
{
    if (info == NULL || info->hMainWnd == NULL) return;

    if (info->hLightingDlg == NULL)
    {
        info->hLightingDlg = CreateDialogParam(
            info->hInstance,
            MAKEINTRESOURCE(IDD_LIGHTING),
            info->hMainWnd,
            LightingDlgProc,
            (LPARAM)info);

        if (info->hLightingDlg == NULL) return;

        // Restore prior position; ignore stale off-screen rects.
        if (info->lightingDlgRect.right > info->lightingDlgRect.left)
        {
            RECT r = info->lightingDlgRect;
            HMONITOR hm = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
            if (hm != NULL)
            {
                SetWindowPos(info->hLightingDlg, NULL, r.left, r.top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    if (info->lightingDlgVisible)
    {
        GetWindowRect(info->hLightingDlg, &info->lightingDlgRect);
        HKEY hKey;
        if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
                           REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
        {
            RegSetValueEx(hKey, L"LightingDialogPos", 0, REG_BINARY,
                          (const BYTE*)&info->lightingDlgRect, sizeof(info->lightingDlgRect));
            RegCloseKey(hKey);
        }
        ShowWindow(info->hLightingDlg, SW_HIDE);
        info->lightingDlgVisible = false;
    }
    else
    {
        ShowWindow(info->hLightingDlg, SW_SHOW);
        SetForegroundWindow(info->hLightingDlg);
        info->lightingDlgVisible = true;
    }

    CheckMenuItem(GetMenu(info->hMainWnd), ID_VIEW_LIGHTING,
                  MF_BYCOMMAND | (info->lightingDlgVisible ? MF_CHECKED : MF_UNCHECKED));
}

// Returns the display label for a mod (nickname if set, else folder name).
static const wstring& ModDisplayLabel(const ModEntry& m)
{
	return m.nickname.empty() ? m.folderName : m.nickname;
}

// ScanModsDir / DiscoverMods moved to ModManager (src/ModManager.cpp)
// in D6 so both the legacy menu and the new-UI bridge dispatcher
// share the same disk-scanning code.

// Lazily create (and cache) the menu fonts used to draw mod entries.
// Owner-drawn items need to know which font to render with — we use the system
// menu font for the folder name and an italic variant for "(nickname)".
static void EnsureMenuFonts(APPLICATION_INFO* info)
{
	if (info->hMenuFont != NULL) return;

	NONCLIENTMETRICS ncm = {};
	ncm.cbSize = sizeof(ncm);
	if (!SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
	{
		// Fallback to the default GUI font if SPI fails
		info->hMenuFont       = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
		info->hMenuItalicFont = info->hMenuFont;
		return;
	}

	info->hMenuFont = CreateFontIndirect(&ncm.lfMenuFont);
	LOGFONT lf = ncm.lfMenuFont;
	lf.lfItalic = TRUE;
	info->hMenuItalicFont = CreateFontIndirect(&lf);
}

// Build (or rebuild) the dynamically-populated Mods popup. The HMENU returned
// is owned by the caller; we pass it back so we can re-tick on selection
// changes. The submenus are owned by the parent menu and freed with it.
static void RebuildModsMenu(APPLICATION_INFO* info)
{
	// Empty out the current menu by deleting all items
	if (info->hModsMenu != NULL)
	{
		while (GetMenuItemCount(info->hModsMenu) > 0)
		{
			DeleteMenu(info->hModsMenu, 0, MF_BYPOSITION);
		}
	}
	else
	{
		info->hModsMenu = CreatePopupMenu();
		// Insert into the main menu bar before "Help"
		HMENU hMenuBar = GetMenu(info->hMainWnd);
		int   helpPos  = -1;
		for (int i = 0; i < GetMenuItemCount(hMenuBar); i++)
		{
			TCHAR buf[64] = {0};
			MENUITEMINFO mii = {};
			mii.cbSize     = sizeof(mii);
			mii.fMask      = MIIM_STRING;
			mii.dwTypeData = buf;
			mii.cch        = 63;
			if (GetMenuItemInfo(hMenuBar, i, TRUE, &mii) && wcsstr(buf, L"Help") != NULL)
			{
				helpPos = i;
				break;
			}
		}
		MENUITEMINFO mii = {};
		mii.cbSize     = sizeof(mii);
		mii.fMask      = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
		mii.wID        = 0;  // not used, has submenu
		mii.hSubMenu   = info->hModsMenu;
		mii.dwTypeData = (LPWSTR)L"&Mods";
		InsertMenuItem(hMenuBar, helpPos >= 0 ? helpPos : GetMenuItemCount(hMenuBar), TRUE, &mii);
	}

	// Read mod state from ModManager (single source of truth post-D6).
	const std::vector<ModEntry>& mods = info->modManager ? info->modManager->GetMods() : std::vector<ModEntry>{};
	const std::wstring& selectedPath = info->modManager ? info->modManager->GetSelectedModPath() : std::wstring{};

	// Top: Unmodded radio item
	UINT noneFlags = MF_STRING | (selectedPath.empty() ? MF_CHECKED : MF_UNCHECKED);
	AppendMenu(info->hModsMenu, noneFlags, ID_MOD_NONE, L"&Unmodded");
	AppendMenu(info->hModsMenu, MF_SEPARATOR, 0, NULL);

	EnsureMenuFonts(info);

	// Build FoC submenu and Base Game submenu. Mod entries are owner-drawn so
	// we can render the folder name in regular weight followed by " (nickname)"
	// in italics. The mod's index is stashed in dwItemData for the
	// WM_MEASUREITEM / WM_DRAWITEM handlers to look up.
	HMENU hFoCMenu  = CreatePopupMenu();
	HMENU hBaseMenu = CreatePopupMenu();
	int   nFoC = 0, nBase = 0;
	for (size_t i = 0; i < mods.size(); i++)
	{
		const ModEntry& m = mods[i];
		UINT id      = ID_MOD_FIRST + (UINT)i;
		bool checked = (_wcsicmp(m.path.c_str(), selectedPath.c_str()) == 0);

		MENUITEMINFO mii = {};
		mii.cbSize     = sizeof(mii);
		mii.fMask      = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA;
		mii.fType      = MFT_OWNERDRAW;
		mii.fState     = checked ? MFS_CHECKED : MFS_UNCHECKED;
		mii.wID        = id;
		mii.dwItemData = (ULONG_PTR)i;

		HMENU hTarget = m.isFoC ? hFoCMenu : hBaseMenu;
		InsertMenuItem(hTarget, GetMenuItemCount(hTarget), TRUE, &mii);
		if (m.isFoC) nFoC++; else nBase++;
	}

	if (nFoC > 0)
	{
		AppendMenu(info->hModsMenu, MF_POPUP | MF_STRING, (UINT_PTR)hFoCMenu, L"&Forces of Corruption");
	}
	else
	{
		DestroyMenu(hFoCMenu);
	}
	if (nBase > 0)
	{
		AppendMenu(info->hModsMenu, MF_POPUP | MF_STRING, (UINT_PTR)hBaseMenu, L"&Base Game");
	}
	else
	{
		DestroyMenu(hBaseMenu);
	}

	if (nFoC > 0 || nBase > 0)
	{
		AppendMenu(info->hModsMenu, MF_SEPARATOR, 0, NULL);
	}
	AppendMenu(info->hModsMenu, MF_STRING, ID_MOD_REFRESH, L"&Refresh Mod List");

	// Enable WM_MENURBUTTONUP delivery for right-click context (set-nickname).
	// Without MNS_DRAGDROP, Windows silently dismisses on right-click instead
	// of notifying us. Apply to the parent and the FoC/Base submenus.
	MENUINFO mi = {};
	mi.cbSize  = sizeof(mi);
	mi.fMask   = MIM_STYLE;
	mi.dwStyle = MNS_DRAGDROP;
	SetMenuInfo(info->hModsMenu, &mi);
	if (nFoC > 0)  SetMenuInfo(hFoCMenu,  &mi);
	if (nBase > 0) SetMenuInfo(hBaseMenu, &mi);

	DrawMenuBar(info->hMainWnd);
}

// Apply a mod selection. D6: the cross-mode core (FileManager
// swap, registry persist, palette swap, thumbnail cache clear, engine
// shader/texture reload) lives in ModManager::SelectMod so the new-UI
// bridge can call it. The legacy wrapper below adds the Win32-only
// finalisation (status bar text on shader-reload failure, HBITMAP
// preview rebuild, modeless skydome picker refresh, render-window
// invalidate, HMENU rebuild).
static void SelectMod(APPLICATION_INFO* info, const wstring& modPath)
{
	bool ok = info->modManager ? info->modManager->SelectMod(modPath) : false;
	if (!ok && info->engine != NULL && info->hStatusBar != NULL)
	{
		SendMessage(info->hStatusBar, SB_SETTEXT, 4,
		            (LPARAM)L"Mod shader reload failed — keeping previous shaders");
	}

	// follow-up: skydome texture was re-resolved inside the engine
	// ReloadTextures call, but the toolbar preview's cached HBITMAP and
	// the (possibly open) picker's image list still point at the
	// previous mod's bytes. Rebuild both so the UI matches what the
	// engine just loaded.
	if (info->engine != NULL)
	{
		RebuildBackgroundPreviewBitmap(info);
		if (info->hSkydomePicker != NULL && IsWindowVisible(info->hSkydomePicker))
		{
			SendMessage(info->hSkydomePicker, WM_USER, 0, 0);
		}
	}
	RebuildModsMenu(info);
	if (info->hRenderWnd != NULL)
	{
		InvalidateRect(info->hRenderWnd, NULL, TRUE);
	}
}

// Find the mod entry corresponding to a given menu command ID, or NULL.
// Const-correct: returns a pointer into ModManager's internal vector;
// caller must not retain the pointer across mod-list mutations
// (DiscoverMods will reallocate). Used by the nickname dialog
// WM_COMMAND handler, which acts on the returned entry's path
// immediately and doesn't cache it.
static ModEntry* FindModById(APPLICATION_INFO* info, UINT id)
{
	if (id < ID_MOD_FIRST || id > ID_MOD_LAST) return NULL;
	if (!info->modManager) return NULL;
	UINT idx = id - ID_MOD_FIRST;
	const auto& mods = info->modManager->GetMods();
	if (idx >= mods.size()) return NULL;
	// const_cast is safe here: WriteModNickname (the only caller via
	// WM_APP_SHOW_NICKNAME) writes the user-edited nickname BACK into
	// the entry, and the entry's storage lives inside ModManager's
	// mods vector. Future refactor: route nickname writes through a
	// ModManager method so the cast goes away.
	return const_cast<ModEntry*>(&mods[idx]);
}

// Backing data for the IDD_MOD_NICKNAME dialog
struct NicknameDialogState
{
	const ModEntry* mod;
	wstring         result;
	bool            committed;
};

static INT_PTR CALLBACK NicknameDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	NicknameDialogState* state = (NicknameDialogState*)(LONG_PTR)GetWindowLongPtr(hDlg, GWLP_USERDATA);
	switch (msg)
	{
	case WM_INITDIALOG:
		state = (NicknameDialogState*)lParam;
		SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)state);
		{
			wstring title = L"Nickname for " + state->mod->folderName;
			SetWindowText(hDlg, title.c_str());
			SetDlgItemText(hDlg, IDC_MOD_NICKNAME_EDIT, state->mod->nickname.c_str());
			SendDlgItemMessage(hDlg, IDC_MOD_NICKNAME_EDIT, EM_SETSEL, 0, -1);
			SetFocus(GetDlgItem(hDlg, IDC_MOD_NICKNAME_EDIT));
		}
		return FALSE; // we set focus ourselves

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
			{
				TCHAR buf[256] = {0};
				GetDlgItemText(hDlg, IDC_MOD_NICKNAME_EDIT, buf, 255);
				state->result    = buf;
				state->committed = true;
				EndDialog(hDlg, IDOK);
			}
			return TRUE;
		case IDCANCEL:
			state->committed = false;
			EndDialog(hDlg, IDCANCEL);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

static bool ShowNicknameDialog(HWND hParent, const ModEntry& mod, wstring& outNickname)
{
	NicknameDialogState state = { &mod, L"", false };
	INT_PTR rv = DialogBoxParam(GetModuleHandle(NULL),
	                            MAKEINTRESOURCE(IDD_MOD_NICKNAME),
	                            hParent,
	                            NicknameDialogProc,
	                            (LPARAM)&state);
	if (rv == IDOK && state.committed)
	{
		outNickname = state.result;
		return true;
	}
	return false;
}

// =============================================================================
//: Import emitters from another .alo file.
//
// File → Import Emitters from File… opens a .alo picker, then a modal dialog
// showing the source file's emitter tree with TVS_CHECKBOXES. On OK the
// ticked emitters are cloned into the active ParticleSystem via the
// existing copy / paste serialiser (Emitter::copy + Emitter(ChunkReader&)),
// with two extra passes:
//
//   * Spawn-field re-map. The clipboard serialiser writes -1 for spawn
//     children. When the user kept the source emitter's child in the
//     same import, we rewrite the destination's spawn field to point at
//     the cloned child's destination index. Dropped children stay -1.
//
//   * Link-group re-create. Source linkGroup IDs aren't portable (they
//     collide with unrelated destination IDs). For each source group
//     where ≥2 members were imported, allocate a fresh destination ID
//     via CreateLinkGroup. Single-member imports arrive unlinked.
// =============================================================================
struct ImportEmittersDialogState
{
    APPLICATION_INFO* info;
    wstring           sourcePath;
    ParticleSystem*   source;        // dialog owns this; deleted by caller
    bool              autoChildren;  // mirrors IDC_IMPORT_AUTO_CHILDREN
    bool              cascading;     // re-entry guard for parent/child cascade
    vector<size_t>    picks;         // populated by OK handler from tree state
};

static ParticleSystem* ImportEmitters_LoadFile(HWND hParent, const wstring& path)
{
    //: delegate the load to the pure-IO helper; the legacy UI
    // (MessageBox surfacing) stays here. Pre-this function
    // distinguished three failure modes (file-open / wexception /
    // generic catch-all); the helper merges them into one — the
    // single user-visible MessageBox is fine because the user only
    // cares "open failed" + a message.
    std::string err;
    std::unique_ptr<ParticleSystem> owned = LoadParticleSystem(path, &err);
    if (!owned)
    {
        if (err.empty())
        {
            MessageBox(hParent, L"Could not open the selected file.",
                       L"Import Emitters from File", MB_OK | MB_ICONERROR);
        }
        else
        {
            MessageBox(hParent,
                       LoadString(IDS_ERROR_FILE_OPEN, AnsiToWide(err).c_str()).c_str(),
                       L"Import Emitters from File", MB_OK | MB_ICONERROR);
        }
        return NULL;
    }
    return owned.release();
}

static HTREEITEM ImportEmitters_AddTreeItem(HWND hTree, HTREEITEM hParent,
                                              const ParticleSystem* source,
                                              size_t srcIdx)
{
    if (srcIdx >= source->getEmitters().size()) return NULL;
    const ParticleSystem::Emitter& emit = source->getEmitter(srcIdx);
    wstring label = AnsiToWide(emit.name);
    if (emit.linkGroup != 0)
    {
        wchar_t suffix[32];
        swprintf_s(suffix, L" [L%u]", (unsigned)emit.linkGroup);
        label += suffix;
    }
    TVINSERTSTRUCTW tvis = {};
    tvis.hParent      = hParent;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
    tvis.item.pszText = const_cast<LPWSTR>(label.c_str());
    tvis.item.lParam  = (LPARAM)srcIdx;
    HTREEITEM hItem = TreeView_InsertItem(hTree, &tvis);
    if (emit.spawnDuringLife != (size_t)-1)
        ImportEmitters_AddTreeItem(hTree, hItem, source, emit.spawnDuringLife);
    if (emit.spawnOnDeath != (size_t)-1)
        ImportEmitters_AddTreeItem(hTree, hItem, source, emit.spawnOnDeath);
    return hItem;
}

static void ImportEmitters_PopulateTree(HWND hDlg, const ParticleSystem* source)
{
    HWND hTree = GetDlgItem(hDlg, IDC_IMPORT_TREE);
    TreeView_DeleteAllItems(hTree);
    if (source == NULL) return;
    for (size_t i = 0; i < source->getEmitters().size(); ++i)
    {
        if (source->getEmitter(i).parent == NULL)
        {
            HTREEITEM hRoot = ImportEmitters_AddTreeItem(hTree, TVI_ROOT, source, i);
            if (hRoot != NULL) TreeView_Expand(hTree, hRoot, TVE_EXPAND);
        }
    }
}

static bool ImportEmitters_IsChecked(HWND hTree, HTREEITEM hItem)
{
    TVITEM ti = {};
    ti.mask      = TVIF_HANDLE | TVIF_STATE;
    ti.hItem     = hItem;
    ti.stateMask = TVIS_STATEIMAGEMASK;
    TreeView_GetItem(hTree, &ti);
    // TVS_CHECKBOXES: state-image index 2 = checked, 1 = unchecked.
    return ((ti.state & TVIS_STATEIMAGEMASK) >> 12) == 2;
}

static void ImportEmitters_CollectTicked(HWND hTree, HTREEITEM hItem,
                                           vector<size_t>& out)
{
    while (hItem != NULL)
    {
        TVITEM ti = {};
        ti.mask      = TVIF_HANDLE | TVIF_PARAM | TVIF_STATE;
        ti.hItem     = hItem;
        ti.stateMask = TVIS_STATEIMAGEMASK;
        TreeView_GetItem(hTree, &ti);
        if (((ti.state & TVIS_STATEIMAGEMASK) >> 12) == 2)
            out.push_back((size_t)ti.lParam);
        ImportEmitters_CollectTicked(hTree, TreeView_GetChild(hTree, hItem), out);
        hItem = TreeView_GetNextSibling(hTree, hItem);
    }
}

static void ImportEmitters_SetCheckRecursive(HWND hTree, HTREEITEM hItem,
                                               BOOL checked)
{
    while (hItem != NULL)
    {
        TreeView_SetCheckState(hTree, hItem, checked);
        ImportEmitters_SetCheckRecursive(hTree, TreeView_GetChild(hTree, hItem),
                                          checked);
        hItem = TreeView_GetNextSibling(hTree, hItem);
    }
}

static void ImportEmitters_UpdateOKEnable(HWND hDlg)
{
    HWND hTree = GetDlgItem(hDlg, IDC_IMPORT_TREE);
    vector<size_t> picks;
    ImportEmitters_CollectTicked(hTree, TreeView_GetRoot(hTree), picks);
    EnableWindow(GetDlgItem(hDlg, IDOK), !picks.empty());
}

// Execute the import: clone picks into the destination, re-map spawn fields,
// re-create link groups. Caller refreshes the tree + captures undo.
static void ImportEmitters_Execute(APPLICATION_INFO* info,
                                     ParticleSystem* source,
                                     const vector<size_t>& picks)
{
    if (info == NULL || info->particleSystem == NULL ||
        source == NULL || picks.empty()) return;

    // The clone / spawn-rebind / graph-revalidate / link-group-recreate core
    // now lives on the data layer (ParticleSystem::ImportEmittersFrom), shared
    // with the `emitters/import-from-file` bridge handler. The UI's
    // unique-name generator is injected so the data layer stays UI-agnostic.
    // Caller still owns undo + tree refresh.
    info->particleSystem->ImportEmittersFrom(
        *source, picks,
        [info](const std::string& name) {
            return GenerateDuplicateName(info->particleSystem, name);
        });
}

static INT_PTR CALLBACK ImportEmittersDialogProc(HWND hDlg, UINT msg,
                                                   WPARAM wParam, LPARAM lParam)
{
    ImportEmittersDialogState* state =
        (ImportEmittersDialogState*)(LONG_PTR)GetWindowLongPtr(hDlg, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
        state = (ImportEmittersDialogState*)lParam;
        SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)state);
        SetDlgItemText(hDlg, IDC_IMPORT_PATH_LABEL, state->sourcePath.c_str());
        CheckDlgButton(hDlg, IDC_IMPORT_AUTO_CHILDREN,
                       state->autoChildren ? BST_CHECKED : BST_UNCHECKED);
        ImportEmitters_PopulateTree(hDlg, state->source);
        ImportEmitters_UpdateOKEnable(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_IMPORT_BROWSE:
        {
            wchar_t buf[1024] = {};
            wcscpy_s(buf, state->sourcePath.c_str());
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hDlg;
            ofn.lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = (DWORD)(sizeof(buf) / sizeof(buf[0]));
            ofn.lpstrTitle  = L"Import Emitters from File";
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
            if (GetOpenFileNameW(&ofn))
            {
                wstring newPath = buf;
                ParticleSystem* newSrc = ImportEmitters_LoadFile(hDlg, newPath);
                if (newSrc != NULL)
                {
                    delete state->source;
                    state->source     = newSrc;
                    state->sourcePath = newPath;
                    SetDlgItemText(hDlg, IDC_IMPORT_PATH_LABEL, newPath.c_str());
                    ImportEmitters_PopulateTree(hDlg, state->source);
                    ImportEmitters_UpdateOKEnable(hDlg);
                }
            }
            return TRUE;
        }
        case IDC_IMPORT_AUTO_CHILDREN:
            state->autoChildren =
                IsDlgButtonChecked(hDlg, IDC_IMPORT_AUTO_CHILDREN) == BST_CHECKED;
            return TRUE;
        case IDC_IMPORT_SELECT_ALL:
        {
            HWND hTree = GetDlgItem(hDlg, IDC_IMPORT_TREE);
            state->cascading = true;
            ImportEmitters_SetCheckRecursive(hTree, TreeView_GetRoot(hTree), TRUE);
            state->cascading = false;
            ImportEmitters_UpdateOKEnable(hDlg);
            return TRUE;
        }
        case IDC_IMPORT_CLEAR:
        {
            HWND hTree = GetDlgItem(hDlg, IDC_IMPORT_TREE);
            state->cascading = true;
            ImportEmitters_SetCheckRecursive(hTree, TreeView_GetRoot(hTree), FALSE);
            state->cascading = false;
            ImportEmitters_UpdateOKEnable(hDlg);
            return TRUE;
        }
        case IDOK:
        {
            HWND hTree = GetDlgItem(hDlg, IDC_IMPORT_TREE);
            state->picks.clear();
            ImportEmitters_CollectTicked(hTree, TreeView_GetRoot(hTree),
                                          state->picks);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_NOTIFY:
    {
        LPNMHDR hdr = (LPNMHDR)lParam;
        if (hdr->idFrom == IDC_IMPORT_TREE)
        {
            HWND hTree = GetDlgItem(hDlg, IDC_IMPORT_TREE);
            // NM_CLICK: hit-test for a checkbox toggle. We can't read the
            // post-toggle state synchronously (the comctl32 toggle hasn't
            // fired yet), so we defer via PostMessage and process it after
            // the message pump runs.
            if (hdr->code == NM_CLICK)
            {
                DWORD pos = GetMessagePos();
                POINT pt = { (LONG)(SHORT)LOWORD(pos), (LONG)(SHORT)HIWORD(pos) };
                ScreenToClient(hTree, &pt);
                TVHITTESTINFO ht = {};
                ht.pt = pt;
                HTREEITEM hHit = TreeView_HitTest(hTree, &ht);
                if (hHit != NULL && (ht.flags & TVHT_ONITEMSTATEICON))
                {
                    PostMessage(hDlg, WM_APP + 1, 0, (LPARAM)hHit);
                }
            }
            // TVN_KEYDOWN: spacebar toggles the focused item's checkbox.
            else if (hdr->code == TVN_KEYDOWN)
            {
                NMTVKEYDOWN* k = (NMTVKEYDOWN*)lParam;
                if (k->wVKey == VK_SPACE)
                {
                    HTREEITEM hSel = TreeView_GetSelection(hTree);
                    if (hSel != NULL)
                        PostMessage(hDlg, WM_APP + 1, 0, (LPARAM)hSel);
                }
            }
        }
        break;
    }

    case WM_APP + 1:
    {
        // Deferred checkbox-toggle handler. By the time this fires the
        // user's click/space has flipped the state, so we read the new
        // state and (optionally) cascade descendants.
        HTREEITEM hItem = (HTREEITEM)lParam;
        HWND hTree = GetDlgItem(hDlg, IDC_IMPORT_TREE);
        if (hItem != NULL && state != NULL &&
            state->autoChildren && !state->cascading)
        {
            const bool checked = ImportEmitters_IsChecked(hTree, hItem);
            state->cascading = true;
            ImportEmitters_SetCheckRecursive(hTree, TreeView_GetChild(hTree, hItem),
                                              checked ? TRUE : FALSE);
            state->cascading = false;
        }
        ImportEmitters_UpdateOKEnable(hDlg);
        return TRUE;
    }
    }
    return FALSE;
}

static void DoImportEmittersFromFile(APPLICATION_INFO* info)
{
    if (info == NULL || info->particleSystem == NULL) return;

    // Step 1: file picker.
    wchar_t buf[1024] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = info->hMainWnd;
    ofn.lpstrFilter = L"Particle Files (*.alo)\0*.alo\0All Files (*.*)\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle  = L"Import Emitters from File";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    wstring path = buf;

    // Step 2: load source ParticleSystem (no global state mutated).
    ParticleSystem* source = ImportEmitters_LoadFile(info->hMainWnd, path);
    if (source == NULL) return;

    // Step 3: dialog.
    ImportEmittersDialogState state;
    state.info         = info;
    state.sourcePath   = path;
    state.source       = source;
    state.autoChildren = true;
    state.cascading    = false;

    INT_PTR rv = DialogBoxParam(GetModuleHandle(NULL),
                                MAKEINTRESOURCE(IDD_IMPORT_EMITTERS),
                                info->hMainWnd,
                                ImportEmittersDialogProc,
                                (LPARAM)&state);

    // Step 4: on OK with non-empty picks, do the import.
    if (rv == IDOK && !state.picks.empty())
    {
        ImportEmitters_Execute(info, state.source, state.picks);
        // Refresh the emitter list tree (rebuild from m_emitters) and emit
        // one undo capture so Ctrl+Z rolls the whole batch back.
        EmitterList_SetParticleSystem(info->hEmitterList, info->particleSystem);
        CaptureUndo(info, 0);
        SetFileChanged(info, true);
#ifndef NDEBUG
        fprintf(stdout, "[Import] %zu emitter(s) from %S\n",
                state.picks.size(), path.c_str());
        fflush(stdout);
#endif
    }

    delete state.source;
}

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

	// If the user picked a path that worked, persist it for next launch
	if (fileManager != NULL && !pickedPath.empty())
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

void main( APPLICATION_INFO* info, const vector<wstring>& argv )
{
	vector<wstring> gameRoots;
	FileManager* fileManager = createFileManager( info->hMainWnd, argv, &gameRoots );
	if (fileManager == NULL)
	{
		// No file manager, no play
		return;
	}

	try
	{
		// Initialize the other managers and engine
		TextureManager textureManager(fileManager, "Data\\Art\\Textures\\");
        ShaderManager  shaderManager (fileManager, "Data\\Art\\Shaders\\");

		// Wire up mod-state pointers so the UI can hot-swap textures
		info->fileManager    = fileManager;
		info->textureManager = &textureManager;
		info->shaderManager  = &shaderManager;
		info->gameRoots      = gameRoots;

		// D6: ModManager owns mod discovery + active-mod state for
		// both UI modes. Stack-local lifetime matches textureManager /
		// shaderManager (live until main() returns). Setting m_engine
		// happens AFTER `info->engine = new Engine(...)` below.
		// RestoreLastSelectedMod() applies the registry path to
		// FileManager + TexturePalette internally so this block no
		// longer needs the inline ReadLastMod / SetActiveMod chain.
		ModManager modManager(fileManager, gameRoots);
		info->modManager = &modManager;
		modManager.DiscoverMods();
		modManager.RestoreLastSelectedMod();
		RebuildModsMenu(info);

		// Create the rendering engine
        try
        {
		    info->engine = new Engine(info->hMainWnd, info->hRenderWnd, textureManager, shaderManager, *info->fileManager);

		    // D6: now that the Engine exists, give it to ModManager
		    // so future SelectMod() calls can hot-swap shaders / textures.
		    modManager.SetEngine(info->engine);

		    // — give the texture-palette its services now that the
		    // engine has a D3D device. Both pointers must outlive the
		    // editor window, which they do (engine and FileManager
		    // both live in this enclosing scope until app exit).
		    TexturePalette::SetServices(info->fileManager, info->engine->GetDevice());

            // View settings persisted across sessions: pull from registry,
            // fall back to engine defaults when no value stored. Defaults
            // are passed to the helpers so a fresh registry behaves
            // identically to before this feature.
            info->engine->SetBackground(ReadBackgroundColor(info->engine->GetBackground()));
            info->engine->SetGround    (ReadShowGround    (info->engine->GetGround()));
            // Ground Z deliberately does NOT persist across sessions — every
            // launch starts at 0. Anchoring the preview's vertical reference
            // to a fixed point makes "did I just open the editor or is this
            // a continued workflow?" unambiguous, and Reset View Settings
            // can't accidentally surprise you with a stale offset that was
            // tweaked in a previous session.
            info->engine->SetGroundZ(0.0f);
            //: restore per-slot custom file paths BEFORE the
            // selected-slot load, so SetGroundTexture can find the
            // right source. Each slot's path persists independently
            // of the current selection (they're "user data"; the
            // selected slot is a "view setting"). Reset View Settings
            // does NOT touch these — only the picker dialog's
            // "Reset all slots" button does.
            for (int slot = 0; slot < Engine::kGroundTextureCount; ++slot)
            {
                std::wstring path = ReadGroundSlotPath(slot);
                if (!path.empty())
                    info->engine->SetGroundSlotCustomPath(slot, path);
            }
            //: load persisted solid-colour for the special slot 4.
            info->engine->SetGroundSolidColor(
                ReadGroundSolidColor(info->engine->GetGroundSolidColor()));
            info->engine->SetGroundTexture(
                ReadGroundTexture(info->engine->GetGroundTexture()));
            // Bloom: defaults off; tunables default to game-spec values
            // baked into Engine's constructor (0.1 / 1.0 / 0.25).
            info->engine->SetBloom        (ReadBloomEnabled(false));
            info->engine->SetBloomStrength(ReadBloomFloat(L"BloomStrength", info->engine->GetBloomStrength()));
            info->engine->SetBloomCutoff  (ReadBloomFloat(L"BloomCutoff",   info->engine->GetBloomCutoff()));
            info->engine->SetBloomSize    (ReadBloomFloat(L"BloomSize",     info->engine->GetBloomSize()));
            COLORREF persistedCustom[16];
            if (ReadCustomColors(persistedCustom)) ColorButton_SetCustomColors(persistedCustom);

            // Spawner config is intentionally NOT restored from registry —
            // it resets to defaults at the start of each session per
            // user preference. Dialog position is still restored so the
            // window doesn't bounce around between launches.
            ReadSpawnerDialogPos(info->spawnerWindowRect);
            ReadBloomDialogPos(info->bloomDlgRect);

            //: push the persisted (or default) lighting values
            // into the engine before the first frame renders, so the
            // viewport opens with whatever lighting the user last
            // configured. Default-on-miss yields the map-editor's
            // canonical Sun/Fill setup (intensity 0.5, sun tilt 45°,
            // force-aligned fills) which differs from Engine's
            // built-in hardcoded defaults (sun white along +X, fills
            // off, ambient black).
            PushLightingToEngine(info->engine);
            ReadLightingDialogPos(info->lightingDlgRect);

            // rework: no ColorButton seed — the Background button is a
            // plain owner-drawn BUTTON that reads engine state directly.
            // RebuildBackgroundPreviewBitmap below paints the thumbnail if a
            // skydome slot is active, otherwise the owner-draw falls through
            // to the swatch path on first WM_DRAWITEM.

            //: build the toolbar preview thumbnail for whatever
            // slot the engine actually loaded. May differ from the
            // persisted index if that slot's texture failed to load
            // (we fell back to dirt). Built once now; refreshed on
            // any subsequent selection change.
            RebuildGroundTexturePreviewBitmap(info);
            InvalidateRect(info->hGroundTexturePreview, NULL, TRUE);

            //: skydome restore. Custom paths first so SetSkydomeSlot can
            // reload a previously-active custom slot.
            for (int s = Engine::kSkydomeFirstCustomSlot; s < Engine::kSkydomeSlotCount; ++s)
            {
                info->engine->SetSkydomeCustomPath(s, ReadSkydomeCustomPath(s));
            }
            info->engine->SetSkydomeSlot(ReadSkydomeIndex(0));

            //: build the skydome toolbar preview thumbnail.
            RebuildBackgroundPreviewBitmap(info);

            // Sync the ground-toggle toolbar button to the (possibly
            // restored-from-registry) engine state. The TBBUTTON definition
            // hardcodes TBSTATE_CHECKED at init time; without this re-sync
            // a persisted "ground off" looks like the toolbar lying — render
            // is correct but the button still shows pressed, and the next
            // click does the wrong thing.
            SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_SHOWGROUND,
                        MAKELONG(info->engine->GetGround() ? TRUE : FALSE, 0));

            // Seed the Ground Z spinner with the restored engine value
            // and grey it out if ground is hidden. SPIF_VALUE only —
            // range and increment were set at create time.
            {
                SPINNER_INFO si;
                si.Mask    = SPIF_VALUE;
                si.IsFloat = true;
                si.f.Value = info->engine->GetGroundZ();
                Spinner_SetInfo(info->hGroundZSpinner, &si);
            }
            EnableWindow(info->hGroundZLabel,   info->engine->GetGround());
            EnableWindow(info->hGroundZSpinner, info->engine->GetGround());

            // Sync the bloom-toggle toolbar button to engine state.
            // Grey out the button when bloom can't run (shader missing
            // / unsupported); otherwise reflect the persisted enable.
            const bool bloomAvail = info->engine->IsBloomAvailable();
            SendMessage(info->hToolbar, TB_ENABLEBUTTON, ID_VIEW_BLOOM_TOGGLE,
                        MAKELONG(bloomAvail ? TRUE : FALSE, 0));
            SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_BLOOM_TOGGLE,
                        MAKELONG(info->engine->GetBloom() ? TRUE : FALSE, 0));
        }
        catch (exception&)
        {
            DestroyWindow(info->hRenderWnd);
        }
		
		DoCloseFile(info);

        bool loaded = false;
        if (info->engine != NULL)
        {
			// See if a file was specified on the command line. CLI
			// arg wins over autosave recovery — if the user double-
			// clicked a .alo in Explorer, they want THAT file, not
			// a recovery prompt that interrupts their gesture. Any
			// orphan autosave stays untouched in TEMP for next
			// launch (the next plain-launch without a CLI arg will
			// see and prompt for it).
			for (size_t i = 1; i < argv.size(); i++)
			{
				if (PathFileExists(argv[i].c_str()) && !PathIsDirectory(argv[i].c_str()))
				{
					loaded = LoadFile(info, argv[i]);
					break;
				}
			}
        }

        // Recovery flow — only if no CLI file was loaded. Scans
        // %TEMP%\AloParticleEditor\ for autosave files left behind by
        // a crashed prior editor session (PID no longer matches a
        // live ParticleEditor.exe). Prompts the user; on Yes/No
        // restores the chosen tier; on Cancel discards. The orphan
        // session is consumed (files deleted) in all three cases.
        if (!loaded && info->engine != NULL)
        {
            Autosave::OrphanSession recover;
            if (Autosave::ScanForOrphan(&recover))
            {
                int choice = ShowRecoveryPrompt(info, recover);
                wstring restorePath;
                if (choice == IDYES && !recover.recentPath.empty())
                {
                    restorePath = recover.recentPath;
                }
                else if ((choice == IDYES && recover.recentPath.empty()
                       && !recover.stablePath.empty())
                      || (choice == IDNO  && !recover.stablePath.empty()
                       && !recover.recentPath.empty()))
                {
                    // YES on stable-only prompt, or NO on a both-tiers prompt
                    restorePath = recover.stablePath;
                }
                // Any other case (Cancel, or NO on a recent-only / stable-only
                // prompt) leaves restorePath empty → no restore.

                if (!restorePath.empty())
                {
                    loaded = RestoreFromAutosave(info, restorePath, recover.originalFilename);
                }

                // Orphan files are consumed regardless of the user's
                // choice. If we leave them, they'd surface again on
                // the next launch — confusing if the user just
                // chose "Discard."
                Autosave::DeleteOrphan(recover);
            }
        }

        if (!loaded)
        {
		    DoNewFile(info);
        }
		ShowWindow(info->hMainWnd, SW_SHOWNORMAL);

        HACCEL hAccel = LoadAccelerators( info->hInstance, MAKEINTRESOURCE(IDR_ACCELERATOR1));
		for (bool quit = false; !quit; )
		{
			MSG msg;
			while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				// Route keyboard input to the modeless spawner dialog when
				// it owns focus. IsDialogMessage handles Tab navigation /
				// arrow keys / etc. Order matters: try the spawner dialog
				// before the main window so spawner-focused keys aren't
				// stolen by the main accelerator table.
				bool consumed = false;
				if (info->hSpawnerDlg != NULL && info->spawnerVisible
				    && IsDialogMessage(info->hSpawnerDlg, &msg))
				{
					consumed = true;
				}
				// Same routing for the ground-texture picker so its
				// keyboard navigation (Tab between buttons, Esc to
				// dismiss, etc.) works while modeless.
				if (!consumed && info->hGroundPicker != NULL
				    && IsWindowVisible(info->hGroundPicker)
				    && IsDialogMessage(info->hGroundPicker, &msg))
				{
					consumed = true;
				}
				//: skydome picker — same modeless routing.
				if (!consumed && info->hSkydomePicker != NULL
				    && IsWindowVisible(info->hSkydomePicker)
				    && IsDialogMessage(info->hSkydomePicker, &msg))
				{
					consumed = true;
				}
				//: lighting dialog. Tab between its 18 controls,
				// Esc to dismiss (routed to IDCANCEL → WM_CLOSE).
				if (!consumed && info->hLightingDlg != NULL
				    && info->lightingDlgVisible
				    && IsDialogMessage(info->hLightingDlg, &msg))
				{
					consumed = true;
				}
				// Skip TranslateAccelerator while a tree drag-drop is in
				// progress: a stray Ctrl+Z mid-drag would call DoUndo →
				// RestoreFromSnapshot → delete info->particleSystem while
				// EmitterListControl::dragSource still points into the freed
				// system, producing a use-after-free on the next mouse
				// message. The drag's WM_KEYDOWN handler still receives Esc
				// because dispatch falls through to TranslateMessage /
				// DispatchMessage when accelerators are skipped.
				bool dragging = EmitterList_IsDragging(info->hEmitterList);
				if (!consumed
				    && (dragging || !TranslateAccelerator(info->hMainWnd, hAccel, &msg))
				    && !IsDialogMessage(info->hMainWnd, &msg))
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}

				if (msg.message == WM_QUIT)
				{
					quit = true;
				}
			}

            if (!quit && (info->isMinimized || info->engine == NULL))
			{
				WaitMessage();
			}
			else if (info->engine != NULL)
			{
				Render(info);
			}
		}

		delete fileManager;
	}
	catch (...)
	{
		delete fileManager;
		throw;
	}
}

static bool InitializeWindows( APPLICATION_INFO* info )
{
	WNDCLASSEX wcx;
	wcx.cbSize        = sizeof(WNDCLASSEX);
	wcx.style         = CS_HREDRAW | CS_VREDRAW;
	wcx.lpfnWndProc   = MainWindowProc;
	wcx.cbClsExtra    = 0;
	wcx.cbWndExtra    = 0;
	wcx.hInstance     = info->hInstance;
	// LoadIcon returns the 32×32 system-size icon; for hIconSm we
	// want the 16×16 variant explicitly. Leaving hIconSm = NULL lets
	// Windows downscale hIcon, but on some Win10/11 builds that scaler
	// falls back to a generic blank icon, which shows up as a plain
	// window glyph in the taskbar. LoadImage with explicit 16×16
	// picks the matching frame from logo.ico (which ships both 32×32
	// and 16×16 frames). Cached in locals because wcx.hIcon gets
	// overwritten when we register the renderer class below.
	HICON hIconBig   = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_LOGO), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
	HICON hIconSmall = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_LOGO), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
	// LoadImage can return NULL on certain configurations where the
	// RT_GROUP_ICON lookup fails to match the requested size — fall
	// back to LoadIcon which always returns the system-default size.
	if (hIconBig   == NULL) hIconBig   = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_LOGO));
	if (hIconSmall == NULL) hIconSmall = hIconBig;
	wcx.hIcon         = hIconBig;
	wcx.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wcx.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
	wcx.lpszMenuName  = MAKEINTRESOURCE(IDR_MENU1);
	wcx.lpszClassName = L"ParticleEditor";
	wcx.hIconSm       = hIconSmall;

	if (!RegisterClassEx(&wcx))
	{
		return false;
	}

	wcx.lpfnWndProc   = RenderWindowProc;
	wcx.hIcon         = NULL;
	wcx.hbrBackground = NULL;
	wcx.lpszMenuName  = NULL;
	wcx.lpszClassName = L"ParticleEditorRenderer";

	if (!RegisterClassEx(&wcx))
	{
		UnregisterClass(L"ParticleEditor", info->hInstance);
		return false;
	}

	if ((info->hMainWnd = CreateWindow(L"ParticleEditor", L"Particle Editor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_GROUP,
		50, 50, 1150, 850, NULL, NULL, info->hInstance, info)) == NULL)
	{
		UnregisterClass(L"ParticleEditorRenderer", info->hInstance);
		UnregisterClass(L"ParticleEditor", info->hInstance);
		return false;
	}

	// Modern Windows derives the taskbar icon from WM_SETICON on the
	// top-level window rather than from the WNDCLASS alone, so set
	// both icons explicitly. Without this the taskbar can fall back
	// to the generic "plain window" glyph even when the WNDCLASS has
	// a valid hIcon. We use the cached HICONs because wcx.hIcon was
	// cleared to NULL when we registered the renderer class.
	SendMessage(info->hMainWnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);
	SendMessage(info->hMainWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
	// Belt-and-suspenders: also set the class icon. RegisterClassEx
	// took the hIcon at the time we built wcx, but its hIconSm slot
	// might have been overwritten on the second RegisterClassEx call
	// for the renderer class on some compilers / runtimes. Re-asserting
	// both at runtime makes the class-level lookup consistent.
	SetClassLongPtr(info->hMainWnd, GCLP_HICON,   (LONG_PTR)hIconBig);
	SetClassLongPtr(info->hMainWnd, GCLP_HICONSM, (LONG_PTR)hIconSmall);

	if ((info->hRenderWnd = CreateWindowEx(WS_EX_CLIENTEDGE | WS_EX_ACCEPTFILES
        , L"ParticleEditorRenderer", NULL, WS_CHILD | WS_VISIBLE | WS_GROUP,
		400, 4, 100, 100, info->hMainWnd, NULL, info->hInstance, info)) == NULL)
	{
		DestroyWindow(info->hMainWnd);
		UnregisterClass(L"ParticleEditorRenderer", info->hInstance);
		UnregisterClass(L"ParticleEditor", info->hInstance);
		return false;
	}

	return true;
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

	// Task 1.3 — the WebView2 + D3D9 host is the DEFAULT UI on x64. With
	// no flag we build the same three managers legacy main() builds (so the
	// host can construct Engine identically), then return early — no legacy
	// WNDCLASS / windows are registered. `--legacy` (alias `--legacy-ui`) opts
	// back into the classic chrome; `--new-ui` is a no-op kept for the harness.
	//
	// x86 has no host (the `#else` at the dispatch below hard-returns), so the
	// default MUST stay legacy there — the `newUi` initializer is x64-gated.
	{
		const std::vector<std::wstring> argv = parseCommandLine();
#ifdef _WIN64
		bool newUi    = true;   // new UI is the default on x64
#else
		bool newUi    = false;  // x86: no host, legacy only (see #else at dispatch)
#endif
		bool legacy   = false;  // --legacy opts back into the classic chrome
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
		for (size_t i = 1; i < argv.size(); ++i)
		{
			if (argv[i] == L"--new-ui")    newUi    = true;  // no-op: now the default
			// --legacy / --legacy-ui both opt back into the classic chrome.
			// The `-ui` alias mirrors `--new-ui` and is the name used
			// throughout the codebase's comments/docs; before the default
			// flip it "worked" only because legacy was the default, so accept
			// it explicitly now that it no longer falls through.
			if (argv[i] == L"--legacy" ||
			    argv[i] == L"--legacy-ui") legacy = true;
			if (argv[i] == L"--dev-ui")    devUi    = true;
			if (argv[i] == L"--test-host") testHost = true;
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
			}
			// [world-lit] --sun r,g,b: sun diffuse colour (0..1 floats).
			if (argv[i] == L"--sun" && i + 1 < argv.size())
			{
				if (swscanf_s(argv[i + 1].c_str(), L"%f,%f,%f",
				              &captureSun[0], &captureSun[1], &captureSun[2]) == 3)
					captureHasSun = true;
			}
			// [world-lit] --sun-intensity f: scalar folded into the sun colour.
			if (argv[i] == L"--sun-intensity" && i + 1 < argv.size())
			{
				captureSunIntensity = (float)_wtof(argv[i + 1].c_str());
				captureHasSunI = true;
			}
		}
		// Warn when both --capture and --capture-ref are supplied: --capture-ref
		// wins (last write to capturePng, captureRef takes the branch in HostWindow).
		// This used to be a silent surprise; make it explicit so operators notice.
		if (!captureAlo.empty() && !captureRef.empty())
			fprintf(stderr, "warning: both --capture and --capture-ref supplied; using --capture-ref\n");
		// `--legacy` opts back into the classic chrome (clears the x64
		// default-true). Applied before the --capture clamp so a headless
		// --capture run — which needs the host to own the Engine — still wins.
		if (legacy) newUi = false;
		// --capture implies the new-UI host (it owns the Engine). Clamp a
		// garbage/zero --frames back to the default.
		if ((!captureAlo.empty() || !captureRef.empty()) && !capturePng.empty()) newUi = true;
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
			e->randomRotationAverage  = 0.0f;
			e->randomRotationVariance = 3.0f;  // ~+/-172 deg spread (default 0 = no rotation!)
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
			fwprintf(stderr, L"gen-smoke-test: wrote %s (blendMode 5, randomRotation, color ramp)\n", genSmokeTestPath.c_str());
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
		if (newUi)
		{
#ifdef _WIN64
			// D6: capture gameRoots so the host can build its
			// ModManager. createFileManager already discovers them
			// internally; pass &gameRoots to extract.
			std::vector<std::wstring> gameRoots;
			FileManager* fileManager = createFileManager(NULL, argv, &gameRoots);
			if (fileManager == NULL)
			{
				// Same semantics as legacy: user cancelled the data-path
				// picker. Exit cleanly with the same -1 sentinel.
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
			                           captureHasSunI, captureSunIntensity);
			delete fileManager;
#ifndef NDEBUG
			FreeConsole();
#endif
			return hostResult;
#else
			MessageBoxW(NULL,
				L"--new-ui is only available in the x64 build of this editor.",
				L"AloParticleEditor", MB_ICONERROR);
			return -1;
#endif
		}
	}

	int result = -1;

    APPLICATION_INFO info;
	info.hInstance				= hInstance;
	info.hMainWnd				= NULL;
	info.particleSystem			= NULL;
	info.selectedEmitter		= NULL;
	info.attachedParticleSystem = NULL;
	info.engine					= NULL;
	info.dragmode				= APPLICATION_INFO::NONE;
	info.isMinimized			= false;
	info.fileManager			= NULL;
	info.textureManager			= NULL;
	info.shaderManager			= NULL;
	info.modManager				= NULL;
	info.hModsMenu				= NULL;
	info.hMenuFont				= NULL;
	info.hMenuItalicFont		= NULL;
	info.spawner                = new SpawnerDriver();
	info.hSpawnerDlg            = NULL;
	info.spawnerVisible         = false;
	info.hGroundPicker          = NULL;
	info.hSkydomePicker         = NULL;
	info.hBackgroundPreviewBitmap = NULL;
	memset(&info.spawnerWindowRect, 0, sizeof(info.spawnerWindowRect));
	info.hBloomDlg              = NULL;
	info.bloomDlgVisible        = false;
	memset(&info.bloomDlgRect, 0, sizeof(info.bloomDlgRect));
	info.hLightingDlg           = NULL;
	info.lightingDlgVisible     = false;
	memset(&info.lightingDlgRect, 0, sizeof(info.lightingDlgRect));

#ifdef NDEBUG
 	try
#endif
    {
		// Initialize UI classes and create windows
		if (!UI_Initialize(hInstance) || !TexturePalette::Initialize(hInstance) || !InitializeWindows(&info))
		{
			//throw wruntime_error(LoadString(IDS_ERROR_UI_INITIALIZATION));
		}
		// — wire the EmitterProps HWND as the WM_PALETTE_COMMIT
		// recipient now that windows are created. Done before Engine
		// init because both the EmitterProps and the popup are created
		// before any commits can fire.
		TexturePalette::SetCommitTarget(info.hPropertyTabs);

		// Run the program
		result = main( &info, parseCommandLine() );
        DestroyWindow(info.hMainWnd);
        delete info.engine;
        delete info.spawner;
        info.spawner = NULL;
	}
#ifdef NDEBUG
	catch (wexception& e)
	{
        DestroyWindow(info.hMainWnd);
        delete info.engine;
        delete info.spawner;
        info.spawner = NULL;
		MessageBox(NULL, e.what(), NULL, MB_OK | MB_ICONHAND);
	}
	catch (exception& e)
	{
        DestroyWindow(info.hMainWnd);
        delete info.engine;
        delete info.spawner;
        info.spawner = NULL;
		MessageBoxA(NULL, e.what(), NULL, MB_OK | MB_ICONHAND);
	}
#endif

#ifndef NDEBUG
	FreeConsole();
#endif
	return result;
}
