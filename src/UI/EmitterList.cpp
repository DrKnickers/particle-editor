#include "UI/UI.h"
#include "utils.h"
#include "Rescale.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
using namespace std;

// Registered clipboard format
static UINT CF_PARTICLE_EMITTER = 0;

// Returns "<base>_<n>" where <n> is one more than the highest numeric suffix
// already in use among emitters in `system` whose name matches `<base>` or
// `<base>_<digits>`. If `sourceName` itself ends in `_<digits>` that suffix
// is stripped first, so duplicating "Foo_3" repeatedly yields Foo_4, Foo_5
// rather than Foo_3_1, Foo_3_1_1. An emitter named exactly `<base>` counts
// as n=0 for the purpose of picking the next free slot.
static std::string GenerateDuplicateName(const ParticleSystem* system, const std::string& sourceName)
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

struct EmitterListControl
{
	HWND			hDialog;
    HMENU           hNewEmitterMenu;
    HMENU           hEmitterContextMenu;
    HWND            hTree;
    HWND            hToolbar;
	ParticleSystem* system;
    ParticleSystem::Emitter* selection;

    // Drag-drop reorder state. dragSource non-NULL means a drag is in
    // progress; the public EmitterList_IsDragging accessor reads from
    // this. All drag-drop teardown goes through EndDrag so each field
    // gets cleared in one place — see Risks 1-3 in tasks/todo.md.
    ParticleSystem::Emitter* dragSource;
    HIMAGELIST               dragImageList;
    HTREEITEM                dragInsertTarget;  // current insertion-mark target
    bool                     dragInsertAfter;   // false = above, true = below
    UINT_PTR                 dragScrollTimer;   // 0 = no autoscroll, else timer id
    int                      dragScrollDir;     // -1 up, +1 down (only valid when timer set)
};

static void NotifyParent(EmitterListControl* control, UINT code)
{
    if (code == ELN_SELCHANGED || code == ELN_LISTCHANGED)
    {
        // Enable buttons on toolbar
        SendMessage(control->hToolbar, TB_ENABLEBUTTON, ID_DELETE_EMITTER,            control->selection != NULL);
        SendMessage(control->hToolbar, TB_ENABLEBUTTON, ID_TOGGLE_EMITTER_VISIBILITY, control->selection != NULL);

        // Move Up/Down: only meaningful for a root emitter with a neighboring
        // root in that direction. Recomputed on both SELCHANGED and
        // LISTCHANGED because a reorder doesn't fire SELCHANGED but does
        // change which neighbors exist.
        bool canUp = false, canDown = false;
        if (control->system != NULL && control->selection != NULL && control->selection->parent == NULL)
        {
            const std::vector<ParticleSystem::Emitter*>& emitters = control->system->getEmitters();
            bool seenSelf = false;
            for (size_t i = 0; i < emitters.size(); i++)
            {
                if (emitters[i]->parent != NULL) continue;
                if (emitters[i] == control->selection) { seenSelf = true; continue; }
                if (seenSelf) { canDown = true; break; }
                canUp = true;
            }
        }
        SendMessage(control->hToolbar, TB_ENABLEBUTTON, ID_MOVE_EMITTER_UP,   canUp);
        SendMessage(control->hToolbar, TB_ENABLEBUTTON, ID_MOVE_EMITTER_DOWN, canDown);
    }

	NMHDR hdr;
	hdr.code     = code;
    hdr.hwndFrom = control->hDialog;
    hdr.idFrom   = GetDlgCtrlID(hdr.hwndFrom);
    SendMessage(GetParent(hdr.hwndFrom), WM_NOTIFY, (WPARAM)hdr.idFrom, (LPARAM)&hdr );
}

static bool CopyEmitter(HWND hWnd, EmitterListControl* control)
{
    if (control->selection != NULL)
    {
        // Copy the emitter
        MemoryFile* memfile = new MemoryFile;
        try
        {
            ChunkWriter writer(memfile);
            control->selection->copy(writer);

            HGLOBAL hMemory = GlobalAlloc(GMEM_MOVEABLE, memfile->size()); 
            if (hMemory == NULL) 
            { 
                return false;
            } 
     
            // Lock the handle and copy the text to the buffer. 
            void* data = GlobalLock(hMemory); 
            memfile->seek(0);
            memfile->read(data, memfile->size());
            GlobalUnlock(hMemory); 
     
            // Place the handle on the clipboard. 
            OpenClipboard(hWnd);
            SetClipboardData(CF_PARTICLE_EMITTER, hMemory); 
            memfile->Release();
            CloseClipboard();
        }
        catch (...)
        {
            memfile->Release();
            CloseClipboard();
            MessageBox(NULL, LoadString(IDS_ERROR_EMITTER_COPY).c_str(), NULL, MB_OK | MB_ICONHAND);
            return false;
        }
    }
    return true;
}

static bool PasteEmitter(HWND hWnd, EmitterListControl* control, void (*func)(HWND, const ParticleSystem::Emitter&) = &EmitterList_AddRootEmitter)
{
    // Paste an emitter
    OpenClipboard(hWnd);
    HANDLE hMemory = GetClipboardData(CF_PARTICLE_EMITTER);
    if (hMemory == NULL)
    {
        CloseClipboard();
        return false;
    }

    MemoryFile* memfile = new MemoryFile();
    try
    {
        void* data = GlobalLock(hMemory); 
        memfile->write(data, (unsigned long)GlobalSize(hMemory));
        memfile->seek(0);
        GlobalUnlock(hMemory); 

        // Create the emitter
        ChunkReader reader(memfile);
        ParticleSystem::Emitter emitter(reader);
        // Suffix the name so paste — like duplicate — never collides with an
        // existing emitter, including the source if it's still present.
        emitter.name = GenerateDuplicateName(control->system, emitter.name);
        (*func)(hWnd, emitter);
        memfile->Release();
        CloseClipboard();
    }
    catch (...)
    {
        memfile->Release();
        CloseClipboard();
        MessageBox(NULL, LoadString(IDS_ERROR_EMITTER_PASTE).c_str(), NULL, MB_OK | MB_ICONHAND);
        return false;
    }
    return true;
}

static int GetTreeNodeIcon(const ParticleSystem* system, size_t iEmitter)
{
    const ParticleSystem::Emitter& emitter = system->getEmitter(iEmitter);
    int base = (emitter.visible ? 0 : 4);
    if (emitter.parent != NULL && emitter.parent->spawnOnDeath == emitter.index)
    {
        // Spawn-on-death type particle
        return base + 1;
    }
    
    if (emitter.useBursts && emitter.nBursts > 0)
    {
        // Finite amount of particles
        return base + 2;
    }

    // Infinite (weather or normal)
    return base + (emitter.isWeatherParticle ? 3 : 0);
}

static HTREEITEM InsertTreeItem(EmitterListControl* control, HTREEITEM hParent, ParticleSystem::Emitter* emitter)
{
    wstring name = AnsiToWide(emitter->name);
    TVINSERTSTRUCT tvis;
    tvis.hParent        = hParent;
    tvis.hInsertAfter   = TVI_LAST;
    tvis.item.mask      = TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN | TVIF_TEXT;
    tvis.item.pszText   = (LPWSTR)name.c_str();
    tvis.item.lParam    = (LPARAM)emitter;
    tvis.item.cChildren = 1;
    tvis.item.iImage    = GetTreeNodeIcon(control->system, emitter->index);
    tvis.item.iSelectedImage = tvis.item.iImage;
    HTREEITEM hItem = TreeView_InsertItem(control->hTree, &tvis);
    if (hItem != NULL)
    {
        TreeView_Expand(control->hTree, hParent, TVE_EXPAND);
    }
    return hItem;
}

