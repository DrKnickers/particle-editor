#define _WIN32_WINNT 0x0501
#include <cmath>
#include <iostream>
#include <iomanip>
#include <string>
#include <algorithm>
#include <cfloat>
#include <sstream>
#include <queue>

#include "exceptions.h"
#include "UI/UI.h"
#include "SpawnerDriver.h"
#include "UndoStack.h"
#include "utils.h"
#include "engine.h"
#include "ParticleSystemInstance.h"
#include "Rescale.h"
#include "resource.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commdlg.h>
using namespace std;

static const int VERSION_MAJOR = 1;
static const int VERSION_MINOR = 5;

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

class TextureManager : public ITextureManager
{
	typedef map<string,IDirect3DTexture9*> TextureMap;

	TextureMap			textures;
	string				basePath;
	IFileManager*		fileManager;
	IDirect3DTexture9*  pDefaultTexture;

	static IDirect3DTexture9* createTexture(IDirect3DDevice9* pDevice, IFile* file)
	{
		IDirect3DTexture9* pTexture = NULL;
		unsigned long size = file->size();
		char* data = new char[ size ];
		file->read( (void*)data, size );
		if (D3DXCreateTextureFromFileInMemory( pDevice, (void*)data, size, &pTexture ) != D3D_OK)
		{
            delete[] data;
			return NULL;
		}
		delete[] data;
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
		return createTexture(pDevice, file);
	}

public:
	IDirect3DTexture9* getTexture(IDirect3DDevice9* pDevice, string filename)
	{
		size_t pos;
		transform(filename.begin(), filename.end(), filename.begin(), toupper);
		
		IDirect3DTexture9* pTexture = NULL;

		// See if the file exists as specified
		try
		{
			IFile* file = new PhysicalFile(AnsiToWide(filename));
			pTexture = createTexture(pDevice, file);
			delete file;
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

	static Effect* createShader(IDirect3DDevice9* pDevice, IFile* file)
	{
		ID3DXEffect* pShader = NULL;
		unsigned long size = file->size();
		char* data = new char[ size ];
		file->read( (void*)data, size );
		if (FAILED(D3DXCreateEffect( pDevice, (void*)data, size, NULL, NULL, D3DXFX_NOT_CLONEABLE, NULL, &pShader, NULL )))
		{
            delete[] data;
			return NULL;
		}
		delete[] data;
        
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
		return createShader(pDevice, file);
	}

public:
	Effect* getShader(IDirect3DDevice9* pDevice, string filename)
	{
		size_t pos;
		transform(filename.begin(), filename.end(), filename.begin(), toupper);
		Effect* pShader = NULL;

		// See if the file exists as specified
		try
		{
			IFile* file = new PhysicalFile(AnsiToWide(filename));
			pShader = createShader(pDevice, file);
			delete file;
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

class MouseCursor : public Object3D
{
    D3DXVECTOR3   m_oldPosition;
    LARGE_INTEGER m_updated;
    LARGE_INTEGER m_frequency;

public:
	void SetPosition(const D3DXVECTOR3& position)
	{
	    m_position = position;
    }

  	void UpdateVelocity()
    {
        LARGE_INTEGER time;
        QueryPerformanceCounter(&time);

        D3DXVECTOR3 dx = m_position - m_oldPosition;
        float       dt = (float)(time.QuadPart - m_updated.QuadPart) / (float)m_frequency.QuadPart;
        m_velocity = dx / dt;

        m_oldPosition = m_position;
        m_updated     = time;
    }

    MouseCursor() : Object3D(NULL, D3DXVECTOR3(0,0,0))
	{
        QueryPerformanceFrequency(&m_frequency);
        m_oldPosition = D3DXVECTOR3(0,0,0);
	}
};

static INT_PTR CALLBACK AboutProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            HWND hVersion = GetDlgItem(hWnd, IDC_VERSION);
            wstring text = GetWindowStr(hVersion);
            text = FormatString(text.c_str(), VERSION_MAJOR, VERSION_MINOR);
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

// Mod selection
struct ModEntry
{
	wstring path;          // full path, e.g. D:\...\corruption\Mods\Mod
	wstring folderName;    // "Mod"
	wstring nickname;      // user-set, may be empty
	bool    isFoC;         // true if under corruption\Mods, false if under GameData\Mods
};

// Reserved WM_COMMAND IDs for the dynamically-built Mods menu.
// Picked above the standard MFC range (0xE100+) and below 0xF000.
static const UINT ID_MOD_NONE     = 0xA000;
static const UINT ID_MOD_REFRESH  = 0xA001;
static const UINT ID_MOD_FIRST    = 0xA100;
static const UINT ID_MOD_LAST     = 0xAFFF;

// Posted to the main window from WM_MENURBUTTONUP to defer the nickname
// dialog until after the menu's modal loop has finished tearing down.
// wParam = WM_COMMAND ID of the moused-over mod entry.
static const UINT WM_APP_SHOW_NICKNAME = WM_APP + 1;

// Forward declarations for the Mods support code (defined later).
struct APPLICATION_INFO;
static vector<ModEntry> DiscoverMods(const vector<wstring>& gameRoots);
static void             RebuildModsMenu(APPLICATION_INFO* info);
static void             SelectMod(APPLICATION_INFO* info, const wstring& modPath);
static ModEntry*        FindModById(APPLICATION_INFO* info, UINT id);
static void             WriteModNickname(const wstring& modPath, const wstring& nickname);
static bool             ShowNicknameDialog(HWND hParent, const ModEntry& mod, wstring& outNickname);
static const wstring&   ModDisplayLabel(const ModEntry& m);
static void             EnsureMenuFonts(APPLICATION_INFO* info);
// View-state persistence (defined later, near the other Read/Write helpers).
static COLORREF         ReadBackgroundColor(COLORREF defaultValue);
static void             WriteBackgroundColor(COLORREF color);
static bool             ReadShowGround(bool defaultValue);
static void             WriteShowGround(bool show);
static bool             ReadCustomColors(COLORREF out[16]);
static void             WriteCustomColors(const COLORREF in[16]);
static void             ResetViewSettings();
static bool             ReadSpawnerDialogPos(RECT& out);
static void             WriteSpawnerDialogPos(const RECT& in);
static void             ToggleSpawnerDialog(APPLICATION_INFO* info);
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
    HWND      hEmitterList;
	HWND      hPropertyTabs;
	HWND      hRebar;
	HWND	  hToolbar;
	HWND	  hStatusBar;
	HWND	  hTrackTabs;
	HWND      hTrackEditors[N_TRACKS];

	Engine*         engine;
	MouseCursor		mouseCursor;

	ParticleSystem*			 particleSystem;
    ParticleSystem::Emitter* selectedEmitter;
	ParticleSystemInstance*  attachedParticleSystem;

	wstring   filename;
	bool      changed;

	// Mod state — set up after createFileManager and used by the Mods menu
	FileManager*    fileManager;     // non-owning; owned by main()
	TextureManager* textureManager;  // non-owning; owned by main()
	ShaderManager*  shaderManager;   // non-owning; owned by main()
	vector<wstring> gameRoots;       // the EmpireAtWarPaths used to construct fileManager
	vector<ModEntry> mods;
	wstring         selectedModPath; // empty = unmodded
	HMENU           hModsMenu;       // top-level "Mods" submenu (HMENU of the popup)
	HFONT           hMenuFont;       // cached for owner-drawn mod entries
	HFONT           hMenuItalicFont; // italic variant for the nickname parenthetical

	// Programmable particle spawner (View → Spawner… / F7)
	SpawnerDriver*  spawner;            // owned; created in WinMain init
	HWND            hSpawnerDlg;        // NULL until first show; lazy-created
	bool            spawnerVisible;     // tracks visibility for menu check-mark
	RECT            spawnerWindowRect;  // last-known position (session + registry)

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

static bool LoadFile(APPLICATION_INFO* info, const wstring& filename)
{
	// Delete old particle system
	if (info->engine != NULL)
	{
		info->engine->Clear();
	}
	delete info->particleSystem;
	info->particleSystem = NULL;

	PhysicalFile* file = new PhysicalFile(filename);
    ParticleSystem* system = NULL;
	try
	{
		system = new ParticleSystem(file);
		info->filename = filename;
		file->Release();
	}
	catch (wexception& e)
	{
        system = NULL;
		file->Release();
		MessageBox(info->hMainWnd, LoadString(IDS_ERROR_FILE_OPEN, e.what()).c_str(), NULL, MB_OK | MB_ICONERROR );
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

	PhysicalFile* file = new PhysicalFile(info->filename, PhysicalFile::WRITE);
	try
	{
		// Create particleSystem name from filename
		wstring name = info->filename;

		size_t pos = name.find_last_of('\\');
		if (pos != wstring::npos) name = name.substr(pos + 1);
		pos = name.find_last_of('.');
		if (pos != wstring::npos) name = name.substr(0, pos);
		transform(name.begin(), name.end(), name.begin(), tolower);

		info->particleSystem->setName(WideToAnsi(name,"_"));
		info->particleSystem->write(file);
		file->Release();
	}
	catch (wexception& e)
	{
		file->Release();
		MessageBox(info->hMainWnd, LoadString(IDS_ERROR_FILE_SAVE, e.what()).c_str(), NULL, MB_OK | MB_ICONERROR );
	}
    SetFileChanged(info, false);
    info->undoStack.MarkSaved();
    UpdateUndoRedoUI(info);
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
}

static bool DoMenuItem(APPLICATION_INFO* info, UINT id)
{
	switch (id)
	{
		case ID_FILE_NEW:     if (DoCloseFile   (info)) DoNewFile(info); break;
		case ID_FILE_OPEN:	  if (DoCheckChanges(info)) DoOpenFile(info); break;
		case ID_FILE_EXIT:    if (DoCheckChanges(info)) DestroyWindow(info->hMainWnd); break;
		case ID_FILE_SAVE:    DoSaveFile(info); break;
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

        case ID_SPAWNER_TRIGGER:
            // Shift+Space global hotkey. In Manual mode, fires a single
            // burst from the current path-anchor with the configured
            // velocity / jitter. In Auto mode, no-op (the schedule is
            // already running).
            if (info->spawner != NULL)
            {
                info->spawner->Trigger(info->particleSystem, info->engine);
            }
            break;

        case ID_VIEW_RESET_VIEW_SETTINGS:
            // Confirm — destructive: clears persisted background color,
            // ground-plane visibility, and the ChooseColor custom-color
            // palette. Camera is intentionally NOT included; it has its
            // own Reset Camera command above. Spawner config is
            // session-only and resets each launch; we still reset the
            // in-memory spawner state here for parity.
            if (MessageBox(info->hMainWnd,
                           L"Reset background color, ground plane visibility, and the color picker's custom colors to defaults?",
                           L"Reset View Settings",
                           MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                ResetViewSettings();
                if (info->engine != NULL)
                {
                    // Defaults match Engine's constructor (engine.cpp). Kept
                    // in sync by hand — there's only one default each, and
                    // they rarely change.
                    info->engine->SetBackground(RGB(0x14, 0x08, 0x34));
                    info->engine->SetGround(true);
                    ColorButton_SetColor(info->hBackgroundBtn, info->engine->GetBackground());
                    SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_SHOWGROUND,
                                MAKELONG(info->engine->GetGround(), 0));
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
    static TimeF       lastFrameTime = 0.0f;

    // Drive the programmable spawner first so any new instances it
    // creates are visible to the engine update + render this frame.
    {
        TimeF now = GetTimeF();
        float dt = (lastFrameTime > 0.0f) ? (float)(now - lastFrameTime) : 0.0f;
        lastFrameTime = now;
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
    SendMessage(info->hStatusBar, SB_SETTEXT, 2, (LPARAM)LoadString(IDS_STATUS_FPS,       (int)measurer.getFPS()).c_str());

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
			if ((info->hPropertyTabs = CreateWindowEx(WS_EX_CONTROLPARENT, L"EmitterProps", NULL, WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE | TCS_FOCUSNEVER | WS_TABSTOP,
				4, 4, SIDEBAR_WIDTH, 514, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
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
			// 7 cells now: file new/open/save (0..2), ground/heat (3..4),
			// undo/redo (5..6). See tasks/extend_toolbar1_bmp.ps1 for the
			// bitmap-extension pattern.
			HIMAGELIST hImgList = ImageList_Create(16, 16, ILC_COLOR24 | ILC_MASK, 7, 0);
			ImageList_AddMasked(hImgList, hBmpToolbar, RGB(0,128,128));
			DeleteObject(hBmpToolbar);
			SendMessage(info->hToolbar, TB_SETIMAGELIST, 0, (LPARAM)hImgList);

			TBBUTTON buttons[] = {
				{0, 0, 0, BTNS_SEP},
				{0, ID_FILE_NEW,  TBSTATE_ENABLED, BTNS_BUTTON},
				{1, ID_FILE_OPEN, TBSTATE_ENABLED, BTNS_BUTTON},
				{2, ID_FILE_SAVE, TBSTATE_ENABLED, BTNS_BUTTON},
				{0, 0, 0, BTNS_SEP},
				{5, ID_EDIT_UNDO, 0,                BTNS_BUTTON},
				{6, ID_EDIT_REDO, 0,                BTNS_BUTTON},
				{0, 0, 0, BTNS_SEP},
				{3, ID_VIEW_SHOWGROUND, TBSTATE_ENABLED | TBSTATE_CHECKED, BTNS_CHECK},
				{4, ID_VIEW_DEBUGHEAT,  TBSTATE_ENABLED, BTNS_CHECK},
			};
			SendMessage(info->hToolbar, TB_ADDBUTTONS, 10, (LPARAM)&buttons);
			
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
			
			if ((info->hBackgroundBtn = CreateWindowEx(0, L"ColorButton", NULL, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
				0, 0, 24, 24, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
			{
				return -1;
			}

            // Create the "leave particles" check box
            if ((info->hLeaveParticles = CreateWindow(L"BUTTON", LoadString(IDS_LABEL_LEAVE_PARTICLES).c_str(), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                0, 0, 300, 16, hWnd, NULL, pcs->hInstance, NULL)) == NULL)
            {
                return -1;
            }
            SendMessage(info->hLeaveParticles, WM_SETFONT, (WPARAM)hFont, FALSE);

			SetEmitterInfo(info);
            AppendHistory(info, hWnd);
			ShowWindow(info->hTrackEditors[0], SW_SHOW);
            SetFocus(info->hEmitterList);
			break;
		}

        case WM_CLOSE:
            if (DoCloseFile(info))
            {
                DestroyWindow(hWnd);
            }
            return 0;

		case WM_DESTROY:
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
			if (idx >= info->mods.size()) break;
			const ModEntry& m = info->mods[idx];

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
			if (dis->CtlType != ODT_MENU) break;
			if (dis->itemID < ID_MOD_FIRST || dis->itemID > ID_MOD_LAST) break;

			size_t idx = (size_t)dis->itemData;
			if (idx >= info->mods.size()) break;
			const ModEntry& m = info->mods[idx];

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
                        info->mods = DiscoverMods(info->gameRoots);
                        // If our currently-selected mod no longer exists, fall back to Unmodded
                        bool stillExists = info->selectedModPath.empty();
                        for (const ModEntry& m : info->mods)
                        {
                            if (_wcsicmp(m.path.c_str(), info->selectedModPath.c_str()) == 0)
                            {
                                stillExists = true;
                                break;
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
				else if (code == CBN_CHANGE)
				{
					if (hControl == info->hBackgroundBtn && info->engine != NULL)
					{
						// The background color has changed
						info->engine->SetBackground(ColorButton_GetColor(hControl));
                        RedrawWindow(info->hRenderWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                        // Persist. Also save the ChooseColor palette: even on
                        // Cancel the user may have defined custom slots, and
                        // CBN_CHANGE fires for every interim value during the
                        // dialog — by the time we land here the palette is
                        // final. Cheap (~64-byte write).
                        WriteBackgroundColor(info->engine->GetBackground());
                        COLORREF currentCustom[16];
                        ColorButton_GetCustomColors(currentCustom);
                        WriteCustomColors(currentCustom);
					}
				}
                else if (code == BN_CLICKED)
                {
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
                GetClientRect(info->hLeaveParticles, &checkbox);
                GetClientRect(info->hBackgroundLabel, &label);
                int height = max(max(24, checkbox.bottom), label.bottom);
                MoveWindow(info->hLeaveParticles, props.right + 8, top + 4 + (height - checkbox.bottom) / 2, checkbox.right, label.bottom, TRUE);
				MoveWindow(info->hBackgroundBtn,   LOWORD(lParam) - 28, top + 4 + (height - 24) / 2, 24, 24, TRUE);
				MoveWindow(info->hBackgroundLabel, LOWORD(lParam) - 32 - label.right, top + 4 + (height - label.bottom) / 2, label.right, label.bottom, TRUE);

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
			}
			return 0;
        }
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// Calculates the 3D position of the intersection of the cursor with Z = 0
static void GetCursorPos3D(Engine* engine, short x, short y, D3DXVECTOR3& position)
{
	D3DXVECTOR3  front, back;
	D3DVIEWPORT9 viewport;
	D3DXMATRIX   world;
	D3DXMatrixIdentity(&world);
	engine->GetViewPort(&viewport);

	D3DXVec3Unproject(&front, &D3DXVECTOR3(x, y, 0.0f), &viewport, &engine->GetProjectionMatrix(), &engine->GetViewMatrix(), &world);
	D3DXVec3Unproject(&back,  &D3DXVECTOR3(x, y, 0.9f), &viewport, &engine->GetProjectionMatrix(), &engine->GetViewMatrix(), &world);

	D3DXPLANE plane(0,0,1,0);
	D3DXPlaneIntersectLine(&position, &plane, &front, &back);
}

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
			Render(info);
			break;

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
// Mods support
//

// Read the user-set nickname for a given mod path from the registry.
// Returns empty string if no nickname is set.
static wstring ReadModNickname(const wstring& modPath)
{
	wstring nickname;
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor\\ModNicknames", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
	{
		TCHAR  buf[256] = {0};
		DWORD  type;
		DWORD  size = sizeof(buf);
		if (RegQueryValueEx(hKey, modPath.c_str(), NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
		{
			nickname = buf;
		}
		RegCloseKey(hKey);
	}
	return nickname;
}

static void WriteModNickname(const wstring& modPath, const wstring& nickname)
{
	HKEY hKey;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor\\ModNicknames", 0, NULL,
	                   REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
	{
		if (nickname.empty())
		{
			RegDeleteValue(hKey, modPath.c_str());
		}
		else
		{
			RegSetValueEx(hKey, modPath.c_str(), 0, REG_SZ,
			              (const BYTE*)nickname.c_str(),
			              (DWORD)((nickname.size() + 1) * sizeof(TCHAR)));
		}
		RegCloseKey(hKey);
	}
}

static wstring ReadLastMod()
{
	wstring path;
	HKEY hKey;
	if (RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
	{
		TCHAR  buf[MAX_PATH] = {0};
		DWORD  type;
		DWORD  size = sizeof(buf);
		if (RegQueryValueEx(hKey, L"LastMod", NULL, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ)
		{
			path = buf;
		}
		RegCloseKey(hKey);
	}
	return path;
}

static void WriteLastMod(const wstring& modPath)
{
	HKEY hKey;
	if (RegCreateKeyEx(HKEY_CURRENT_USER, L"Software\\AloParticleEditor", 0, NULL,
	                   REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, L"LastMod", 0, REG_SZ,
		              (const BYTE*)modPath.c_str(),
		              (DWORD)((modPath.size() + 1) * sizeof(TCHAR)));
		RegCloseKey(hKey);
	}
}

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
        RegDeleteValue(hKey, L"CustomColors");
        RegDeleteValue(hKey, L"SpawnerConfig");
        RegDeleteValue(hKey, L"SpawnerDialogPos");
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

    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_X, 0.0f, posLim, 0.5f, cfg.jitterPosition.x);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_Y, 0.0f, posLim, 0.5f, cfg.jitterPosition.y);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_POS_Z, 0.0f, posLim, 0.5f, cfg.jitterPosition.z);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_VEL_X, 0.0f, posLim, 0.5f, cfg.jitterVelocity.x);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_VEL_Y, 0.0f, posLim, 0.5f, cfg.jitterVelocity.y);
    ConfigureFloatSpinner(hDlg, IDC_SPAWNER_JIT_VEL_Z, 0.0f, posLim, 0.5f, cfg.jitterVelocity.z);

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
    cfg.jitterVelocity = D3DXVECTOR3(GetFloatSpinner(hDlg, IDC_SPAWNER_JIT_VEL_X),
                                     GetFloatSpinner(hDlg, IDC_SPAWNER_JIT_VEL_Y),
                                     GetFloatSpinner(hDlg, IDC_SPAWNER_JIT_VEL_Z));
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

// Returns the display label for a mod (nickname if set, else folder name).
static const wstring& ModDisplayLabel(const ModEntry& m)
{
	return m.nickname.empty() ? m.folderName : m.nickname;
}

// Scan a single Mods\ directory for subfolders and append entries.
static void ScanModsDir(const wstring& modsRoot, bool isFoC, vector<ModEntry>& out)
{
	wstring search = modsRoot;
	if (!search.empty() && search.back() != L'\\') search += L'\\';
	search += L"*";

	WIN32_FIND_DATA fd;
	HANDLE hFind = FindFirstFile(search.c_str(), &fd);
	if (hFind == INVALID_HANDLE_VALUE) return;

	do
	{
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if (fd.cFileName[0] == L'.') continue;
		if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;

		ModEntry e;
		e.folderName = fd.cFileName;
		e.path       = modsRoot;
		if (!e.path.empty() && e.path.back() != L'\\') e.path += L'\\';
		e.path      += e.folderName;
		e.isFoC      = isFoC;
		e.nickname   = ReadModNickname(e.path);
		out.push_back(e);
	}
	while (FindNextFile(hFind, &fd));

	FindClose(hFind);
}

// Discover mods under the given game roots (corruption\Mods and GameData\Mods),
// sorted alphabetically by display label within each category (FoC first, then base).
static vector<ModEntry> DiscoverMods(const vector<wstring>& gameRoots)
{
	vector<ModEntry> mods;
	for (const wstring& root : gameRoots)
	{
		wstring trimmed = root;
		while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();

		// Determine flavor by leaf folder name
		size_t sep  = trimmed.find_last_of(L"\\/");
		wstring leaf = (sep == wstring::npos) ? trimmed : trimmed.substr(sep + 1);

		bool isFoC;
		if (_wcsicmp(leaf.c_str(), L"corruption") == 0) isFoC = true;
		else if (_wcsicmp(leaf.c_str(), L"GameData") == 0) isFoC = false;
		else continue;

		wstring modsDir = trimmed + L"\\Mods";
		if (PathIsDirectory(modsDir.c_str()))
		{
			ScanModsDir(modsDir, isFoC, mods);
		}
	}

	// Sort: FoC mods first, then base game; within each, alphabetical by folder
	// name (which is what's displayed first; nicknames are a parenthetical).
	std::sort(mods.begin(), mods.end(), [](const ModEntry& a, const ModEntry& b) {
		if (a.isFoC != b.isFoC) return a.isFoC && !b.isFoC;
		return _wcsicmp(a.folderName.c_str(), b.folderName.c_str()) < 0;
	});

	printf("[Mods] DiscoverMods: scanned %zu game roots, found %zu mods\n",
	       gameRoots.size(), mods.size()); fflush(stdout);
	return mods;
}

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

	// Top: Unmodded radio item
	UINT noneFlags = MF_STRING | (info->selectedModPath.empty() ? MF_CHECKED : MF_UNCHECKED);
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
	for (size_t i = 0; i < info->mods.size(); i++)
	{
		const ModEntry& m = info->mods[i];
		UINT id      = ID_MOD_FIRST + (UINT)i;
		bool checked = (_wcsicmp(m.path.c_str(), info->selectedModPath.c_str()) == 0);

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

// Apply a mod selection: update FileManager, clear caches, persist,
// rebuild menu, redraw render area.
static void SelectMod(APPLICATION_INFO* info, const wstring& modPath)
{
	info->selectedModPath = modPath;

	if (info->fileManager) info->fileManager->SetModPath(modPath);

	WriteLastMod(modPath);
	RebuildModsMenu(info);

	printf("[Mods] Selected: %S\n", modPath.empty() ? L"(unmodded)" : modPath.c_str()); fflush(stdout);

	// Hot-swap shaders + textures so the new mod folder takes effect without
	// restart. Shader reload may fail on a malformed mod shader; in that case
	// we keep the previous set and surface the failure on the status bar.
	if (info->engine != NULL)
	{
		if (!info->engine->ReloadShaders())
		{
			SendMessage(info->hStatusBar, SB_SETTEXT, 4,
			            (LPARAM)L"Mod shader reload failed — keeping previous shaders");
		}
		info->engine->ReloadTextures();
	}
	if (info->hRenderWnd != NULL)
	{
		InvalidateRect(info->hRenderWnd, NULL, TRUE);
	}
}

// Find the mod entry corresponding to a given menu command ID, or NULL.
static ModEntry* FindModById(APPLICATION_INFO* info, UINT id)
{
	if (id < ID_MOD_FIRST || id > ID_MOD_LAST) return NULL;
	UINT idx = id - ID_MOD_FIRST;
	if (idx >= info->mods.size()) return NULL;
	return &info->mods[idx];
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
		info->mods           = DiscoverMods(gameRoots);

		// Restore the previously-selected mod, if any (and it still exists)
		wstring savedMod = ReadLastMod();
		if (!savedMod.empty() && PathIsDirectory(savedMod.c_str()))
		{
			info->selectedModPath = savedMod;
			fileManager->SetModPath(savedMod);
			printf("[Mods] Restored from registry: %S\n", savedMod.c_str()); fflush(stdout);
		}
		else
		{
			info->selectedModPath = L"";
			if (!savedMod.empty())
			{
				printf("[Mods] Saved mod path no longer exists, falling back to unmodded: %S\n", savedMod.c_str()); fflush(stdout);
			}
		}
		RebuildModsMenu(info);

		// Create the rendering engine
        try
        {
		    info->engine = new Engine(info->hMainWnd, info->hRenderWnd, textureManager, shaderManager);

            // View settings persisted across sessions: pull from registry,
            // fall back to engine defaults when no value stored. Defaults
            // are passed to the helpers so a fresh registry behaves
            // identically to before this feature.
            info->engine->SetBackground(ReadBackgroundColor(info->engine->GetBackground()));
            info->engine->SetGround    (ReadShowGround    (info->engine->GetGround()));
            COLORREF persistedCustom[16];
            if (ReadCustomColors(persistedCustom)) ColorButton_SetCustomColors(persistedCustom);

            // Spawner config is intentionally NOT restored from registry —
            // it resets to defaults at the start of each session per
            // user preference. Dialog position is still restored so the
            // window doesn't bounce around between launches.
            ReadSpawnerDialogPos(info->spawnerWindowRect);

            ColorButton_SetColor(info->hBackgroundBtn, info->engine->GetBackground());

            // Sync the ground-toggle toolbar button to the (possibly
            // restored-from-registry) engine state. The TBBUTTON definition
            // hardcodes TBSTATE_CHECKED at init time; without this re-sync
            // a persisted "ground off" looks like the toolbar lying — render
            // is correct but the button still shows pressed, and the next
            // click does the wrong thing.
            SendMessage(info->hToolbar, TB_CHECKBUTTON, ID_VIEW_SHOWGROUND,
                        MAKELONG(info->engine->GetGround() ? TRUE : FALSE, 0));
        }
        catch (exception&)
        {
            DestroyWindow(info->hRenderWnd);
        }
		
		DoCloseFile(info);

        bool loaded = false;
        if (info->engine != NULL)
        {
			// See if a file was specified
			for (size_t i = 1; i < argv.size(); i++)
			{
				if (PathFileExists(argv[i].c_str()) && !PathIsDirectory(argv[i].c_str()))
				{
					loaded = LoadFile(info, argv[i]);
					break;
				}
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
				if (!consumed && !TranslateAccelerator(info->hMainWnd, hAccel, &msg) && !IsDialogMessage(info->hMainWnd, &msg))
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
	wcx.hIcon         = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_LOGO));
	wcx.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wcx.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
	wcx.lpszMenuName  = MAKEINTRESOURCE(IDR_MENU1);
	wcx.lpszClassName = L"ParticleEditor";
	wcx.hIconSm       = NULL;

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
	info.hModsMenu				= NULL;
	info.hMenuFont				= NULL;
	info.hMenuItalicFont		= NULL;
	info.spawner                = new SpawnerDriver();
	info.hSpawnerDlg            = NULL;
	info.spawnerVisible         = false;
	memset(&info.spawnerWindowRect, 0, sizeof(info.spawnerWindowRect));

#ifdef NDEBUG
 	try
#endif
    {
		// Initialize UI classes and create windows
		if (!UI_Initialize(hInstance) || !InitializeWindows(&info))
		{
			//throw wruntime_error(LoadString(IDS_ERROR_UI_INITIALIZATION));
		}

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