// Window procedure for subclassed edit box during treeview label edit
// Workaround for bug in Knowledge Base item Q130691.
static LRESULT WINAPI LabelEditProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;
    }
    WNDPROC wndProc = (WNDPROC)GetProp(hWnd, L"Old_WindowProc");
    return CallWindowProc(wndProc, hWnd, uMsg, wParam, lParam);
}

// Forward decls — defined further down. Used by the drop-commit path
// to rebuild the tree post-move and re-select the moved emitter
// (OnParticleSystemChange auto-selects the first root and would
// otherwise lose the user's drag target).
static HTREEITEM FindTreeItemByEmitter(HWND hTree, HTREEITEM hItem,
                                        const ParticleSystem::Emitter* target);
static void      OnParticleSystemChange(EmitterListControl* control, ParticleSystem* system);
static void      NotifyParent(EmitterListControl* control, UINT code);

//
// Drag-and-drop reorder for the emitter tree
//
// State lives on EmitterListControl::dragSource and friends. The tree's
// subclassed wndproc (EmitterTreeViewWindowProc, below) handles the
// per-message updates while a drag is in progress; TVN_BEGINDRAG lives
// in DlgEmitterListProc's WM_NOTIFY handler. All teardown — capture
// release, image-list destroy, timer kill, insertion-mark clear —
// goes through EndDrag so each step happens in exactly one place.
// See tasks/todo.md for the full plan and risk mitigations.

static const UINT_PTR AUTOSCROLL_TIMER_ID    = 1;
static const UINT     AUTOSCROLL_INTERVAL_MS = 50;
static const int      AUTOSCROLL_HOT_ZONE_PX = 16;

// Position of `root` in the root-only sequence (Nth emitter with
// parent==NULL, counting from 0). SIZE_MAX if not a root in `sys`.
// Use this — not the flat m_emitters index — when comparing against
// drop-target gap indices, since children sit between roots in the
// flat vector and would skew the count.
static size_t RootIndexOf(const ParticleSystem* sys, const ParticleSystem::Emitter* root)
{
    if (sys == NULL || root == NULL) return (size_t)-1;
    const std::vector<ParticleSystem::Emitter*>& emitters = sys->getEmitters();
    size_t k = 0;
    for (size_t i = 0; i < emitters.size(); i++)
    {
        if (emitters[i]->parent != NULL) continue;
        if (emitters[i] == root) return k;
        k++;
    }
    return (size_t)-1;
}

// "Gap index" 0..numRoots:
//   gap 0          = above first root
//   gap numRoots   = below last root
//   gap K (0<K<N)  = between root K-1 and root K
struct DropTarget
{
    size_t    gap;        // valid range [0, numRoots]
    HTREEITEM hTarget;    // tree item the insertion mark anchors to (NULL when above first / below last)
    bool      after;      // false = insertion-mark above hTarget, true = below
    bool      valid;
};

// Compute the drop target for cursor `pt` (in tree client coords).
// Walks up from the hovered item to its root ancestor, then picks
// above/below based on the item-rect midpoint. Cursor outside the
// tree client area or below the last item gets the
// above-first / below-last special cases. Children-as-target are
// detected by the caller (we still walk to the root ancestor, but
// the caller refuses the drop if the cursor was hovering a child
// — see RowInDeepZone below) per the locked-in scope.
static DropTarget ComputeDropTarget(HWND hTree, POINT pt, size_t numRoots)
{
    DropTarget out = { 0, NULL, false, false };
    if (numRoots == 0) return out;

    // Cursor outside the tree's client area entirely → invalid.
    RECT clientRect;
    GetClientRect(hTree, &clientRect);
    if (pt.x < clientRect.left || pt.x >= clientRect.right
        || pt.y < clientRect.top || pt.y >= clientRect.bottom)
    {
        return out;
    }

    TVHITTESTINFO tvht;
    tvht.pt = pt;
    HTREEITEM hHit = TreeView_HitTest(hTree, &tvht);

    if (hHit == NULL)
    {
        // Empty area inside the tree. If above the first item → gap 0,
        // anchored above first root. If below the last → gap numRoots,
        // anchored below last root.
        HTREEITEM hFirst = TreeView_GetRoot(hTree);
        if (hFirst == NULL) return out;
        RECT firstRect;
        TreeView_GetItemRect(hTree, hFirst, &firstRect, TRUE);
        if (pt.y < firstRect.top)
        {
            out.gap = 0; out.hTarget = hFirst; out.after = false; out.valid = true;
            return out;
        }
        // Walk to the last root in the tree (visible last sibling).
        HTREEITEM hLast = hFirst;
        while (HTREEITEM h = TreeView_GetNextSibling(hTree, hLast)) hLast = h;
        RECT lastRect;
        TreeView_GetItemRect(hTree, hLast, &lastRect, TRUE);
        if (pt.y >= lastRect.bottom)
        {
            out.gap = numRoots; out.hTarget = hLast; out.after = true; out.valid = true;
            return out;
        }
        return out;  // somewhere weird (between collapsed-children gap?) — invalid
    }

    // Walk up to the root ancestor of the hovered item.
    HTREEITEM hRoot = hHit;
    while (HTREEITEM hParent = TreeView_GetParent(hTree, hRoot)) hRoot = hParent;
    bool hoverIsChild = (hHit != hRoot);

    // Find the root's position in the root-sibling list of the tree
    // (this matches the data-model root index because OnParticleSystemChange
    // iterates m_emitters in order when populating root siblings).
    size_t rootIdx = 0;
    for (HTREEITEM h = TreeView_GetRoot(hTree); h != NULL && h != hRoot;
         h = TreeView_GetNextSibling(hTree, h))
    {
        rootIdx++;
    }

    RECT itemRect;
    TreeView_GetItemRect(hTree, hRoot, &itemRect, TRUE);
    int midY = (itemRect.top + itemRect.bottom) / 2;

    if (hoverIsChild)
    {
        // Cursor is over a child node — invalid drop target per the
        // locked-in scope (reparenting is the next ROADMAP entry, not
        // this PR). Caller shows IDC_NO and clears the insertion mark.
        return out;
    }

    out.hTarget = hRoot;
    if (pt.y < midY)
    {
        out.gap   = rootIdx;
        out.after = false;
    }
    else
    {
        out.gap   = rootIdx + 1;
        out.after = true;
    }
    out.valid = true;
    return out;
}

static void EndDrag(EmitterListControl* control)
{
#ifndef NDEBUG
    // Only emit the END diagnostic if there was actually a drag to end —
    // EndDrag is called defensively from OnParticleSystemChange / WM_DESTROY
    // and we don't want spurious "[DnD] END" prints in the common no-op case.
    bool wasActive = (control->dragSource != NULL
                   || control->dragImageList != NULL
                   || control->dragScrollTimer != 0);
#endif

    // Clear dragSource FIRST so the WM_CAPTURECHANGED that ReleaseCapture
    // fires synchronously below sees dragSource == NULL and short-circuits
    // its own EndDrag call (preventing harmless-but-confusing re-entry).
    control->dragSource       = NULL;
    control->dragInsertTarget = NULL;
    control->dragInsertAfter  = false;

    if (control->dragScrollTimer != 0)
    {
        KillTimer(control->hTree, control->dragScrollTimer);
        control->dragScrollTimer = 0;
        control->dragScrollDir   = 0;
    }
    if (control->dragImageList != NULL)
    {
        ImageList_DragLeave(control->hTree);
        ImageList_EndDrag();
        ImageList_Destroy(control->dragImageList);
        control->dragImageList = NULL;
    }
    TreeView_SetInsertMark(control->hTree, NULL, FALSE);
    if (GetCapture() == control->hTree) ReleaseCapture();

#ifndef NDEBUG
    if (wasActive)
    {
        DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        printf("[DnD] END   gdi=%lu\n", gdiAfter); fflush(stdout);
    }
#endif
}

// Update insertion mark + cursor based on the cursor's current
// position. Call from WM_MOUSEMOVE and from the autoscroll WM_TIMER
// handler. Returns the computed DropTarget so callers can reuse it
// (e.g. WM_LBUTTONUP commits the move based on the same target).
static DropTarget UpdateInsertMark(EmitterListControl* control, POINT pt)
{
    size_t numRoots = 0;
    if (control->system != NULL)
    {
        const std::vector<ParticleSystem::Emitter*>& emitters = control->system->getEmitters();
        for (size_t i = 0; i < emitters.size(); i++) if (emitters[i]->parent == NULL) numRoots++;
    }

    DropTarget t = ComputeDropTarget(control->hTree, pt, numRoots);

    // No-op detection: dropping on the source's own gap doesn't change
    // anything; show IDC_NO and clear the insertion mark so the user
    // gets unambiguous feedback.
    if (t.valid && control->dragSource != NULL)
    {
        size_t srcIdx = RootIndexOf(control->system, control->dragSource);
        if (srcIdx != (size_t)-1 && (t.gap == srcIdx || t.gap == srcIdx + 1))
        {
            t.valid = false;
        }
    }

    if (t.valid)
    {
        TreeView_SetInsertMark(control->hTree, t.hTarget, t.after ? TRUE : FALSE);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        control->dragInsertTarget = t.hTarget;
        control->dragInsertAfter  = t.after;
    }
    else
    {
        TreeView_SetInsertMark(control->hTree, NULL, FALSE);
        SetCursor(LoadCursor(NULL, IDC_NO));
        control->dragInsertTarget = NULL;
    }
    return t;
}

bool EmitterList_IsDragging(HWND hWnd)
{
    EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    return control != NULL && control->dragSource != NULL;
}

static LRESULT CALLBACK EmitterTreeViewWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	switch (uMsg)
	{
        case WM_CHAR:
            // Handle Ctrl+[C,V,X] chars
            if (GetKeyState(VK_CONTROL) & 0x80000000)
            {
                switch (wParam)
                {
                    case 1 + 'C' - 'A': SendMessage(hWnd, WM_COPY, 0, 0); return 0;
                    case 1 + 'X' - 'A': SendMessage(hWnd, WM_CUT,  0, 0); return 0;
                    case 1 + 'V' - 'A': SendMessage(hWnd, WM_PASTE, 0, 0); return 0;
                }
            }
            break;

        case WM_COPY:  CopyEmitter(hWnd, control);       break;
        case WM_CUT:   if (!CopyEmitter(hWnd, control))  break;
        case WM_CLEAR: EmitterList_DeleteEmitter(hWnd);  break;
        case WM_PASTE: PasteEmitter(hWnd, control);      break;

        case WM_MOUSEMOVE:
            if (control != NULL && control->dragSource != NULL)
            {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

                // Auto-scroll: cursor in top/bottom hot zone starts (or
                // continues) the timer. The timer-handler does the actual
                // scroll + insert-mark + ghost re-anchor atomically; the
                // mousemove just keeps the timer alive while the cursor
                // sits in the zone (mousemoves stop firing when the
                // cursor is stationary, so per-mousemove scroll alone
                // would stall).
                RECT cr;
                GetClientRect(hWnd, &cr);
                int dir = 0;
                if (pt.y < cr.top    + AUTOSCROLL_HOT_ZONE_PX) dir = -1;
                if (pt.y > cr.bottom - AUTOSCROLL_HOT_ZONE_PX) dir = +1;
                if (dir != 0 && control->dragScrollTimer == 0)
                {
                    control->dragScrollDir   = dir;
                    control->dragScrollTimer = SetTimer(hWnd, AUTOSCROLL_TIMER_ID, AUTOSCROLL_INTERVAL_MS, NULL);
                }
                else if (dir == 0 && control->dragScrollTimer != 0)
                {
                    KillTimer(hWnd, control->dragScrollTimer);
                    control->dragScrollTimer = 0;
                }
                else if (dir != 0)
                {
                    control->dragScrollDir = dir;
                }

                ImageList_DragMove(pt.x, pt.y);
                UpdateInsertMark(control, pt);
                return 0;
            }
            break;

        case WM_TIMER:
            if (control != NULL && control->dragSource != NULL
                && wParam == AUTOSCROLL_TIMER_ID)
            {
                // Atomic scroll + recompute. The cursor hasn't moved
                // (no WM_MOUSEMOVE fired) but visible items shift, so
                // we re-anchor the ghost to absolute screen coords and
                // recompute the insertion mark against the new layout.
                SendMessage(hWnd, WM_VSCROLL,
                            control->dragScrollDir < 0 ? SB_LINEUP : SB_LINEDOWN, 0);
                POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
                ImageList_DragMove(pt.x, pt.y);
                UpdateInsertMark(control, pt);
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (control != NULL && control->dragSource != NULL)
            {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                DropTarget t = UpdateInsertMark(control, pt);
                ParticleSystem::Emitter* moved = control->dragSource;
                bool committed = false;
                if (t.valid && control->system != NULL)
                {
                    committed = control->system->moveEmitterToRootIndex(moved, t.gap);
                }

                EndDrag(control);

                if (committed)
                {
                    // Tree shape may have changed — rebuild and re-select
                    // the moved emitter (OnParticleSystemChange auto-selects
                    // first root, which is wrong if the user moved a
                    // different root). Then notify so the editor flags the
                    // file as changed and the undo system captures.
                    OnParticleSystemChange(control, control->system);
                    HTREEITEM hMoved = FindTreeItemByEmitter(
                        control->hTree, TreeView_GetRoot(control->hTree), moved);
                    if (hMoved != NULL)
                    {
                        control->selection = moved;
                        TreeView_SelectItem(control->hTree, hMoved);
                    }
                    NotifyParent(control, ELN_LISTCHANGED);
                }
                return 0;
            }
            break;

        case WM_KEYDOWN:
            if (control != NULL && control->dragSource != NULL && wParam == VK_ESCAPE)
            {
                EndDrag(control);
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            // Capture stolen (Alt+Tab, focus theft, our own ReleaseCapture
            // call). EndDrag is idempotent — null-checks every step — so
            // this safely no-ops when capture was already released by us.
            if (control != NULL && control->dragSource != NULL
                && (HWND)lParam != hWnd)
            {
                EndDrag(control);
            }
            break;
    }
    WNDPROC wndProc = (WNDPROC)GetProp(hWnd, L"Old_WindowProc");
    return CallWindowProc(wndProc, hWnd, uMsg, wParam, lParam);
}

static INT_PTR WINAPI DlgEmitterListProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
	switch (uMsg)
	{
		case WM_INITDIALOG:
		{
			control = (EmitterListControl*)lParam;
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)control);

            HINSTANCE hInstance = (HINSTANCE)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_HINSTANCE);

            //
            // Initialize treeview
            //
            control->hTree     = GetDlgItem(hWnd, IDC_TREE1);
            HBITMAP hBmpTree = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_EMITTER_LIST));
            HIMAGELIST hImgList = ImageList_Create(12, 10, ILC_COLOR24 | ILC_MASK, 8, 0);
            ImageList_AddMasked(hImgList, hBmpTree, RGB(0,128,128));
            DeleteObject(hBmpTree);
    		TreeView_SetImageList(control->hTree, hImgList, TVSIL_NORMAL);

            // Subclass window proc for Cut/Copy/Paste operations
            WNDPROC wndProc = (WNDPROC)(LONG_PTR)GetWindowLongPtr(control->hTree, GWLP_WNDPROC);
            SetProp(control->hTree, L"Old_WindowProc", (HANDLE)wndProc);
            SetWindowLongPtr(control->hTree, GWLP_USERDATA, (LONG_PTR)control);
            SetWindowLongPtr(control->hTree, GWLP_WNDPROC,  (LONG_PTR)EmitterTreeViewWindowProc);

			//
			// Initialize toolbar
			//
			control->hToolbar = GetDlgItem(hWnd, IDC_TOOLBAR1);
			SendMessage(control->hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
			HBITMAP hBmpTb = LoadBitmap(hInstance, MAKEINTRESOURCE(IDR_EMITTER_TOOLBAR));
			hImgList = ImageList_Create(16, 15, ILC_COLOR24 | ILC_MASK, 7, 0);
			ImageList_AddMasked(hImgList, hBmpTb, RGB(0,128,128));
			DeleteObject(hBmpTb);
            SendMessage(control->hToolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DRAWDDARROWS);
			SendMessage(control->hToolbar, TB_SETIMAGELIST, 0, (LPARAM)hImgList);

			// Layout: [New ▾] | [Delete] | [▲][▼] | [👁] | [Show All][Hide All]
			// Move Up / Move Down sit in their own group, separated from the
			// destructive cluster on the left and the visibility cluster on
			// the right. They operate on the selected emitter, so they belong
			// near Delete; not at the far end with the bulk-action buttons.
			TBBUTTON buttons[11] = {
				{0, ID_NEW_EMITTER_ROOT,          TBSTATE_ENABLED, BTNS_DROPDOWN},
				{0, 0,                            TBSTATE_ENABLED, BTNS_SEP},
				{1, ID_DELETE_EMITTER,            TBSTATE_ENABLED, BTNS_BUTTON},
				{0, 0,                            TBSTATE_ENABLED, BTNS_SEP},
				{5, ID_MOVE_EMITTER_UP,           TBSTATE_ENABLED, BTNS_BUTTON},
				{6, ID_MOVE_EMITTER_DOWN,         TBSTATE_ENABLED, BTNS_BUTTON},
				{0, 0,                            TBSTATE_ENABLED, BTNS_SEP},
				{2, ID_TOGGLE_EMITTER_VISIBILITY, TBSTATE_ENABLED, BTNS_BUTTON},
				{0, 0,                            TBSTATE_ENABLED, BTNS_SEP},
                {3, ID_SHOW_ALL_EMITTERS, TBSTATE_ENABLED, BTNS_BUTTON},
                {4, ID_HIDE_ALL_EMITTERS, TBSTATE_ENABLED, BTNS_BUTTON},
			};
			SendMessage(control->hToolbar, TB_ADDBUTTONS, 11, (LPARAM)buttons);

            // Load resources
            control->hNewEmitterMenu     = LoadMenu(hInstance, MAKEINTRESOURCE(IDR_NEW_EMITTER_MENU));
            control->hEmitterContextMenu = LoadMenu(hInstance, MAKEINTRESOURCE(IDR_EMITTER_CONTEXT_MENU));
            break;
        }

        case WM_DESTROY:
            // Defensive: if the user Alt+F4'd mid-drag, tear down the
            // drag image list and timer so we don't leak GDI handles
            // until process exit. EndDrag is idempotent.
            EndDrag(control);
            DestroyMenu(control->hEmitterContextMenu);
            DestroyMenu(control->hNewEmitterMenu);
            break;

		case WM_COMMAND:
			if (lParam != NULL)
			{
				HWND hCtrl = (HWND)lParam;
				switch (HIWORD(wParam))
				{
					case BN_CLICKED:
                        if (hCtrl == control->hToolbar)
						{
							// A toolbar button has been clicked
							switch (LOWORD(wParam))
							{
                                case ID_NEW_EMITTER_ROOT:          EmitterList_AddRootEmitter(hWnd); break;
                                case ID_NEW_EMITTER_LIFETIME:      EmitterList_AddLifetimeEmitter(hWnd); break;
                                case ID_NEW_EMITTER_DEATH:         EmitterList_AddDeathEmitter(hWnd); break;
                                case ID_TOGGLE_EMITTER_VISIBILITY: EmitterList_ToggleEmitterVisibility(hWnd); break;
                                case ID_DELETE_EMITTER:            EmitterList_DeleteEmitter(hWnd); break;
                                case ID_MOVE_EMITTER_UP:           EmitterList_MoveEmitter(hWnd, -1); break;
                                case ID_MOVE_EMITTER_DOWN:         EmitterList_MoveEmitter(hWnd, +1); break;
                                case ID_SHOW_ALL_EMITTERS:         EmitterList_SetAllEmitterVisibility(hWnd, true);  break;
                                case ID_HIDE_ALL_EMITTERS:         EmitterList_SetAllEmitterVisibility(hWnd, false); break;
                            }
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
                        UINT id;
                        UINT text;
                    }
                    tooltips[] =
					{
                        {ID_NEW_EMITTER_ROOT,          IDS_TOOLTIP_EMITTER_NEW},
                        {ID_DELETE_EMITTER,            IDS_TOOLTIP_EMITTER_DELETE},
                        {ID_TOGGLE_EMITTER_VISIBILITY, IDS_TOOLTIP_EMITTER_TOGGLE},
                        {ID_SHOW_ALL_EMITTERS,         IDS_TOOLTIP_EMITTERS_SHOW},
                        {ID_HIDE_ALL_EMITTERS,         IDS_TOOLTIP_EMITTERS_HIDE},
                        {ID_MOVE_EMITTER_UP,           IDS_TOOLTIP_EMITTER_MOVE_UP},
                        {ID_MOVE_EMITTER_DOWN,         IDS_TOOLTIP_EMITTER_MOVE_DOWN},
                        {0, NULL}
					};

                    for (int i = 0; tooltips[i].text != NULL; i++)
                    {
                        if (tooltips[i].id == hdr->idFrom)
                        {
                            nmdi->hinst    = (HINSTANCE)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_HINSTANCE);
					        nmdi->lpszText = MAKEINTRESOURCE(tooltips[i].text);
                            break;
                        }
                    }

                    break;
				}

                case NM_CLICK:
                    if (hdr->hwndFrom == control->hTree)
                    {
                        // Get item under cursor
                        TVHITTESTINFO tvht;
                        GetCursorPos(&tvht.pt);
                        ScreenToClient(control->hTree, &tvht.pt);
                        TreeView_HitTest(control->hTree, &tvht);
                        
                        if (tvht.hItem != NULL && tvht.flags & TVHT_ONITEMICON)
                        {
                            // User clicked an emitter's icon; toggle visibility
                            EmitterList_ToggleEmitterVisibility(hWnd, tvht.hItem);
                        }
                    }
                    break;

                case NM_RCLICK:
                    if (hdr->hwndFrom == control->hTree)
                    {
                        POINT cursor;
                        GetCursorPos(&cursor);

                        // Get item under cursor
                        TVHITTESTINFO tvht;
                        tvht.pt = cursor;
                        ScreenToClient(control->hTree, &tvht.pt);
                        TreeView_HitTest(control->hTree, &tvht);
                        TreeView_SelectItem(control->hTree, tvht.hItem);

                        HMENU hPopupMenu = GetSubMenu(control->hEmitterContextMenu, 0);
                        EnableMenuItem(hPopupMenu, ID_EDIT_COPY,       MF_BYCOMMAND | (control->selection != NULL ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_EDIT_CUT ,       MF_BYCOMMAND | (control->selection != NULL ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_EDIT_DELETE,     MF_BYCOMMAND | (control->selection != NULL ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_EDIT_PASTE,      MF_BYCOMMAND | (IsClipboardFormatAvailable(CF_PARTICLE_EMITTER) ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_EMITTER_RENAME,  MF_BYCOMMAND | (control->selection != NULL ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_EMITTER_RESCALE, MF_BYCOMMAND | (control->selection != NULL ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_NEW_EMITTER_LIFETIME, MF_BYCOMMAND | (control->selection != NULL && control->selection->spawnDuringLife == -1 ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_NEW_EMITTER_DEATH,    MF_BYCOMMAND | (control->selection != NULL && control->selection->spawnOnDeath    == -1 ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_PASTEAS_LIFETIME, MF_BYCOMMAND | (control->selection != NULL && control->selection->spawnDuringLife == -1 ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_PASTEAS_DEATH,    MF_BYCOMMAND | (control->selection != NULL && control->selection->spawnOnDeath    == -1 ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_TOGGLE_EMITTER_VISIBILITY, MF_BYCOMMAND | (control->selection != NULL ? MF_ENABLED : MF_GRAYED));

                        // Move Up/Down: enabled only when a root emitter is
                        // selected and a neighboring root exists in that
                        // direction. Children can't be reordered (each parent
                        // has named slots, not a sibling list).
                        bool canUp = false, canDown = false;
                        if (control->selection != NULL && control->selection->parent == NULL)
                        {
                            const std::vector<ParticleSystem::Emitter*>& emitters = control->system->getEmitters();
                            bool seenSelf = false;
                            for (size_t i = 0; i < emitters.size(); i++)
                            {
                                if (emitters[i]->parent != NULL) continue;
                                if (emitters[i] == control->selection) { seenSelf = true; continue; }
                                if (seenSelf) { canDown = true; break; }
                                canUp = true;  // some root preceded us
                            }
                        }
                        EnableMenuItem(hPopupMenu, ID_MOVE_EMITTER_UP,   MF_BYCOMMAND | (canUp   ? MF_ENABLED : MF_GRAYED));
                        EnableMenuItem(hPopupMenu, ID_MOVE_EMITTER_DOWN, MF_BYCOMMAND | (canDown ? MF_ENABLED : MF_GRAYED));

                        INT id = TrackPopupMenuEx(hPopupMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, cursor.x, cursor.y, hWnd, NULL);
                        switch (id)
                        {
                            case ID_NEW_EMITTER_ROOT:
                            case ID_NEW_EMITTER_LIFETIME:
                            case ID_NEW_EMITTER_DEATH:
                                SendMessage(hWnd, WM_COMMAND, MAKELONG(id, BN_CLICKED), (LPARAM)control->hToolbar);
                                break;

                            case ID_EDIT_CUT:    SendMessage(control->hTree, WM_CUT,   0, 0); break;
                            case ID_EDIT_COPY:   SendMessage(control->hTree, WM_COPY,  0, 0); break;
                            case ID_EDIT_DELETE: SendMessage(control->hTree, WM_CLEAR, 0, 0); break;
                            case ID_EDIT_PASTE:  SendMessage(control->hTree, WM_PASTE, 0, 0); break;
                            case ID_EMITTER_DUPLICATE: EmitterList_DuplicateEmitter(hWnd); break;
                            case ID_MOVE_EMITTER_UP:   EmitterList_MoveEmitter(hWnd, -1); break;
                            case ID_MOVE_EMITTER_DOWN: EmitterList_MoveEmitter(hWnd, +1); break;
                            case ID_PASTEAS_LIFETIME: PasteEmitter(hWnd, control, &EmitterList_AddLifetimeEmitter); break;
                            case ID_PASTEAS_DEATH:    PasteEmitter(hWnd, control, &EmitterList_AddDeathEmitter); break;
                            case ID_EMITTER_RENAME:   TreeView_EditLabel(control->hTree, tvht.hItem); break;
                            case ID_TOGGLE_EMITTER_VISIBILITY: EmitterList_ToggleEmitterVisibility(hWnd); break;
                            case ID_EMITTER_RESCALE:
                                if (RescaleEmitter(hWnd, control->selection))
                                {
                                    NotifyParent(control, ELN_LISTCHANGED);
                                }
                                break;
                        }
                    }
                    break;

                case TVN_SELCHANGING:
                {
					NMTREEVIEW* nmtv = (NMTREEVIEW*)lParam;
                    if (nmtv->action == TVC_BYMOUSE)
                    {
                        // Get item under cursor
                        TVHITTESTINFO tvht;
                        GetCursorPos(&tvht.pt);
                        ScreenToClient(control->hTree, &tvht.pt);
                        TreeView_HitTest(control->hTree, &tvht);

                        if (tvht.flags & TVHT_ONITEMICON)
                        {
                            // Don't change selection
                            SetWindowLongPtr(hWnd, DWLP_MSGRESULT, TRUE);
                            return TRUE;
                        }
                    }
                    break;
                }

				case TVN_SELCHANGED:
                {
					NMTREEVIEW* nmtv   = (NMTREEVIEW*)lParam;
                    control->selection = (nmtv->itemNew.hItem != NULL) ? (ParticleSystem::Emitter*)nmtv->itemNew.lParam : NULL;

                	NotifyParent(control, ELN_SELCHANGED);
					break;
                }

                case TBN_DROPDOWN:
                {
                    NMTOOLBAR* nmtb = (NMTOOLBAR*)lParam;

                    // Enable items as applicable
                    HMENU hPopupMenu = GetSubMenu(control->hNewEmitterMenu, 0);
                    UINT nEnable1 = (control->selection != NULL && control->selection->spawnDuringLife == -1) ? MF_ENABLED : MF_GRAYED;
                    UINT nEnable2 = (control->selection != NULL && control->selection->spawnOnDeath    == -1) ? MF_ENABLED : MF_GRAYED;
                    EnableMenuItem(hPopupMenu, ID_NEW_EMITTER_LIFETIME, nEnable1 | MF_BYCOMMAND);
                    EnableMenuItem(hPopupMenu, ID_NEW_EMITTER_DEATH,    nEnable2 | MF_BYCOMMAND);

                    // Get position and show popup
                    POINT pnt = {nmtb->rcButton.left, nmtb->rcButton.bottom};
                    ClientToScreen(control->hToolbar, &pnt);
                    INT id = TrackPopupMenuEx(hPopupMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pnt.x, pnt.y, hWnd, NULL);
                    switch (id)
                    {
                        case ID_NEW_EMITTER_ROOT:
                        case ID_NEW_EMITTER_LIFETIME:
                        case ID_NEW_EMITTER_DEATH:
                            SendMessage(hWnd, WM_COMMAND, MAKELONG(id, BN_CLICKED), (LPARAM)control->hToolbar);
                            break;
                    }
                    return TBDDRET_DEFAULT;
                }

                case TVN_BEGINDRAG:
                {
                    // Initiate a drag-drop reorder. Win32's drag-threshold
                    // gate (SM_CXDRAG / SM_CYDRAG) already prevents this
                    // from firing on a click-without-movement, so a
                    // simple selection click never reaches here.
                    NMTREEVIEW* nmtv = (NMTREEVIEW*)lParam;
                    ParticleSystem::Emitter* src =
                        (ParticleSystem::Emitter*)nmtv->itemNew.lParam;

                    // Refuse if (a) not a root — children fill named
                    // parent slots and aren't reorderable, (b) only one
                    // root — nowhere to drop, (c) a label edit is in
                    // progress — drag mid-rename would be confusing.
                    if (src == NULL || src->parent != NULL) break;
                    if (TreeView_GetEditControl(control->hTree) != NULL) break;
                    if (control->system != NULL)
                    {
                        size_t numRoots = 0;
                        const std::vector<ParticleSystem::Emitter*>& v = control->system->getEmitters();
                        for (size_t i = 0; i < v.size(); i++) if (v[i]->parent == NULL) numRoots++;
                        if (numRoots < 2) break;
                    }

                    HIMAGELIST hDragList = TreeView_CreateDragImage(control->hTree, nmtv->itemNew.hItem);
                    if (hDragList == NULL) break;

#ifndef NDEBUG
                    // Snapshot GDI handle count at drag start. Compared in
                    // EndDrag — a positive delta points at a leak in the
                    // drag-image / insertion-mark / timer cleanup. Cheap
                    // to leave on in Debug; prints once per drag.
                    DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                    printf("[DnD] BEGIN src='%s' gdi=%lu\n", src->name.c_str(), gdiBefore);
                    fflush(stdout);
#endif

                    control->dragSource    = src;
                    control->dragImageList = hDragList;

                    // Compute hotspot relative to the dragged item so the
                    // ghost tracks the cursor at the same offset where it
                    // was picked up. ImageList_BeginDrag's hotspot is the
                    // pixel within the image that anchors to the cursor.
                    RECT itemRect;
                    if (TreeView_GetItemRect(control->hTree, nmtv->itemNew.hItem, &itemRect, TRUE))
                    {
                        ImageList_BeginDrag(hDragList, 0,
                            nmtv->ptDrag.x - itemRect.left,
                            nmtv->ptDrag.y - itemRect.top);
                    }
                    else
                    {
                        ImageList_BeginDrag(hDragList, 0, 0, 0);
                    }
                    ImageList_DragEnter(control->hTree, nmtv->ptDrag.x, nmtv->ptDrag.y);

                    SetCapture(control->hTree);
                    break;
                }

                case TVN_BEGINLABELEDIT:
                {
                    // Workaround for bug in Knowledge Base item Q130691; subclass the edit control
                    HWND hEdit = TreeView_GetEditControl(control->hTree);
                    WNDPROC wndProc = (WNDPROC)(LONG_PTR)GetWindowLongPtr(hEdit, GWLP_WNDPROC);
                    SetProp(hEdit, L"Old_WindowProc", (HANDLE)wndProc);
                    SetWindowLongPtr(hEdit, GWLP_WNDPROC, (LONG_PTR)LabelEditProc);
                    break;
                }

                case TVN_ENDLABELEDIT:
                {
                    NMTVDISPINFO* nmtvdi = (NMTVDISPINFO*)lParam;
                    if (nmtvdi->item.pszText != NULL)
                    {
                        control->selection->name = WideToAnsi(nmtvdi->item.pszText);
                        TreeView_SetItem(control->hTree, &nmtvdi->item);
                        NotifyParent(control, ELN_LISTCHANGED);
                        NotifyParent(control, ELN_SELCHANGED);
                    }
                    break;
                }

				case TVN_KEYDOWN:
				{
					NMTVKEYDOWN* pnkd = (NMTVKEYDOWN*)lParam;
					switch (pnkd->wVKey)
					{
						case VK_F2:     EmitterList_RenameEmitter(hWnd); return 0;
						case VK_DELETE: EmitterList_DeleteEmitter(hWnd); return 0;
					}
					break;
                }

                default:
			        // Pass notification on
			        SendMessage(GetParent(hWnd), WM_NOTIFY, wParam, lParam);
                    break;
            }
			break;
		}

        case WM_SIZE:
        {
            RECT toolbar;
            GetClientRect(control->hToolbar, &toolbar);
            MoveWindow(control->hToolbar, 0, HIWORD(lParam) - toolbar.bottom, LOWORD(lParam), toolbar.bottom, TRUE);
            MoveWindow(control->hTree,    0, 0, LOWORD(lParam), HIWORD(lParam) - toolbar.bottom, TRUE);
            break;
        }
    }
    return FALSE;
}

static EmitterListControl* CreateEmitterListControl(HWND hOwner, HINSTANCE hInstance)
{
	EmitterListControl* control = new EmitterListControl;
	if (control != NULL)
	{
        control->selection         = NULL;
		control->system            = NULL;
        control->dragSource        = NULL;
        control->dragImageList     = NULL;
        control->dragInsertTarget  = NULL;
        control->dragInsertAfter   = false;
        control->dragScrollTimer   = 0;
        control->dragScrollDir     = 0;
        control->hDialog   = CreateDialogParam(hInstance, MAKEINTRESOURCE(IDD_EMITTER_LIST), hOwner, DlgEmitterListProc, (LPARAM)control);
        if (control->hDialog == NULL)
        {
            delete control;
            return NULL;
        }
        ShowWindow(control->hDialog, SW_SHOW);
	}
	return control;
}

static LRESULT CALLBACK EmitterListWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	switch (uMsg)
	{
		case WM_CREATE:
		{
			CREATESTRUCT* pcs = (CREATESTRUCT*)lParam;
			control = CreateEmitterListControl(hWnd, pcs->hInstance);
			if (control == NULL)
			{
				return -1;
			}
			SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)control);
			break;
		}

		case WM_DESTROY:
			break;

		case WM_NOTIFY:
			// Pass notification on
			SendMessage(GetParent(hWnd), WM_NOTIFY, wParam, lParam);
			break;

		case WM_SIZE:
            if (control != NULL)
            {
                MoveWindow(control->hDialog, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
            }
			break;

		case WM_SETFONT:
            SendMessage(control->hDialog, uMsg, wParam, lParam);
			break;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static void OnParticleSystemChange_AddChildren(HWND hTree, ParticleSystem* system, HTREEITEM hNode, const ParticleSystem::Emitter* emitter, set<size_t>& index)
{
	if (emitter->spawnOnDeath != -1 || emitter->spawnDuringLife != -1)
	{
		TVINSERTSTRUCT tvis;
		tvis.hParent        = hNode;
		tvis.hInsertAfter   = TVI_LAST;
		tvis.item.mask      = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM | TVIF_STATE | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
		tvis.item.cChildren = 1;
		tvis.item.stateMask = TVIS_SELECTED;

		if (emitter->spawnDuringLife != -1)
		{
			index.erase(emitter->spawnDuringLife);
			const ParticleSystem::Emitter& onLife  = system->getEmitter(emitter->spawnDuringLife);
			wstring text = AnsiToWide(onLife.name);

            tvis.item.iImage  = GetTreeNodeIcon(system, onLife.index);
			tvis.item.pszText = (LPWSTR)text.c_str();
			tvis.item.lParam  = (LPARAM)&onLife;
			tvis.item.state   = 0;
            tvis.item.iSelectedImage = tvis.item.iImage;
			HTREEITEM hChild = TreeView_InsertItem(hTree, &tvis);
			OnParticleSystemChange_AddChildren(hTree, system, hChild, &onLife, index);
			TreeView_Expand(hTree, hChild, TVE_EXPAND);
		}

        if (emitter->spawnOnDeath != -1)
		{
			index.erase(emitter->spawnOnDeath);
			const ParticleSystem::Emitter& onDeath = system->getEmitter(emitter->spawnOnDeath);
			wstring text = AnsiToWide(onDeath.name);

            tvis.item.iImage  = GetTreeNodeIcon(system, onDeath.index);
			tvis.item.pszText = (LPWSTR)text.c_str();
            tvis.item.lParam  = (LPARAM)&onDeath;
			tvis.item.state   = 0;
            tvis.item.iSelectedImage = tvis.item.iImage;
			HTREEITEM hChild = TreeView_InsertItem(hTree, &tvis);
			OnParticleSystemChange_AddChildren(hTree, system, hChild, &onDeath, index);
			TreeView_Expand(hTree, hChild, TVE_EXPAND);
		}
    }
}

static void OnParticleSystemChange(EmitterListControl* control, ParticleSystem* system)
{
    // Defensive teardown: if a drag is somehow still active when the
    // particle system is being swapped (e.g. file open / new fired
    // mid-drag despite the accelerator gate), the drag's
    // dragSource pointer would dangle into the old system. EndDrag is
    // idempotent so a no-op when the common case (no drag) holds.
    EndDrag(control);

	// Fill the emitter list
	TreeView_DeleteAllItems(control->hTree);
    control->selection = NULL;
    if (system != NULL)
	{
		TVINSERTSTRUCT tvis;
		tvis.hParent        = NULL;
		tvis.hInsertAfter   = TVI_ROOT;
		tvis.item.mask      = TVIF_TEXT | TVIF_CHILDREN | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;

		const std::vector<ParticleSystem::Emitter*>& emitters = system->getEmitters();
		std::set<size_t> index;
		for (size_t i = 0; i < emitters.size(); i++)
		{
			index.insert(i);
		}

		while (!index.empty())
		{
			size_t i = *index.begin();
            if (emitters[i]->parent == NULL)
            {
			    wstring name = AnsiToWide(emitters[i]->name);
                tvis.item.iImage    = GetTreeNodeIcon(system, i);
			    tvis.item.cChildren = 1;
			    tvis.item.pszText   = (LPWSTR)name.c_str();
			    tvis.item.lParam    = (LPARAM)emitters[i];
			    tvis.item.state     = (i == 0) ? TVIS_SELECTED : 0;
                tvis.item.iSelectedImage = tvis.item.iImage;

			    HTREEITEM hChild = TreeView_InsertItem(control->hTree, &tvis);
			    OnParticleSystemChange_AddChildren(control->hTree, system, hChild, emitters[i], index);
			    TreeView_Expand(control->hTree, hChild, TVE_EXPAND);

                if (control->selection == NULL)
                {
                    control->selection = emitters[i];
                    TreeView_SelectItem(control->hTree, hChild);
                }
            }
 			index.erase(index.begin());
		}
	}
}


// Walk the tree depth-first looking for an item whose lParam == target.
// Returns NULL if not found.
static HTREEITEM FindTreeItemByEmitter(HWND hTree, HTREEITEM hItem,
                                        const ParticleSystem::Emitter* target)
{
    while (hItem != NULL)
    {
        TVITEM item;
        item.hItem = hItem;
        item.mask  = TVIF_PARAM;
        if (TreeView_GetItem(hTree, &item) && (ParticleSystem::Emitter*)item.lParam == target)
        {
            return hItem;
        }
        HTREEITEM hChild = TreeView_GetChild(hTree, hItem);
        if (hChild != NULL)
        {
            HTREEITEM found = FindTreeItemByEmitter(hTree, hChild, target);
            if (found != NULL) return found;
        }
        hItem = TreeView_GetNextSibling(hTree, hItem);
    }
    return NULL;
}

void EmitterList_SelectEmitter(HWND hWnd, ParticleSystem::Emitter* emitter)
{
    EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (control == NULL || emitter == NULL) return;

    HTREEITEM hRoot = TreeView_GetRoot(control->hTree);
    HTREEITEM hFound = FindTreeItemByEmitter(control->hTree, hRoot, emitter);
    if (hFound != NULL)
    {
        TreeView_SelectItem(control->hTree, hFound);
    }
}

void EmitterList_SetParticleSystem(HWND hWnd, ParticleSystem* system)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL)
	{
        control->system = NULL;
        OnParticleSystemChange(control, system);
        control->system = system;
        // OnParticleSystemChange auto-selects the first root via TreeView_
        // SelectItem, which fires TVN_SELCHANGED while control->system is
        // still NULL. The toolbar Move Up / Down enable logic depends on
        // control->system, so it needs a re-fire now that the system is in
        // place. (Delete / Visibility don't care; they only look at
        // control->selection, which was correct on the first fire.)
        NotifyParent(control, ELN_SELCHANGED);
    }
}

void EmitterList_AddRootEmitter(HWND hWnd, const ParticleSystem::Emitter& emitter)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL)
    {
        ParticleSystem::Emitter* pEmitter = control->system->addRootEmitter(emitter);
        if (pEmitter != NULL)
        {
            HTREEITEM hItem = InsertTreeItem(control, NULL, pEmitter);
            control->selection = pEmitter;
            NotifyParent(control, ELN_LISTCHANGED);
            TreeView_SelectItem(control->hTree, hItem);
        }
    }
}

void EmitterList_AddLifetimeEmitter(HWND hWnd, const ParticleSystem::Emitter& emitter)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL && control->selection != NULL)
    {
        ParticleSystem::Emitter* pEmitter = control->system->addLifetimeEmitter(control->selection, emitter);
        if (pEmitter != NULL)
        {
            HTREEITEM hItem = InsertTreeItem(control, TreeView_GetSelection(control->hTree), pEmitter);
            control->selection = pEmitter;
            NotifyParent(control, ELN_LISTCHANGED);
            TreeView_SelectItem(control->hTree, hItem);
        }
    }
}

void EmitterList_AddDeathEmitter(HWND hWnd, const ParticleSystem::Emitter& emitter)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL && control->selection != NULL)
    {
        ParticleSystem::Emitter* pEmitter = control->system->addDeathEmitter(control->selection, emitter);
        if (pEmitter != NULL)
        {
            HTREEITEM hItem = InsertTreeItem(control, TreeView_GetSelection(control->hTree), pEmitter);
            control->selection = pEmitter;
            NotifyParent(control, ELN_LISTCHANGED);
            TreeView_SelectItem(control->hTree, hItem);
        }
    }
}

void EmitterList_DeleteEmitter(HWND hWnd)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL && control->selection != NULL)
    {
        control->system->deleteEmitter(control->selection);
        TreeView_DeleteItem(control->hTree, TreeView_GetSelection(control->hTree));
        if (control->system->getEmitters().empty())
        {
            control->selection = NULL;
        }
        NotifyParent(control, ELN_LISTCHANGED);
        NotifyParent(control, ELN_SELCHANGED);
    }
}

void EmitterList_DuplicateEmitter(HWND hWnd)
{
    EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (control == NULL || control->selection == NULL) return;

    // Round-trip the source through the chunk serializer so the duplicate
    // starts with empty m_instances. A direct copy-construct would shallow-
    // copy that std::set and we'd end up double-freeing live EmitterInstance
    // pointers when either the original or the duplicate is later deleted.
    // Same trick the Copy / Paste flow already uses safely.
    MemoryFile* memfile = new MemoryFile;
    ParticleSystem::Emitter* pEmitter = NULL;
    try
    {
        ChunkWriter writer(memfile);
        control->selection->copy(writer);

        memfile->seek(0);
        ChunkReader reader(memfile);
        ParticleSystem::Emitter cleanCopy(reader);

        // Suffix the name with the next free _<n> so the duplicate is
        // visually distinct in the tree and never collides with an existing
        // emitter. See GenerateDuplicateName for the increment rule.
        cleanCopy.name = GenerateDuplicateName(control->system, control->selection->name);

        pEmitter = control->system->insertEmitterAfter(control->selection, cleanCopy);
        memfile->Release();
    }
    catch (...)
    {
        memfile->Release();
        MessageBox(NULL, LoadString(IDS_ERROR_EMITTER_COPY).c_str(), NULL, MB_OK | MB_ICONHAND);
        return;
    }

    if (pEmitter == NULL) return;

    // Tree insertion. The duplicate is always a tree-root (parent=NULL was
    // set by insertEmitterAfter). If the source is also a root, we can place
    // the new tree item directly after the source's tree item — that's the
    // "right below the original" UX the roadmap describes. If the source is
    // a child emitter (its tree item lives under a parent), `hInsertAfter`
    // would be at a different tree level than `hParent=NULL`; the only
    // legal placement at root level in that case is TVI_LAST.
    HTREEITEM hAfter = TVI_LAST;
    if (control->selection->parent == NULL)
    {
        hAfter = TreeView_GetSelection(control->hTree);
    }

    wstring name = AnsiToWide(pEmitter->name);
    TVINSERTSTRUCT tvis;
    tvis.hParent             = NULL;
    tvis.hInsertAfter        = hAfter;
    tvis.item.mask           = TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_CHILDREN | TVIF_TEXT;
    tvis.item.pszText        = (LPWSTR)name.c_str();
    tvis.item.lParam         = (LPARAM)pEmitter;
    tvis.item.cChildren      = 1;
    tvis.item.iImage         = GetTreeNodeIcon(control->system, pEmitter->index);
    tvis.item.iSelectedImage = tvis.item.iImage;
    HTREEITEM hItem = TreeView_InsertItem(control->hTree, &tvis);

    control->selection = pEmitter;
    NotifyParent(control, ELN_LISTCHANGED);
    TreeView_SelectItem(control->hTree, hItem);
}

void EmitterList_MoveEmitter(HWND hWnd, int direction)
{
    EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (control == NULL || control->system == NULL || control->selection == NULL) return;

    ParticleSystem::Emitter* moved = control->selection;
    if (!control->system->moveEmitter(moved, direction)) return;

    // The vector reordered; rebuild the tree. OnParticleSystemChange clears
    // selection along the way, so we restore it by walking the tree for the
    // HTREEITEM whose lParam matches `moved`. The moved emitter is always a
    // root after a successful moveEmitter (children can't be reordered), so
    // searching the top level is sufficient.
    OnParticleSystemChange(control, control->system);

    HTREEITEM hItem = TreeView_GetRoot(control->hTree);
    while (hItem != NULL)
    {
        TVITEM item;
        item.mask  = TVIF_PARAM;
        item.hItem = hItem;
        if (TreeView_GetItem(control->hTree, &item) && (ParticleSystem::Emitter*)item.lParam == moved)
        {
            control->selection = moved;
            TreeView_SelectItem(control->hTree, hItem);
            break;
        }
        hItem = TreeView_GetNextSibling(control->hTree, hItem);
    }

    NotifyParent(control, ELN_LISTCHANGED);
    NotifyParent(control, ELN_SELCHANGED);
}

void EmitterList_RenameEmitter(HWND hWnd)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL && control->selection != NULL)
    {
        TreeView_EditLabel(control->hTree, TreeView_GetSelection(control->hTree));
    }
}

ParticleSystem::Emitter* EmitterList_GetSelection(HWND hWnd)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL)
	{
        return control->selection;
    }
    return NULL;
}

static void EmitterList_SetAllEmitterVisibility(HWND hWnd, HTREEITEM hItem, bool visible)
{
    while (hItem != NULL)
    {
        // Get item
        TVITEM item;
        item.hItem = hItem;
        item.mask  = TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        TreeView_GetItem(hWnd, &item);

        ParticleSystem::Emitter* emitter = (ParticleSystem::Emitter*)item.lParam;
        emitter->visible = visible;

        // Set new image
        item.iSelectedImage = item.iImage = (item.iImage % 4) + (emitter->visible ? 0 : 4);
        TreeView_SetItem(hWnd, &item);

        EmitterList_SetAllEmitterVisibility(hWnd, TreeView_GetChild(hWnd, hItem), visible);
        hItem = TreeView_GetNextSibling(hWnd, hItem);
    }
}

void EmitterList_SetAllEmitterVisibility(HWND hWnd, bool visible)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL)
	{
        EmitterList_SetAllEmitterVisibility(control->hTree, TreeView_GetRoot(control->hTree), visible);
    }
}

void EmitterList_ToggleEmitterVisibility(HWND hWnd, HTREEITEM hItem)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL)
	{
        if (hItem == NULL)
        {
            hItem = TreeView_GetSelection(control->hTree);
        }

        if (hItem != NULL)
        {
            // Get item
            TVITEM item;
            item.hItem = hItem;
            item.mask  = TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            TreeView_GetItem(control->hTree, &item);

            ParticleSystem::Emitter* emitter = (ParticleSystem::Emitter*)item.lParam;
            emitter->visible = !emitter->visible;

            // Set new image
            item.iSelectedImage = item.iImage = (item.iImage % 4) + (emitter->visible ? 0 : 4);
            TreeView_SetItem(control->hTree, &item);
        }
    }
}

void EmitterList_SelectionChanged(HWND hWnd)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL)
	{
        wstring name = AnsiToWide(control->system->getEmitter(control->selection->index).name);

		TVITEM item;
        item.hItem   = TreeView_GetSelection(control->hTree);
        item.mask    = TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_TEXT;
        item.pszText = (LPWSTR)name.c_str();
        item.iImage  = GetTreeNodeIcon(control->system, control->selection->index);
        item.iSelectedImage = item.iImage;
        TreeView_SetItem(control->hTree, &item);
    }
}

bool EmitterList_HasFocus(HWND hWnd)
{
	EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd,GWLP_USERDATA);
	if (control != NULL)
    {
        return GetFocus() == control->hTree;
    }
    return false;
}

bool EmitterList_Initialize(HINSTANCE hInstance)
{
	WNDCLASSEX wcx;
	wcx.cbSize        = sizeof(WNDCLASSEX);
	wcx.style         = CS_HREDRAW | CS_VREDRAW;
	wcx.lpfnWndProc   = EmitterListWindowProc;
	wcx.cbClsExtra    = 0;
	wcx.cbWndExtra    = 0;
	wcx.hInstance     = hInstance;
	wcx.hIcon         = NULL;
	wcx.hCursor       = NULL;
	wcx.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wcx.lpszMenuName  = NULL;
	wcx.lpszClassName = L"EmitterList";
	wcx.hIconSm       = NULL;
	
	if (!RegisterClassEx(&wcx))
	{
		return false;
	}

    // Register clipboard format
    CF_PARTICLE_EMITTER = RegisterClipboardFormat(L"Alamo_ParticleEmitter");

	return true;
}
