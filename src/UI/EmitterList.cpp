#include "UI/UI.h"
#include "utils.h"
#include "Rescale.h"
#include "LinkGroup.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <cwchar>       // swprintf
#include <algorithm>    // std::find, std::sort
#include <cmath>        // pow (palette contrast metric)
#include <map>          // BracketLayout group accumulator

//: AlphaBlend for hover-row tinting at 15%. msimg32 is part of the
// Windows SDK; pragma-link here keeps the .vcxproj input list unchanged.
#pragma comment(lib, "msimg32.lib")

using namespace std;

// ----------------------------------------------------------------------------
// link-group visual bracket — layout cache + palette + helpers
//
// Bracket painting (lane lines, member dots, hover tint, click-to-select-group)
// is rendered by the existing NM_CUSTOMDRAW handler. The layout is computed
// once per paint at CDDS_PREPAINT, cached on the EmitterListControl, and
// reused by hit-tests for hover (WM_MOUSEMOVE) and click (WM_LBUTTONDOWN).
//
// Lane allocation: greedy interval scheduling sorted by minY ascending —
// each group takes the lowest-index lane whose previous occupant ended
// above this group's first member.
//
// Palette: Tableau-derived 12-colour categorical palette, luminance-shifted
// where raw Tableau values failed thin-line contrast on COLOR_WINDOW (white).
// First 6 entries are perceptually-distinct hues for the common case
// (realistic systems mostly use <= 6 link groups). Entries 7-12 cover the
// tail. Group N uses palette[N % 12]; group 13 reuses group 1's colour and
// is differentiated by lane position.
//
// High-Contrast theme: when SystemParametersInfo(SPI_GETHIGHCONTRAST)
// reports active, every bracket paints in COLOR_HIGHLIGHT instead. Lane
// position + the [L<n>] text prefix carry group identity in HC mode.

struct BracketLayout
{
    struct Member
    {
        LONG                              centreY;   // tree-client coords
        ParticleSystem::Emitter*          emitter;
    };
    struct Group
    {
        uint32_t                          groupId;
        int                               lane;       // 0..numLanes-1
        COLORREF                          colour;
        std::vector<Member>               members;    // built in tree pre-order
        LONG                              minY;
        LONG                              maxY;
    };
    std::vector<Group>                    groups;
    int                                   numLanes;
    int                                   laneWidth;        // px, DPI-aware
    int                                   dotRadius;        // px
    int                                   stubLength;       // px
    int                                   strokeWidth;      // px (hover thickens to *2)
    int                                   rightEdgeOffset;  // px from tree client.left to lane-0 dotX (lanes extend RIGHTWARD)
    int                                   scrollOriginY;    // SB_VERT pos at rebuild — for stale detection
    bool                                  hcMode;           // built under High-Contrast theme
    bool                                  valid;            // false = needs rebuild at next CDDS_PREPAINT

    // Previous-paint snapshot, used to detect bracket geometry shifts
    // between paints (typically caused by rename / link mutation / tree
    // resize). When any of these changes, RebuildBracketLayout queues a
    // full-tree invalidate so partial repaints (e.g. after the tree
    // invalidates only the renamed row) can't leave stale bracket
    // pixels at the previous X position.
    int                                   prevRightEdgeOffset;
    int                                   prevLaneWidth;
    int                                   prevNumLanes;
};

// 12-colour palette. Sources noted per entry — see contrast verification
// at startup (DebugVerifyBracketPalette below). All values target >= 3:1
// against COLOR_WINDOW (WCAG 2.1 SC 1.4.11 non-text contrast).
static const COLORREF kBracketPalette[12] =
{
    RGB(0x1F, 0x4E, 0x79),   //  0  blue       (Tableau blue, darkened)
    RGB(0xC7, 0x57, 0x0A),   //  1  orange     (Tableau orange, darkened)
    RGB(0x2E, 0x7D, 0x32),   //  2  green      (Material 700 green)
    RGB(0xC6, 0x28, 0x28),   //  3  red        (Material 700 red)
    RGB(0x6A, 0x1B, 0x9A),   //  4  purple     (Material 700 purple)
    RGB(0x5D, 0x40, 0x37),   //  5  brown      (Material 700 brown)
    RGB(0xAD, 0x14, 0x57),   //  6  magenta    (Material 700 pink)
    RGB(0x00, 0x69, 0x5C),   //  7  teal       (Material 800 teal)
    RGB(0x82, 0x77, 0x17),   //  8  olive      (Material 800 lime)
    RGB(0x28, 0x35, 0x93),   //  9  indigo     (Material 800 indigo)
    RGB(0x00, 0x83, 0x8F),   // 10  cyan       (Material 800 cyan)
    RGB(0x88, 0x0E, 0x4F),   // 11  rose       (Material 900 pink)
};

// True if the user has the Windows High-Contrast accessibility theme
// active. Read live (cheap; pure registry lookup behind SPI).
// re-evaluates on WM_THEMECHANGED / WM_SETTINGCHANGE by setting
// BracketLayout::valid = false; the next paint rebuilds with the
// fresh hcMode flag.
static bool IsHighContrastActive()
{
    HIGHCONTRAST hc;
    hc.cbSize = sizeof(hc);
    hc.dwFlags = 0;
    hc.lpszDefaultScheme = NULL;
    if (SystemParametersInfo(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    return false;
}

#ifndef NDEBUG
// WCAG 2.1 relative-luminance computation. Used by the startup palette
// verifier to confirm every kBracketPalette entry hits >= 3:1 contrast
// against COLOR_WINDOW (the default-theme bracket background). Failing
// entries print with a LOW_CONTRAST tag — that's a signal to revisit
// the palette, not an assert (the build still ships).
static double SrgbToLinear(double c)
{
    return (c <= 0.03928) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

static double RelLuminance(COLORREF c)
{
    double r = SrgbToLinear(GetRValue(c) / 255.0);
    double g = SrgbToLinear(GetGValue(c) / 255.0);
    double b = SrgbToLinear(GetBValue(c) / 255.0);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static double WcagContrastRatio(COLORREF a, COLORREF b)
{
    double la = RelLuminance(a);
    double lb = RelLuminance(b);
    double brighter = (la > lb) ? la : lb;
    double dimmer   = (la > lb) ? lb : la;
    return (brighter + 0.05) / (dimmer + 0.05);
}

// One-shot palette contrast check. Called from CreateEmitterListControl
// the first time it runs (guarded by a static bool). Prints one line per
// palette entry plus the HC mode at startup.
static void DebugVerifyBracketPalette()
{
    static bool done = false;
    if (done) return;
    done = true;

    const COLORREF background = GetSysColor(COLOR_WINDOW);
    printf("[Link] HC mode active=%s\n", IsHighContrastActive() ? "true" : "false");
    for (int i = 0; i < 12; ++i)
    {
        COLORREF c = kBracketPalette[i];
        double ratio = WcagContrastRatio(c, background);
        const char* tag = (ratio >= 3.0) ? "OK" : "LOW_CONTRAST";
        printf("[Link] palette contrast: i=%-2d rgb=#%02X%02X%02X ratio=%.2f %s\n",
               i,
               GetRValue(c), GetGValue(c), GetBValue(c),
               ratio,
               tag);
    }
    fflush(stdout);
}
#endif

// ----------------------------------------------------------------------------

// Registered clipboard format
static UINT CF_PARTICLE_EMITTER = 0;

// Compose the display name shown in the tree for a given emitter. When
// the emitter belongs to a link group, the name is prefixed with
// `[L<n>] ` so the user can identify group membership from the row
// text alone (independent of any custom-draw bracket affordance).
//
// Group ID is one-based and shown as displayed; the data-model ID is
// the same value (link-group IDs are stable across save/load and
// unique within a particle system).
static std::wstring FormatEmitterDisplayName(const ParticleSystem::Emitter* emitter)
{
    if (emitter == NULL) return L"";
    std::wstring name = AnsiToWide(emitter->name);
    if (emitter->linkGroup == 0) return name;
    wchar_t prefix[32];
    swprintf(prefix, 32, L"[L%u] ", emitter->linkGroup);
    return std::wstring(prefix) + name;
}

// Strip a leading "[L<digits>] " from a wide string in-place. Used by
// the TVN_ENDLABELEDIT handler so the edit box's apparent text (which
// may include the display prefix) doesn't get persisted back into the
// emitter's actual name.
static void StripLinkGroupPrefix(std::wstring& text)
{
    if (text.size() < 4 || text[0] != L'[' || text[1] != L'L') return;
    size_t i = 2;
    while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') i++;
    if (i == 2 || i + 1 >= text.size()) return;       // no digits, or no room for ']'
    if (text[i] != L']' || text[i + 1] != L' ') return;
    text.erase(0, i + 2);
}

// Repaint a single tree item's text to reflect the current link-group
// state. Used after any link-group operation so the [L<n>] prefix
// matches the data model without rebuilding the whole tree (which
// would lose expansion state).
static void RefreshEmitterTreeText(HWND                            hTree,
                                    HTREEITEM                       hItem,
                                    const ParticleSystem::Emitter*  emitter)
{
    if (hTree == NULL || hItem == NULL || emitter == NULL) return;
    std::wstring text = FormatEmitterDisplayName(emitter);
    TVITEM item;
    item.hItem   = hItem;
    item.mask    = TVIF_TEXT;
    item.pszText = (LPWSTR)text.c_str();
    TreeView_SetItem(hTree, &item);
}

// Multi-select state update (). Modifier-aware: reads
// GetKeyState(VK_CONTROL) / VK_SHIFT at call time. Updates the
// control's multi-set and selection anchor per the click semantics.
// Does NOT update the primary `selection` field or call
// TreeView_SelectItem — caller decides whether to forward the event
// to the tree's default proc (which fires TVN_SELCHANGED and updates
// primary) or to update the primary manually.
//
// Caller is responsible for invalidating the tree afterward so
// secondary-select paint reflects the new state.
//
// Modifier matrix:
//   Plain click   : multi = {clicked}, anchor = clicked
//   Ctrl-click    : toggle clicked in multi, anchor = clicked
//   Shift-click   : multi = pre-order range(anchor, clicked),
//                   anchor unchanged
struct EmitterListControl;  // forward decl
static void UpdateMultiSelectionFromClick(EmitterListControl*       control,
                                           ParticleSystem::Emitter*  clicked,
                                           bool                      ctrlDown,
                                           bool                      shiftDown);

// Walk the visible tree in pre-order, collecting every emitter
// between (and including) `from` and `to`. Used by Shift-click to
// resolve a range against the tree's current visible order — not
// the underlying emitter vector, which can include hidden children.
static std::vector<ParticleSystem::Emitter*> CollectTreeRange(
    HWND                            hTree,
    const ParticleSystem::Emitter*  from,
    const ParticleSystem::Emitter*  to);

// Confirmation dialog shown when a link operation will overwrite one
// emitter's parameters with another's. Spells out the direction
// explicitly ("X will be overwritten to match Y"), names the source
// of the surviving values, and lists the affected fields.
//
// `victim` is the emitter whose values will be lost. `source` is
// either the other emitter (for Create) or a textual description of
// the surviving values (for Join, where the "source" is the group's
// canonical member). Caller passes an empty `diffs` to suppress the
// dialog entirely.
//
// Returns true to proceed, false to cancel.
static bool ConfirmLinkOverwrite(HWND                            hOwner,
                                  const std::wstring&             title,
                                  const std::wstring&             victimName,
                                  const std::wstring&             sourceDescription,
                                  const std::vector<std::string>& diffs)
{
    if (diffs.empty()) return true;

    std::wstring msg;
    msg += L"\"" + victimName + L"\" will be overwritten to match ";
    msg += sourceDescription;
    msg += L".\n\nAffected fields (";
    wchar_t nbuf[16];
    swprintf(nbuf, 16, L"%zu", diffs.size());
    msg += nbuf;
    msg += L"):\n";

    size_t shown = 0;
    for (size_t d = 0; d < diffs.size() && shown < 8; d++, shown++)
    {
        msg += L"  - ";
        msg += AnsiToWide(diffs[d]);
        msg += L"\n";
    }
    if (diffs.size() > 8)
    {
        wchar_t mbuf[32];
        swprintf(mbuf, 32, L"  ... and %zu more\n", diffs.size() - 8);
        msg += mbuf;
    }

    msg += L"\nThe textures, atlas index curve, and name are kept "
           L"per-emitter and will not be overwritten.\n\nContinue?";

    return MessageBoxW(hOwner, msg.c_str(), title.c_str(),
                       MB_YESNO | MB_ICONQUESTION) == IDYES;
}

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

    // Drag-drop state. dragSource non-NULL means a drag is in progress;
    // the public EmitterList_IsDragging accessor reads from this. All
    // drag-drop visual teardown goes through EndDragVisual so each
    // field gets cleared in one place; the dragSource clear lives in
    // EndDragLogical so EmitterList_IsDragging stays true through any
    // post-drop modal popup (slot picker for reparent). See risks
    // section in tasks/todo.md.
    //
    // Operates as both reorder (drop between root gaps; existing PR
    // #35 behaviour) and reparent (drop onto an emitter; new in this
    // PR). The DropTarget computed in WM_MOUSEMOVE carries the kind.
    ParticleSystem::Emitter* dragSource;
    HIMAGELIST               dragImageList;
    HTREEITEM                dragInsertTarget;   // current insertion-mark target (BetweenGap)
    bool                     dragInsertAfter;    // false = above, true = below
    HTREEITEM                dragDropHighlight;  // currently TVIS_DROPHILITED'd item (OntoEmitter)
    UINT_PTR                 dragScrollTimer;    // 0 = no autoscroll, else timer id
    int                      dragScrollDir;      // -1 up, +1 down (only valid when timer set)

    // Multi-select state (). `selection` above remains the
    // "primary" — the single emitter the inspector pane reflects.
    // `multiSelection` is a superset that always includes the primary
    // (invariant). Right-click menu handlers consult this set for
    // batch operations like "Link selected". `selectionAnchor` marks
    // the most recent plain or Ctrl-click target — Shift-click selects
    // the tree-order range from anchor to the new click.
    //
    // Multi-set is cleared on particle-system swap; drag-drop reorder
    // acts on `selection` only (multi-set untouched) so the user can
    // reposition linked emitters independently for interleaved layering.
    std::set<ParticleSystem::Emitter*> multiSelection;
    ParticleSystem::Emitter*            selectionAnchor;

    // Marquee (rubber-band) selection state (). Active between
    // WM_LBUTTONDOWN on tree empty space and the matching WM_LBUTTONUP
    // (or WM_CAPTURECHANGED on cancel). Coordinates are in tree client
    // space. `marqueeAdditive` is true when Ctrl was held at drag
    // start — items inside the rect get ADDED to the existing
    // selection rather than replacing it.
    //
    // Sticky semantics: `marqueeSweptHits` accumulates every emitter
    // the rect has ever touched during the current drag. Multi-set
    // each move = preCtrl ∪ sweptHits. The user can sweep rows in any
    // order without later mouse positions deselecting earlier hits,
    // and rows at the edge of the rect (where IntersectRect returns
    // zero on shared borders) still get captured once briefly
    // overlapped.
    bool                                marqueeActive;
    bool                                marqueeAdditive;
    POINT                               marqueeStart;
    POINT                               marqueeCurrent;
    std::set<ParticleSystem::Emitter*>  marqueePreCtrl;  // selection at drag start (additive case)
    std::set<ParticleSystem::Emitter*>  marqueeSweptHits;

    // link-group visual bracket — layout cache + hover state.
    // bracketLayout is rebuilt at CDDS_PREPAINT when `valid` is false;
    // invalidated by any operation that affects tree Y order, group
    // membership, tree width, or theme. hoveredGroupId is mutated by
    // WM_MOUSEMOVE in the tree subclass and cleared on cursor exit /
    // focus loss / capture change. mouseTrackingArmed gates the
    // TrackMouseEvent re-arm cycle for WM_MOUSELEAVE delivery.
    BracketLayout                       bracketLayout;
    uint32_t                            hoveredGroupId;     // 0 = none
    bool                                mouseTrackingArmed;
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

// ----------------------------------------------------------------------------
// — layout cache rebuild
//
// Walks the expanded tree (not just the viewport-visible portion) and
// collects every linked emitter's row centre Y into a per-group bucket.
// Groups with < 2 visible members are discarded — no bracket needed.
// Remaining groups are lane-allocated via greedy interval scheduling
// (sort by minY ascending; each group takes the lowest-index lane
// whose previous occupant ended before this group's first member).
//
// Cost is O(N log N) where N is the count of expanded linked emitters.
// Called from CDDS_PREPAINT when `bracketLayout.valid` is false.

static void BracketLayout_WalkExpanded(HWND                            hTree,
                                       HTREEITEM                       hItem,
                                       std::map<uint32_t, BracketLayout::Group>& byId,
                                       LONG&                           maxLabelRight)
{
    while (hItem != NULL)
    {
        TVITEM ti = { 0 };
        ti.mask  = TVIF_PARAM;
        ti.hItem = hItem;
        if (TreeView_GetItem(hTree, &ti))
        {
            ParticleSystem::Emitter* e = (ParticleSystem::Emitter*)ti.lParam;
            if (e != NULL)
            {
                // Track the max label.right across ALL expanded items
                // (linked or not) so the bracket can position itself
                // right of every visible name, not just right of group
                // members. A long unlinked row between two linked rows
                // would otherwise have its text overlap the bracket.
                // TRUE = label rect (the box around the emitter name),
                // not the full row rect.
                RECT labelR;
                if (TreeView_GetItemRect(hTree, hItem, &labelR, TRUE))
                {
                    if (labelR.right > maxLabelRight) maxLabelRight = labelR.right;
                }
                if (e->linkGroup != 0)
                {
                    RECT r;
                    if (TreeView_GetItemRect(hTree, hItem, &r, FALSE))
                    {
                        LONG centreY = (r.top + r.bottom) / 2;
                        BracketLayout::Group& g = byId[e->linkGroup];
                        if (g.members.empty())
                        {
                            g.groupId = e->linkGroup;
                            g.minY    = centreY;
                            g.maxY    = centreY;
                        }
                        else
                        {
                            if (centreY < g.minY) g.minY = centreY;
                            if (centreY > g.maxY) g.maxY = centreY;
                        }
                        BracketLayout::Member m;
                        m.centreY = centreY;
                        m.emitter = e;
                        g.members.push_back(m);
                    }
                }
            }
        }
        // Recurse only into expanded children — collapsed branches
        // contribute no rect.
        if ((TreeView_GetItemState(hTree, hItem, TVIS_EXPANDED) & TVIS_EXPANDED) != 0)
        {
            HTREEITEM hChild = TreeView_GetChild(hTree, hItem);
            if (hChild != NULL)
                BracketLayout_WalkExpanded(hTree, hChild, byId, maxLabelRight);
        }
        hItem = TreeView_GetNextSibling(hTree, hItem);
    }
}

static void RebuildBracketLayout(EmitterListControl* control)
{
    BracketLayout& L = control->bracketLayout;
    L.groups.clear();
    L.numLanes = 0;
    L.valid = true;

    if (control->hTree == NULL || control->system == NULL) return;

    // 1. Walk + bucket by linkGroup. Also pick up the widest label
    //    rect across the whole expanded tree so the bracket can sit
    //    just right of every visible name (adapts to renames and
    //    differing label widths).
    std::map<uint32_t, BracketLayout::Group> byId;
    LONG maxLabelRight = 0;
    HTREEITEM hRoot = TreeView_GetRoot(control->hTree);
    BracketLayout_WalkExpanded(control->hTree, hRoot, byId, maxLabelRight);

    // 2. Keep only groups with >= 2 visible (expanded) members.
    for (std::map<uint32_t, BracketLayout::Group>::iterator it = byId.begin();
         it != byId.end(); ++it)
    {
        if (it->second.members.size() >= 2)
            L.groups.push_back(it->second);
    }

    // 3. Greedy interval scheduling by minY.
    std::sort(L.groups.begin(), L.groups.end(),
              [](const BracketLayout::Group& a, const BracketLayout::Group& b)
              { return a.minY < b.minY; });
    std::vector<LONG> laneEnd;
    L.hcMode = IsHighContrastActive();
    for (size_t i = 0; i < L.groups.size(); ++i)
    {
        BracketLayout::Group& g = L.groups[i];
        int lane = -1;
        for (size_t k = 0; k < laneEnd.size(); ++k)
        {
            if (laneEnd[k] < g.minY) { lane = (int)k; laneEnd[k] = g.maxY; break; }
        }
        if (lane < 0) { lane = (int)laneEnd.size(); laneEnd.push_back(g.maxY); }
        g.lane   = lane;
        g.colour = L.hcMode ? GetSysColor(COLOR_HIGHLIGHT)
                            : kBracketPalette[g.groupId % 12];
    }
    L.numLanes = (int)laneEnd.size();

    // 4. DPI-aware sizing. Lane 0's dot sits just right of the longest
    //    visible label (with padding); higher lanes extend RIGHTWARD
    //    from there. If the bracket would overflow the tree client,
    //    clamp the start position so the rightmost lane stays inside
    //    the gutter — this happens for narrow trees with many lanes,
    //    and the lane-width floor (2 px) protects against the
    //    pathological case.
    int dpi = GetDpiForWindow(control->hTree);
    if (dpi <= 0) dpi = 96;
    int baseLaneWidth = MulDiv(6, dpi, 96);
    int padFromLabel  = MulDiv(12, dpi, 96);
    int rightGutter   = MulDiv(4, dpi, 96);
    RECT cr;
    GetClientRect(control->hTree, &cr);
    int availableForBracket
        = cr.right - rightGutter - (int)maxLabelRight - padFromLabel;
    if (L.numLanes > 0 && L.numLanes * baseLaneWidth > availableForBracket)
    {
        int floored = (availableForBracket > 0)
                    ? (availableForBracket / L.numLanes)
                    : 2;
        L.laneWidth = (floored < 2) ? 2 : floored;
    }
    else
    {
        L.laneWidth = baseLaneWidth;
    }
    L.dotRadius       = MulDiv(3, dpi, 96);
    L.stubLength      = MulDiv(5, dpi, 96);
    L.strokeWidth     = MulDiv(1, dpi, 96);
    if (L.strokeWidth < 1) L.strokeWidth = 1;
    // Lane-0 dotX = label.right + 12 px padding. Higher lanes go right.
    L.rightEdgeOffset = (int)maxLabelRight + padFromLabel;
    // Clamp: if labels are unusually long for the tree width, push the
    // bracket back so the rightmost lane fits inside the client gutter.
    int maxStart = cr.right - rightGutter - L.numLanes * L.laneWidth;
    if (L.rightEdgeOffset > maxStart) L.rightEdgeOffset = maxStart;
    if (L.rightEdgeOffset < MulDiv(50, dpi, 96))
        L.rightEdgeOffset = MulDiv(50, dpi, 96);
    L.scrollOriginY   = GetScrollPos(control->hTree, SB_VERT);

    // Detect bracket geometry shift between paints. If the bracket's X
    // position, lane width, or lane count changed since the last paint,
    // partial repaints (e.g. the tree invalidating only the renamed
    // row after a label-edit commit) would leave stale bracket pixels
    // at the previous X on rows that weren't invalidated — visible as
    // a "duplicated" bracket. Queue a full-tree invalidate so the next
    // paint cycle redraws everything at the new geometry. The shift
    // is rare (rename / link mutation / system swap / tree resize),
    // so the extra paint is cheap.
    bool layoutShifted =
        (L.prevRightEdgeOffset >= 0) &&
        ((L.prevRightEdgeOffset != L.rightEdgeOffset) ||
         (L.prevLaneWidth       != L.laneWidth)       ||
         (L.prevNumLanes        != L.numLanes));
    L.prevRightEdgeOffset = L.rightEdgeOffset;
    L.prevLaneWidth       = L.laneWidth;
    L.prevNumLanes        = L.numLanes;
    if (layoutShifted)
    {
#ifndef NDEBUG
        printf("[Link] layout shifted — forcing full-tree invalidate\n");
        fflush(stdout);
#endif
        InvalidateRect(control->hTree, NULL, TRUE);
    }

#ifndef NDEBUG
    int visibleLinkedEmitters = 0;
    for (size_t i = 0; i < L.groups.size(); ++i)
        visibleLinkedEmitters += (int)L.groups[i].members.size();
    printf("[Link] layout groups=%zu lanes=%d visibleLinkedEmitters=%d hc=%d rightEdge=%d\n",
           L.groups.size(), L.numLanes, visibleLinkedEmitters, L.hcMode ? 1 : 0,
           L.rightEdgeOffset);
    fflush(stdout);
#endif
}

// Mark the bracket layout cache stale. Called from every code path that
// can change tree Y order, group membership, tree dimensions, or theme.
// Cheap — just a flag set; next CDDS_PREPAINT will rebuild.
//
// In milestone 2 the painting loop unconditionally rebuilds the cache
// every paint, so this is currently a no-op-with-future-purpose. Kept
// in the API in case milestone 5 re-introduces validity gating.
static void InvalidateBracketLayout(EmitterListControl* control)
{
    if (control != NULL) control->bracketLayout.valid = false;
}

// hit-test: classify a tree-client-space point against the cached
// bracket layout. Returns the group hit (Dot is more specific than Line)
// or { None, 0 } if no hit. Hit slop is +/- (dotRadius + 2) px for dots
// and +/- max(2, strokeWidth + 1) px for lines. The line span is the
// group's full minY..maxY (so the user can click anywhere between two
// member dots and select the group).
//
// Stale-paint guard: if the tree's scroll position has changed since the
// cache was built (paint races, mid-frame scroll), returns None so the
// click is harmlessly ignored. The next paint rebuilds with current Y;
// the user clicks again.
struct BracketHit
{
    enum Kind { None, Dot, Line };
    Kind     kind;
    uint32_t groupId;
};

static BracketHit HitTestBracket(HWND                  hTree,
                                  const BracketLayout&  L,
                                  POINT                 pt)
{
    BracketHit none = { BracketHit::None, 0 };
    if (L.groups.empty()) return none;
    if (hTree != NULL && GetScrollPos(hTree, SB_VERT) != L.scrollOriginY)
        return none;
    const int dotSlop  = L.dotRadius + 2;
    const int lineSlop = (L.strokeWidth + 1 > 2) ? (L.strokeWidth + 1) : 2;
    for (size_t gi = 0; gi < L.groups.size(); ++gi)
    {
        const BracketLayout::Group& g = L.groups[gi];
        // Lane 0 sits at rightEdgeOffset (just right of labels);
        // higher lanes extend RIGHTWARD into the gutter toward the
        // tree's client right edge.
        int dotX = L.rightEdgeOffset + g.lane * L.laneWidth;
        // Dots first — more specific than line.
        for (size_t mi = 0; mi < g.members.size(); ++mi)
        {
            LONG y = g.members[mi].centreY;
            if (abs(pt.x - dotX) <= dotSlop && abs(pt.y - y) <= dotSlop)
            {
                BracketHit h = { BracketHit::Dot, g.groupId };
                return h;
            }
        }
        // Line span: lane column between topmost and bottommost member Y.
        if (abs(pt.x - dotX) <= lineSlop
            && pt.y >= g.minY && pt.y <= g.maxY)
        {
            BracketHit h = { BracketHit::Line, g.groupId };
            return h;
        }
    }
    return none;
}

// hover-state clear. Idempotent — safe to call from any path that
// loses hover (mouse exits tree client, focus loss, capture change,
// modal dialog activation, etc.). Invalidates the tree if there was
// hover to clear so the previously-tinted rows repaint clean.
static void ClearBracketHover(HWND hTree, EmitterListControl* control)
{
    if (control == NULL) return;
    if (control->hoveredGroupId != 0)
    {
#ifndef NDEBUG
        printf("[Link] hover clear (was %u)\n", control->hoveredGroupId);
        fflush(stdout);
#endif
        control->hoveredGroupId = 0;
        // bErase=TRUE forces WM_ERASEBKGND so the prior AlphaBlend tint
        // is cleared from the tree's TVS_EX_DOUBLEBUFFER back surface.
        // bErase=FALSE was leaving the previous frame's tint visible
        // because the buffered paint reused stale pixels.
        if (hTree != NULL) InvalidateRect(hTree, NULL, TRUE);
    }
    control->mouseTrackingArmed = false;
}

// ----------------------------------------------------------------------------

// Walk the visible tree in pre-order. Helper used by Shift-click
// range resolution. Returns NULL when neither endpoint matches a
// visible item.
static void CollectTreeRangeWalk(HWND                                    hTree,
                                  HTREEITEM                               hItem,
                                  const ParticleSystem::Emitter*          from,
                                  const ParticleSystem::Emitter*          to,
                                  bool&                                   inRange,
                                  bool&                                   done,
                                  std::vector<ParticleSystem::Emitter*>&  out)
{
    while (hItem != NULL && !done)
    {
        TVITEM ti = { 0 };
        ti.hItem = hItem;
        ti.mask  = TVIF_PARAM;
        if (TreeView_GetItem(hTree, &ti))
        {
            ParticleSystem::Emitter* e = (ParticleSystem::Emitter*)ti.lParam;
            bool isEndpoint = (e == from || e == to);
            if (isEndpoint && !inRange)
            {
                inRange = true;
                out.push_back(e);
                if (from == to) { done = true; return; }
            }
            else if (isEndpoint && inRange)
            {
                out.push_back(e);
                done = true;
                return;
            }
            else if (inRange)
            {
                out.push_back(e);
            }
        }
        HTREEITEM hChild = TreeView_GetChild(hTree, hItem);
        if (hChild != NULL)
        {
            CollectTreeRangeWalk(hTree, hChild, from, to, inRange, done, out);
            if (done) return;
        }
        hItem = TreeView_GetNextSibling(hTree, hItem);
    }
}

static std::vector<ParticleSystem::Emitter*> CollectTreeRange(
    HWND                            hTree,
    const ParticleSystem::Emitter*  from,
    const ParticleSystem::Emitter*  to)
{
    std::vector<ParticleSystem::Emitter*> out;
    if (hTree == NULL || from == NULL || to == NULL) return out;
    bool inRange = false, done = false;
    CollectTreeRangeWalk(hTree, TreeView_GetRoot(hTree),
                          from, to, inRange, done, out);
    return out;
}

static void UpdateMultiSelectionFromClick(EmitterListControl*       control,
                                           ParticleSystem::Emitter*  clicked,
                                           bool                      ctrlDown,
                                           bool                      shiftDown)
{
    if (control == NULL || clicked == NULL) return;

    if (ctrlDown && !shiftDown)
    {
        // Toggle. Refuses to remove the only remaining member —
        // the multi-set is never empty when a click lands on an
        // emitter row, so the user always has at least one
        // selection. Removing the last member would also force
        // the primary to become NULL, which the tree's selection
        // model doesn't represent cleanly.
        if (control->multiSelection.count(clicked) > 0)
        {
            if (control->multiSelection.size() > 1)
                control->multiSelection.erase(clicked);
            // else: keep clicked (no-op)
        }
        else
        {
            control->multiSelection.insert(clicked);
        }
        control->selectionAnchor = clicked;
    }
    else if (shiftDown)
    {
        ParticleSystem::Emitter* anchor = control->selectionAnchor;
        if (anchor == NULL) anchor = clicked;
        std::vector<ParticleSystem::Emitter*> range
            = CollectTreeRange(control->hTree, anchor, clicked);
        // Defensive: if the walk failed to find both endpoints
        // (shouldn't happen in normal usage), fall back to
        // {anchor, clicked}.
        if (range.empty())
        {
            range.push_back(anchor);
            if (anchor != clicked) range.push_back(clicked);
        }
        control->multiSelection.clear();
        for (size_t i = 0; i < range.size(); i++)
        {
            control->multiSelection.insert(range[i]);
        }
        // Anchor unchanged.
    }
    else
    {
        // Plain click resets the set to {clicked}.
        control->multiSelection.clear();
        control->multiSelection.insert(clicked);
        control->selectionAnchor = clicked;
    }

#ifndef NDEBUG
    printf("[MultiSel] size=%zu primary='%s' anchor='%s'\n",
           control->multiSelection.size(),
           clicked ? clicked->name.c_str() : "(null)",
           control->selectionAnchor ? control->selectionAnchor->name.c_str() : "(null)");
    fflush(stdout);
#endif
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
    wstring name = FormatEmitterDisplayName(emitter);
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

// Drop target kinds. The hit-test classifies the cursor's position
// over the tree into one of these:
enum DropKind
{
    DROP_INVALID,        // outside, in collapsed-child gap, source's own gap, etc.
    DROP_BETWEEN_GAP,    // top/bottom 1/3 of a root item rect → reorder root list
    DROP_ONTO_EMITTER,   // middle 1/3 of any item rect → reparent under that item
};

// "Gap index" 0..numRoots (only valid for DROP_BETWEEN_GAP):
//   gap 0          = above first root
//   gap numRoots   = below last root
//   gap K (0<K<N)  = between root K-1 and root K
struct DropTarget
{
    DropKind  kind;
    HTREEITEM hTarget;                       // tree item the action concerns; NULL when DROP_INVALID
    size_t    gap;                           // valid when DROP_BETWEEN_GAP
    bool      after;                         // valid when DROP_BETWEEN_GAP (false = above hTarget, true = below)
    ParticleSystem::Emitter* targetEmitter;  // valid when DROP_ONTO_EMITTER (the emitter to reparent under)
};

// Compute the drop target for cursor `pt` (in tree client coords).
// Three-zone hit-test on each item's rect: top 1/3 → insertion-mark
// above, middle 1/3 → drop onto, bottom 1/3 → insertion-mark below.
// Plus the four edge cases (above first item, below last item,
// outside client area, child-as-between-target).
//
// Caller is responsible for further validity (no-op / cycle /
// slot-occupied) checks; this function just classifies geometry.
static DropTarget ComputeDropTarget(HWND hTree, POINT pt, size_t numRoots)
{
    DropTarget out = { DROP_INVALID, NULL, 0, false, NULL };
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
        // Empty area inside the tree. Above-first / below-last become
        // BetweenGap drops; anywhere else (e.g. visible-but-collapsed
        // child gap) stays invalid.
        HTREEITEM hFirst = TreeView_GetRoot(hTree);
        if (hFirst == NULL) return out;
        RECT firstRect;
        TreeView_GetItemRect(hTree, hFirst, &firstRect, TRUE);
        if (pt.y < firstRect.top)
        {
            out.kind = DROP_BETWEEN_GAP; out.gap = 0; out.hTarget = hFirst; out.after = false;
            return out;
        }
        HTREEITEM hLast = hFirst;
        while (HTREEITEM h = TreeView_GetNextSibling(hTree, hLast)) hLast = h;
        RECT lastRect;
        TreeView_GetItemRect(hTree, hLast, &lastRect, TRUE);
        if (pt.y >= lastRect.bottom)
        {
            out.kind = DROP_BETWEEN_GAP; out.gap = numRoots; out.hTarget = hLast; out.after = true;
            return out;
        }
        return out;
    }

    // Hovered item's rect — the three-zone classifier operates on this.
    RECT itemRect;
    TreeView_GetItemRect(hTree, hHit, &itemRect, TRUE);
    int height = itemRect.bottom - itemRect.top;
    int third  = (height > 0) ? height / 3 : 0;
    int yIntoItem = pt.y - itemRect.top;

    // Walk up to the root ancestor (used both for the BetweenGap
    // root-index calculation and for the "between-gap on a child"
    // refusal — children don't define a gap in the root list).
    HTREEITEM hRoot = hHit;
    while (HTREEITEM hParent = TreeView_GetParent(hTree, hRoot)) hRoot = hParent;
    bool hoverIsChild = (hHit != hRoot);

    // Middle 1/3 → drop onto. Always valid as a *kind* even for
    // children; reparent allows children-as-target. The caller does
    // cycle / slot-occupied / current-parent validity checks.
    if (third > 0 && yIntoItem >= third && yIntoItem < height - third)
    {
        out.kind = DROP_ONTO_EMITTER;
        out.hTarget = hHit;
        out.targetEmitter = (ParticleSystem::Emitter*)0;  // resolved by caller via item lParam
        TVITEM tvi;
        tvi.hItem = hHit;
        tvi.mask  = TVIF_PARAM;
        if (TreeView_GetItem(hTree, &tvi))
        {
            out.targetEmitter = (ParticleSystem::Emitter*)tvi.lParam;
        }
        return out;
    }

    // Top / bottom thirds → BetweenGap. Children don't define a root
    // gap, so a between-gap classification on a child stays invalid.
    if (hoverIsChild) return out;

    size_t rootIdx = 0;
    for (HTREEITEM h = TreeView_GetRoot(hTree); h != NULL && h != hRoot;
         h = TreeView_GetNextSibling(hTree, h))
    {
        rootIdx++;
    }

    out.kind    = DROP_BETWEEN_GAP;
    out.hTarget = hRoot;
    if (yIntoItem < third)
    {
        out.gap   = rootIdx;
        out.after = false;
    }
    else
    {
        out.gap   = rootIdx + 1;
        out.after = true;
    }
    return out;
}

// True if `candidate` is `ancestor` or appears anywhere in
// `ancestor`'s subtree. Bottom-up walk via parent pointers so this
// can't itself recurse into a malformed cycle.
static bool IsInSubtreeOfEmitter(const ParticleSystem::Emitter* candidate,
                                 const ParticleSystem::Emitter* ancestor)
{
    if (candidate == NULL || ancestor == NULL) return false;
    const ParticleSystem::Emitter* p = candidate;
    while (p != NULL)
    {
        if (p == ancestor) return true;
        p = p->parent;
    }
    return false;
}

// Clear the currently-DROP_HIGHLIGHTed item, if any. Idempotent.
static void ClearDropHighlight(EmitterListControl* control)
{
    if (control->dragDropHighlight == NULL) return;
    TVITEM tvi;
    tvi.hItem     = control->dragDropHighlight;
    tvi.mask      = TVIF_STATE;
    tvi.state     = 0;
    tvi.stateMask = TVIS_DROPHILITED;
    TreeView_SetItem(control->hTree, &tvi);
    control->dragDropHighlight = NULL;
}

// Tear down the visual half of a drag: capture, image list, insertion
// mark, drop-highlight, autoscroll timer. Idempotent — every step
// null-checks first and clears after, so calling this repeatedly is
// harmless. Used by WM_CAPTURECHANGED (which fires synchronously when
// our own ReleaseCapture runs, AND when the slot-picker popup takes
// capture mid-flight) so the visual state is safe to reset multiple
// times.
//
// Does NOT clear control->dragSource — that lives in EndDragLogical
// and is delayed until the drop has fully resolved (including any
// modal slot-picker popup) so EmitterList_IsDragging keeps the
// accelerator gate armed.
static void EndDragVisual(EmitterListControl* control)
{
    bool hadVisual = (control->dragImageList    != NULL
                   || control->dragInsertTarget != NULL
                   || control->dragDropHighlight != NULL
                   || control->dragScrollTimer  != 0);

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
    control->dragInsertTarget = NULL;
    control->dragInsertAfter  = false;
    ClearDropHighlight(control);
    if (GetCapture() == control->hTree) ReleaseCapture();

    // Force a full repaint after tearing down drag-time visual state.
    // Belt-and-braces against drag-image ghost residue and stuck
    // TVIS_DROPHILITED rendering — the per-item TreeView_SetItem +
    // ImageList_DragLeave calls above SHOULD invalidate cleanly, but
    // in practice cancellation paths (Esc, right-click,
    // WM_CAPTURECHANGED from a captured-window-stealing dialog) can
    // leave horizontal stripes / phantom highlights on rows the
    // cursor passed over. A single InvalidateRect over the whole
    // client area is cheap (the tree isn't very tall) and produces
    // unambiguously clean state. Skip when no visual state was set
    // — keeps the defensive EndDrag from OnParticleSystemChange /
    // WM_DESTROY from causing spurious flicker.
    if (hadVisual)
    {
        InvalidateRect(control->hTree, NULL, TRUE);
        UpdateWindow(control->hTree);
    }
}

// Clear the logical drag flag. After this, EmitterList_IsDragging
// returns false and the accelerator gate disarms. Called exactly
// once at the end of a drag's lifecycle, after any modal popup has
// resolved.
static void EndDragLogical(EmitterListControl* control)
{
#ifndef NDEBUG
    if (control->dragSource != NULL)
    {
        DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        printf("[DnD] END   gdi=%lu\n", gdiAfter); fflush(stdout);
    }
#endif
    control->dragSource = NULL;
}

// Combined teardown for cancel paths (Esc, capture loss before the
// drop completes, defensive cleanup from OnParticleSystemChange).
static void EndDrag(EmitterListControl* control)
{
    EndDragVisual(control);
    EndDragLogical(control);
}

// Update visual feedback (insertion mark, drop-highlight, cursor)
// based on the cursor's current position. Call from WM_MOUSEMOVE,
// the autoscroll WM_TIMER handler, and on WM_MOUSEWHEEL re-feedback.
// Returns the computed DropTarget so callers can reuse the
// classification (WM_LBUTTONUP commits based on the same target).
static DropTarget UpdateDropFeedback(EmitterListControl* control, POINT pt)
{
    size_t numRoots = 0;
    if (control->system != NULL)
    {
        const std::vector<ParticleSystem::Emitter*>& emitters = control->system->getEmitters();
        for (size_t i = 0; i < emitters.size(); i++) if (emitters[i]->parent == NULL) numRoots++;
    }

    DropTarget t = ComputeDropTarget(control->hTree, pt, numRoots);
    ParticleSystem::Emitter* src = control->dragSource;

    // Validity refinement, per drop kind. Geometry is classified
    // already; here we apply semantic rules (no-op detection, cycles,
    // source-must-be-root for between-gap, etc.).
    bool valid = false;
    if (t.kind == DROP_BETWEEN_GAP)
    {
        // Reorder semantic — only legal if source is a root, AND the
        // target gap isn't source's own current gap (which would be a
        // no-op layout-wise).
        if (src != NULL && src->parent == NULL)
        {
            size_t srcIdx = RootIndexOf(control->system, src);
            if (srcIdx != (size_t)-1
                && t.gap != srcIdx
                && t.gap != srcIdx + 1)
            {
                valid = true;
            }
        }
    }
    else if (t.kind == DROP_ONTO_EMITTER && t.targetEmitter != NULL)
    {
        // Reparent semantic. Refuse if any of:
        //   - target == source (drop on self)
        //   - target is in source's subtree (cycle)
        //   - target IS source's current parent (slot-switch is out of
        //     scope; ParticleSystem::reparentEmitter would also refuse)
        //   - target's both spawn slots are occupied
        ParticleSystem::Emitter* tgt = t.targetEmitter;
        if (src != NULL && tgt != src
            && !IsInSubtreeOfEmitter(tgt, src)
            && src->parent != tgt)
        {
            bool slotADL = (tgt->spawnDuringLife == (size_t)-1);
            bool slotADD = (tgt->spawnOnDeath    == (size_t)-1);
            if (slotADL || slotADD) valid = true;
        }
    }

    // Apply visual feedback. Always clear the *other* feedback channel
    // before setting one — if the cursor moved from a between-gap into
    // an onto target, the insertion mark needs to disappear (and vice
    // versa for the drop-highlight).
    //
    // Caller (WM_MOUSEMOVE / WM_TIMER / WM_MOUSEWHEEL) is responsible
    // for the ImageList_DragShowNolock wrap; this function MUST NOT
    // wrap internally because DragShowNolock isn't a refcount and a
    // nested wrap would re-show the ghost prematurely.
    if (valid && t.kind == DROP_BETWEEN_GAP)
    {
        TreeView_SetInsertMark(control->hTree, t.hTarget, t.after ? TRUE : FALSE);
        ClearDropHighlight(control);
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        control->dragInsertTarget = t.hTarget;
        control->dragInsertAfter  = t.after;
    }
    else if (valid && t.kind == DROP_ONTO_EMITTER)
    {
        TreeView_SetInsertMark(control->hTree, NULL, FALSE);
        // Clear any previous drop-highlight before setting the new one
        // (cursor moved from item A to item B mid-drag).
        if (control->dragDropHighlight != t.hTarget)
        {
            ClearDropHighlight(control);
            TVITEM tvi;
            tvi.hItem     = t.hTarget;
            tvi.mask      = TVIF_STATE;
            tvi.state     = TVIS_DROPHILITED;
            tvi.stateMask = TVIS_DROPHILITED;
            TreeView_SetItem(control->hTree, &tvi);
            control->dragDropHighlight = t.hTarget;
        }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
    }
    else
    {
        TreeView_SetInsertMark(control->hTree, NULL, FALSE);
        ClearDropHighlight(control);
        SetCursor(LoadCursor(NULL, IDC_NO));
        control->dragInsertTarget = NULL;
    }

    // If we returned t with t.kind != DROP_INVALID but valid==false,
    // the caller's WM_LBUTTONUP must NOT commit — flatten to invalid
    // here so commit logic can use a single t.kind switch.
    if (!valid) t.kind = DROP_INVALID;
    return t;
}

bool EmitterList_IsDragging(HWND hWnd)
{
    EmitterListControl* control = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    return control != NULL && control->dragSource != NULL;
}

// Show a small popup at the cursor with two items — "Reparent as
// Lifetime child" and "Reparent as on-Death child" — and return the
// user's choice. Output `outUseSpawnDuringLife` is set to true when
// the user picked Lifetime, false for Death. Returns true if the
// user picked something, false if they cancelled (Esc / click
// outside / Alt+Tab).
//
// Built at runtime via CreatePopupMenu so we don't need a new
// resource. Same TrackPopupMenuEx + TPM_RETURNCMD pattern the
// emitter context menu and new-emitter dropdown already use.
static bool ShowSlotPickerPopup(HWND hOwner, POINT screenPt, bool* outUseSpawnDuringLife)
{
    HMENU hMenu = CreatePopupMenu();
    if (hMenu == NULL) return false;
    AppendMenu(hMenu, MF_STRING, ID_REPARENT_AS_LIFETIME, LoadString(IDS_MENU_REPARENT_LIFETIME).c_str());
    AppendMenu(hMenu, MF_STRING, ID_REPARENT_AS_DEATH,    LoadString(IDS_MENU_REPARENT_DEATH).c_str());

    INT picked = TrackPopupMenuEx(hMenu,
                                   TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                                   screenPt.x, screenPt.y, hOwner, NULL);
    DestroyMenu(hMenu);

    if (picked == ID_REPARENT_AS_LIFETIME) { *outUseSpawnDuringLife = true;  return true; }
    if (picked == ID_REPARENT_AS_DEATH)    { *outUseSpawnDuringLife = false; return true; }
    return false;
}

// Resolve the reparent slot (auto-pick or popup) and call into the
// data layer. Returns true on a successful reparent. Doesn't touch
// the tree itself — caller (WM_LBUTTONUP flow) rebuilds and
// re-selects on commit.
static bool CommitReparent(HWND hTreeWnd, EmitterListControl* control,
                           ParticleSystem::Emitter* source,
                           ParticleSystem::Emitter* target)
{
    if (source == NULL || target == NULL || control->system == NULL) return false;

    bool slotADL = (target->spawnDuringLife == (size_t)-1);
    bool slotADD = (target->spawnOnDeath    == (size_t)-1);
    if (!slotADL && !slotADD) return false;  // both occupied (shouldn't reach here; UpdateDropFeedback gates)

    bool useSpawnDuringLife;
    if (slotADL && slotADD)
    {
        // Both free — prompt the user. Anchor at the cursor so the
        // popup feels attached to the drop point.
        POINT screenPt; GetCursorPos(&screenPt);
        if (!ShowSlotPickerPopup(hTreeWnd, screenPt, &useSpawnDuringLife))
        {
#ifndef NDEBUG
            printf("[DnD] REPARENT cancelled at slot picker\n"); fflush(stdout);
#endif
            return false;
        }
    }
    else
    {
        useSpawnDuringLife = slotADL;
    }

#ifndef NDEBUG
    printf("[DnD] REPARENT src='%s' target='%s' slot=%s\n",
           source->name.c_str(), target->name.c_str(),
           useSpawnDuringLife ? "Lifetime" : "Death");
    fflush(stdout);
#endif
    return control->system->reparentEmitter(source, target, useSpawnDuringLife);
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

        case WM_LBUTTONDOWN:
            // Multi-select (). Read Ctrl/Shift state BEFORE the
            // tree's default selection runs so we can update
            // multiSelection / selectionAnchor with the modifier
            // semantics. For Ctrl/Shift clicks we eat the message
            // (because the tree's default would reset selection to
            // just the clicked item, fighting multi-select); we set
            // primary via TreeView_SelectItem instead. For plain
            // clicks we forward to the default so label-edit timer
            // and drag-prep still work. For clicks on empty tree
            // space (no item under cursor), we start a marquee
            // rubber-band selection.
            if (control != NULL && control->system != NULL)
            {
                TVHITTESTINFO ht = { 0 };
                ht.pt.x = GET_X_LPARAM(lParam);
                ht.pt.y = GET_Y_LPARAM(lParam);
                HTREEITEM hHit = TreeView_HitTest(hWnd, &ht);

                // bracket-click intercept. Runs BEFORE the marquee
                // / tree-row dispatch — clicking a bracket dot or line
                // is a group-select gesture independent of which row
                // the cursor lands on. The bracket lives in the right
                // margin gutter, so for non-pathological tree widths
                // it cannot overlap label text (R3 mitigation; D4/D5).
                //
                // Modifier semantics (Q1 resolution):
                //   plain        : replace multi-set with group members
                //   Ctrl         : union — add members to existing set
                //   Shift, Alt   : treat as plain (no useful "range"
                //                  semantic when the gesture is "this
                //                  whole group")
                {
                    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                    BracketHit bh = HitTestBracket(hWnd,
                                                    control->bracketLayout,
                                                    pt);
                    if (bh.kind != BracketHit::None)
                    {
                        std::vector<ParticleSystem::Emitter*> members
                            = GetLinkGroupMembers(*control->system,
                                                   bh.groupId);
                        if (members.empty())
                        {
                            // Defensive — shouldn't happen because the
                            // bracket only paints for groups with >= 2
                            // members. Treat as a click in empty space.
                            break;
                        }
                        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                        ParticleSystem::Emitter* topmostVisible = NULL;
                        RECT crv; GetClientRect(hWnd, &crv);
                        // Search the bracket layout for the group's
                        // member list (tree pre-order) and pick the
                        // first member whose centreY is in the viewport.
                        // The layout walker collects members in tree
                        // order, so members[0] is topmost-in-tree;
                        // we want topmost-in-viewport for the inspector
                        // to focus on something the user can see.
                        const BracketLayout& L = control->bracketLayout;
                        for (size_t gi = 0; gi < L.groups.size(); ++gi)
                        {
                            if (L.groups[gi].groupId != bh.groupId) continue;
                            for (size_t mi = 0; mi < L.groups[gi].members.size(); ++mi)
                            {
                                LONG y = L.groups[gi].members[mi].centreY;
                                if (y >= crv.top && y <= crv.bottom)
                                {
                                    topmostVisible
                                        = L.groups[gi].members[mi].emitter;
                                    break;
                                }
                            }
                            break;
                        }
                        if (topmostVisible == NULL)
                            topmostVisible = members[0];

                        if (ctrl)
                        {
                            // Union: add every group member into the
                            // existing multi-set without disturbing
                            // non-member members. selectionAnchor
                            // moves to topmostVisible so subsequent
                            // Shift-range starts there.
                            for (size_t i = 0; i < members.size(); ++i)
                                control->multiSelection.insert(members[i]);
                            // Primary stays where it was unless the
                            // current primary isn't in the multi-set
                            // (defensive — shouldn't happen).
                            if (control->selection == NULL ||
                                control->multiSelection.find(control->selection)
                                  == control->multiSelection.end())
                            {
                                control->selection = topmostVisible;
                            }
                            control->selectionAnchor = topmostVisible;
                        }
                        else
                        {
                            // Plain / Shift / Alt: replace with group.
                            control->multiSelection.clear();
                            for (size_t i = 0; i < members.size(); ++i)
                                control->multiSelection.insert(members[i]);
                            control->selection       = topmostVisible;
                            control->selectionAnchor = topmostVisible;
                        }
                        // Sync the tree's idea of primary so its own
                        // selection bookkeeping (focus rect, default
                        // paint colour) tracks. eats the click — the
                        // tree's default WM_LBUTTONDOWN proc would
                        // otherwise hit-test the bracket gutter as
                        // TVHT_ONITEMRIGHT and select the row under
                        // cursor, fighting us.
                        HTREEITEM hPrimary = FindTreeItemByEmitter(
                            hWnd,
                            TreeView_GetRoot(hWnd),
                            control->selection);
                        if (hPrimary != NULL)
                            TreeView_SelectItem(hWnd, hPrimary);
                        InvalidateRect(hWnd, NULL, FALSE);
                        NotifyParent(control, ELN_SELCHANGED);
#ifndef NDEBUG
                        printf("[Link] click select group=%u members=%zu anchor='%s' ctrl=%d\n",
                               bh.groupId, members.size(),
                               topmostVisible ? topmostVisible->name.c_str() : "(null)",
                               ctrl ? 1 : 0);
                        fflush(stdout);
#endif
                        return 0;   // eat
                    }
                }

                // Marquee branch fires ONLY when the click is truly
                // outside any row (hHit == NULL). If the hit-test
                // returned an item — even if the cursor is in the
                // row's indent, right-of-label, or stateicon area —
                // we treat it as a click on that row. Expand/collapse
                // button (TVHT_ONITEMBUTTON) and visibility icon
                // (TVHT_ONITEMICON) clicks pass through to the
                // default proc / existing NM_CLICK handler.
                if (hHit == NULL)
                {
                    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    // Only start a marquee when the click is in the
                    // tree's "labels area" — the left half of the
                    // client width where the emitter names live. A
                    // click in the empty right-side area shouldn't
                    // begin a marquee at all (avoids accidentally
                    // selecting rows whose Y stripe is crossed by a
                    // mostly-empty right-side drag).
                    RECT cr;
                    GetClientRect(hWnd, &cr);
                    int clickX = GET_X_LPARAM(lParam);
                    bool inLabelsArea = (clickX < (cr.right / 2));
                    if (!inLabelsArea)
                    {
                        // Pass through to default proc; no marquee.
                        break;
                    }
                    control->marqueeActive   = true;
                    control->marqueeAdditive = ctrl;
                    control->marqueeStart.x  = clickX;
                    control->marqueeStart.y  = GET_Y_LPARAM(lParam);
                    control->marqueeCurrent  = control->marqueeStart;
                    control->marqueeSweptHits.clear();
                    if (ctrl)
                    {
                        // Additive: snapshot current selection so
                        // mousemove can recompute (preStart ∪ hits)
                        // without losing pre-existing members.
                        control->marqueePreCtrl = control->multiSelection;
                    }
                    else
                    {
                        // Replacement: clear selection up front; the
                        // marquee will fill it as items are swept.
                        control->marqueePreCtrl.clear();
                    }
                    SetCapture(hWnd);
                    return 0;  // eat the message
                }

                if (hHit != NULL)
                {
                    // Skip our handling for icon clicks (they toggle
                    // visibility via the existing NM_CLICK path; the
                    // TVN_SELCHANGING handler already refuses the
                    // selection change in that case) and expand-
                    // button clicks (default proc toggles).
                    if (!(ht.flags & (TVHT_ONITEMICON | TVHT_ONITEMBUTTON)))
                    {
                        TVITEM ti = { 0 };
                        ti.hItem = hHit;
                        ti.mask  = TVIF_PARAM;
                        if (TreeView_GetItem(hWnd, &ti))
                        {
                            ParticleSystem::Emitter* clicked
                                = (ParticleSystem::Emitter*)ti.lParam;
                            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;

                            // Capture pre-state so we can decide what
                            // to use as the new primary after a
                            // Ctrl-click that removes the clicked
                            // item from the multi-set.
                            bool wasInMulti
                                = control->multiSelection.count(clicked) > 0;
                            ParticleSystem::Emitter* oldPrimary
                                = control->selection;

                            UpdateMultiSelectionFromClick(control, clicked,
                                                           ctrl, shift);

                            if (ctrl || shift)
                            {
                                // Decide new primary:
                                //   Shift / add-via-Ctrl: clicked
                                //   Ctrl-removing-non-primary: oldPrimary
                                //     stays (clicked dropped from multi)
                                //   Ctrl-removing-primary: pick another
                                //     member from the remaining multi-set
                                ParticleSystem::Emitter* newPrimary = clicked;
                                bool nowInMulti
                                    = control->multiSelection.count(clicked) > 0;
                                if (ctrl && wasInMulti && !nowInMulti)
                                {
                                    // Removed clicked from set.
                                    if (clicked == oldPrimary &&
                                        !control->multiSelection.empty())
                                    {
                                        newPrimary = *control->multiSelection.begin();
                                    }
                                    else
                                    {
                                        newPrimary = oldPrimary;  // unchanged
                                    }
                                }

                                HTREEITEM hNew = (newPrimary == clicked)
                                    ? hHit
                                    : FindTreeItemByEmitter(hWnd,
                                                            TreeView_GetRoot(hWnd),
                                                            newPrimary);
                                if (hNew != NULL)
                                {
                                    TreeView_SelectItem(hWnd, hNew);
                                }
                                InvalidateRect(hWnd, NULL, FALSE);
                                // Always notify even if primary didn't
                                // change — the multi-set may have
                                // grown/shrunk and the inspector needs
                                // to grey or ungrey accordingly.
                                NotifyParent(control, ELN_SELCHANGED);
                                return 0;  // eat the message
                            }
                            // Plain click: fall through to default
                            // (which selects the clicked item and
                            // arms label-edit / drag-prep state).
                            InvalidateRect(hWnd, NULL, FALSE);
                        }
                    }
                }
            }
            break;

        case WM_MOUSEMOVE:
            // Marquee selection (): track the cursor while the
            // marquee is active, invalidate the union of the old and
            // new rect so both sides repaint cleanly, and recompute
            // the multi-selection from items that intersect the rect.
            if (control != NULL && control->marqueeActive)
            {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

                auto marqueeRectFor = [&](POINT s, POINT e) -> RECT {
                    // Parens around std::min/max suppress the
                    // Windows.h min/max macro expansion.
                    RECT r;
                    r.left   = (std::min)(s.x, e.x);
                    r.top    = (std::min)(s.y, e.y);
                    r.right  = (std::max)(s.x, e.x);
                    r.bottom = (std::max)(s.y, e.y);
                    return r;
                };
                RECT oldR = marqueeRectFor(control->marqueeStart,
                                            control->marqueeCurrent);
                control->marqueeCurrent = pt;
                RECT newR = marqueeRectFor(control->marqueeStart,
                                            control->marqueeCurrent);

                // Invalidate the entire tree (not just the marquee
                // rect) so rows OUTSIDE the rect repaint when their
                // multi-set membership changes. Otherwise rows that
                // were secondary-highlighted before the marquee keep
                // their stale paint cache even after multi-set is
                // reset, making them look "still selected" while the
                // inspector / overlay correctly reflect the new
                // (smaller / empty) set. bErase=TRUE forces
                // WM_ERASEBKGND so the prior marquee frame is
                // cleared in empty inter-row space. Double-buffered
                // tree (TVS_EX_DOUBLEBUFFER) suppresses flicker.
                (void)oldR; (void)newR;
                InvalidateRect(hWnd, NULL, TRUE);

                // Sticky semantics: any emitter the rect has ever
                // touched during this drag stays in marqueeSweptHits.
                // multi-set each frame = preCtrl ∪ sweptHits — no
                // later mouse position can deselect an emitter the
                // user already swept over.
                //
                // 1 px inflation on the hit-test rect forgives the
                // shared-border case where IntersectRect would return
                // zero when the marquee's edge falls exactly on a
                // row boundary.
                RECT hitR = newR;
                InflateRect(&hitR, 1, 1);
                HTREEITEM hVis = TreeView_GetFirstVisible(hWnd);
                ParticleSystem::Emitter* primary = NULL;
                while (hVis != NULL)
                {
                    // Marquee start was gated to the labels-area, so
                    // we can safely use the full row rect for the
                    // hit-test here — that gives generous Y coverage
                    // and prevents off-by-a-few-pixel misses on the
                    // last row's label-only bounds.
                    RECT rowR;
                    if (TreeView_GetItemRect(hWnd, hVis, &rowR, FALSE))
                    {
                        RECT hit;
                        if (IntersectRect(&hit, &hitR, &rowR))
                        {
                            TVITEM ti = { 0 };
                            ti.hItem = hVis;
                            ti.mask  = TVIF_PARAM;
                            if (TreeView_GetItem(hWnd, &ti))
                            {
                                ParticleSystem::Emitter* e
                                    = (ParticleSystem::Emitter*)ti.lParam;
                                if (e != NULL)
                                {
                                    control->marqueeSweptHits.insert(e);
                                    primary = e;  // last hit in tree order
                                }
                            }
                        }
                    }
                    hVis = TreeView_GetNextVisible(hWnd, hVis);
                }
                // Compose final multi-set from pre-drag selection
                // plus the cumulative swept set.
                std::set<ParticleSystem::Emitter*> next
                    = control->marqueePreCtrl;
                for (auto* e : control->marqueeSweptHits) next.insert(e);
                size_t prevSize = control->multiSelection.size();
                control->multiSelection = next;
                // Set primary to a member of the set so the invariant
                // holds and the inspector reflects something
                // meaningful. Prefer the last marquee-hit emitter
                // (lowest in the tree, matches drag direction). If
                // the marquee swept nothing yet, leave primary alone.
                if (primary != NULL && primary != control->selection)
                {
                    HTREEITEM hi = FindTreeItemByEmitter(
                        hWnd, TreeView_GetRoot(hWnd), primary);
                    if (hi != NULL) TreeView_SelectItem(hWnd, hi);
                }
                // If the multi-set size crossed the 1↔2 boundary
                // without primary changing (e.g. the marquee just
                // added/removed a row but the topmost hit is still
                // the same), fire ELN_SELCHANGED so the overlay /
                // disabled-state tracks live during the drag.
                size_t newSize = control->multiSelection.size();
                if ((prevSize < 2) != (newSize < 2))
                {
                    NotifyParent(control, ELN_SELCHANGED);
                }
                return 0;
            }
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

                // Single hide/show pair around all of: ghost reposition
                // and tree-state changes. ImageList_DragShowNolock isn't
                // a refcount, so callees (UpdateDropFeedback) do NOT
                // wrap themselves — every wrap lives at the message
                // handler level. Hiding the ghost first lets the tree
                // repaint cleanly when TreeView_SetItem flips
                // TVIS_DROPHILITED on the row under the cursor; without
                // the wrap the imagelist's saved-background restore
                // gets clobbered by the tree's row repaint and the
                // ghost smears across every row the cursor visits.
                bool ghostActive = (control->dragImageList != NULL);
                if (ghostActive) ImageList_DragShowNolock(FALSE);
                ImageList_DragMove(pt.x, pt.y);
                UpdateDropFeedback(control, pt);
                if (ghostActive) ImageList_DragShowNolock(TRUE);
                return 0;
            }
            // hover. Only fires when neither marquee nor drag-drop
            // is active (both early-return above). Tracks the cursor's
            // position against the bracket layout; transitions between
            // groups (or to no-group) trigger a tree invalidate so old
            // and new hover-row tints + line-thickness repaint.
            //
            // TrackMouseEvent with TME_LEAVE arms the WM_MOUSELEAVE we
            // depend on to clear hover when the cursor exits the tree.
            // The flag must be re-armed each leave; we set
            // mouseTrackingArmed once and clear it in the leave handler.
            if (control != NULL)
            {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                BracketHit hit = HitTestBracket(hWnd,
                                                 control->bracketLayout,
                                                 pt);
                uint32_t newHover = (hit.kind != BracketHit::None)
                                  ? hit.groupId : 0;
                if (newHover != control->hoveredGroupId)
                {
#ifndef NDEBUG
                    printf("[Link] hover group=%u (was %u)\n",
                           newHover, control->hoveredGroupId);
                    fflush(stdout);
#endif
                    control->hoveredGroupId = newHover;
                    // bErase=TRUE so the AlphaBlend tint from the
                    // previous hovered group is cleared via WM_ERASEBKGND
                    // before the next paint. bErase=FALSE was letting
                    // the double-buffer keep stale tinted pixels.
                    InvalidateRect(hWnd, NULL, TRUE);
                }
                if (!control->mouseTrackingArmed)
                {
                    TRACKMOUSEEVENT tme;
                    tme.cbSize      = sizeof(tme);
                    tme.dwFlags     = TME_LEAVE;
                    tme.hwndTrack   = hWnd;
                    tme.dwHoverTime = 0;
                    if (TrackMouseEvent(&tme))
                        control->mouseTrackingArmed = true;
                }
            }
            break;

        case WM_MOUSEWHEEL:
            // User scrolled the tree mid-drag. The default tree proc
            // does the actual scroll; afterwards we recompute drop
            // feedback against the new layout (cursor stays put but
            // item rects shifted, so the insertion mark / drop-highlight
            // need to track the new visible items). Re-anchor the
            // ghost to the cursor too.
            //
            // Single ImageList_DragShowNolock hide/show wraps all the
            // tree-mutating calls (scroll, ghost reposition, drop
            // feedback updates) — see WM_MOUSEMOVE for why nesting
            // would break.
            if (control != NULL && control->dragSource != NULL)
            {
                bool ghostActive = (control->dragImageList != NULL);
                if (ghostActive) ImageList_DragShowNolock(FALSE);
                LRESULT defResult = CallWindowProc(
                    (WNDPROC)GetProp(hWnd, L"Old_WindowProc"),
                    hWnd, uMsg, wParam, lParam);
                POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
                ImageList_DragMove(pt.x, pt.y);
                UpdateDropFeedback(control, pt);
                if (ghostActive) ImageList_DragShowNolock(TRUE);
                return defResult;
            }
            break;

        case WM_TIMER:
            if (control != NULL && control->dragSource != NULL
                && wParam == AUTOSCROLL_TIMER_ID)
            {
                // Atomic scroll + recompute. The cursor hasn't moved
                // (no WM_MOUSEMOVE fired) but visible items shift, so
                // we re-anchor the ghost to absolute screen coords and
                // recompute drop feedback against the new layout.
                // Single ImageList_DragShowNolock wrap covers the
                // WM_VSCROLL repaint plus the subsequent state changes.
                bool ghostActive = (control->dragImageList != NULL);
                if (ghostActive) ImageList_DragShowNolock(FALSE);
                SendMessage(hWnd, WM_VSCROLL,
                            control->dragScrollDir < 0 ? SB_LINEUP : SB_LINEDOWN, 0);
                POINT pt; GetCursorPos(&pt); ScreenToClient(hWnd, &pt);
                ImageList_DragMove(pt.x, pt.y);
                UpdateDropFeedback(control, pt);
                if (ghostActive) ImageList_DragShowNolock(TRUE);
                return 0;
            }
            break;

        case WM_MOUSELEAVE:
            //: cursor left the tree client. Clear hover tint and
            // line-thickening. Re-armed on next WM_MOUSEMOVE.
#ifndef NDEBUG
            printf("[Link] WM_MOUSELEAVE\n"); fflush(stdout);
#endif
            ClearBracketHover(hWnd, control);
            break;

        case WM_KILLFOCUS:
            //: focus lost (Alt-Tab, modal dialog, other window).
            // Hover state should not survive a focus change.
            ClearBracketHover(hWnd, control);
            break;

        case WM_THEMECHANGED:
            //: Windows theme switched (default <-> dark <-> high-
            // contrast). RebuildBracketLayout consults IsHighContrastActive()
            // every paint, so the only thing missing is forcing a paint.
            // Hover state also clears because the palette colour may
            // have changed underneath us.
            ClearBracketHover(hWnd, control);
            InvalidateRect(hWnd, NULL, TRUE);
            break;

        case WM_SETTINGCHANGE:
            //: SPI_SETHIGHCONTRAST is broadcast as WM_SETTINGCHANGE
            // with wParam == SPI_SETHIGHCONTRAST (0x0043). Some systems
            // also broadcast it via "WindowMetrics" or similar string
            // payloads — repaint on any setting change is cheap and
            // safe; the bracket layout rebuild every paint handles
            // whatever changed.
            if (wParam == SPI_SETHIGHCONTRAST)
            {
                ClearBracketHover(hWnd, control);
                InvalidateRect(hWnd, NULL, TRUE);
            }
            break;

        case WM_LBUTTONUP:
            if (control != NULL && control->marqueeActive)
            {
                // One final hit-test using the release coordinates.
                // WM_MOUSEMOVE doesn't necessarily fire for the
                // exact pixel where the user releases — the last
                // mouse-move could be several pixels short of the
                // actual release point, leaving the bottom-most
                // swept row unselected. This pass catches anything
                // between the previous marqueeCurrent and the
                // release point.
                {
                    POINT releasePt = { GET_X_LPARAM(lParam),
                                         GET_Y_LPARAM(lParam) };
                    control->marqueeCurrent = releasePt;
                    RECT finalR;
                    finalR.left   = (std::min)(control->marqueeStart.x,
                                                releasePt.x);
                    finalR.top    = (std::min)(control->marqueeStart.y,
                                                releasePt.y);
                    finalR.right  = (std::max)(control->marqueeStart.x,
                                                releasePt.x);
                    finalR.bottom = (std::max)(control->marqueeStart.y,
                                                releasePt.y);
                    InflateRect(&finalR, 1, 1);

                    HTREEITEM hVis = TreeView_GetFirstVisible(hWnd);
                    while (hVis != NULL)
                    {
                        RECT rowR;
                        if (TreeView_GetItemRect(hWnd, hVis, &rowR, FALSE))
                        {
                            RECT hit;
                            if (IntersectRect(&hit, &finalR, &rowR))
                            {
                                TVITEM ti = { 0 };
                                ti.hItem = hVis;
                                ti.mask  = TVIF_PARAM;
                                if (TreeView_GetItem(hWnd, &ti))
                                {
                                    ParticleSystem::Emitter* e
                                        = (ParticleSystem::Emitter*)ti.lParam;
                                    if (e != NULL)
                                        control->marqueeSweptHits.insert(e);
                                }
                            }
                        }
                        hVis = TreeView_GetNextVisible(hWnd, hVis);
                    }

                    // Refresh multi-set from preCtrl ∪ sweptHits
                    // after the final pass.
                    std::set<ParticleSystem::Emitter*> finalMulti
                        = control->marqueePreCtrl;
                    for (auto* e : control->marqueeSweptHits)
                        finalMulti.insert(e);
                    control->multiSelection = finalMulti;

#ifndef NDEBUG
                    // Marquee debug: dump every visible row's rect
                    // and which side of the hit-test it landed on.
                    printf("[Marquee] start=(%ld,%ld) release=(%ld,%ld) finalR=(%ld,%ld,%ld,%ld)\n",
                           control->marqueeStart.x, control->marqueeStart.y,
                           releasePt.x, releasePt.y,
                           finalR.left, finalR.top, finalR.right, finalR.bottom);
                    HTREEITEM hDbg = TreeView_GetFirstVisible(hWnd);
                    int idx = 0;
                    while (hDbg != NULL)
                    {
                        RECT rowR2, labelR2;
                        TreeView_GetItemRect(hWnd, hDbg, &rowR2,   FALSE);
                        TreeView_GetItemRect(hWnd, hDbg, &labelR2, TRUE);
                        bool yH = (finalR.top    <= rowR2.bottom &&
                                   finalR.bottom >= rowR2.top);
                        bool xH = (finalR.left   <= labelR2.right &&
                                   finalR.right  >= labelR2.left);
                        TVITEM tiDbg = { 0 };
                        tiDbg.hItem = hDbg;
                        tiDbg.mask  = TVIF_PARAM;
                        ParticleSystem::Emitter* eDbg = NULL;
                        if (TreeView_GetItem(hWnd, &tiDbg))
                            eDbg = (ParticleSystem::Emitter*)tiDbg.lParam;
                        const char* name = (eDbg != NULL) ? eDbg->name.c_str() : "?";
                        bool inMulti = (eDbg != NULL &&
                                        control->multiSelection.count(eDbg) > 0);
                        printf("  row %d '%s' rowR=(%ld,%ld,%ld,%ld) labelR=(%ld,%ld,%ld,%ld) yHit=%d xHit=%d inMulti=%d\n",
                               idx, name,
                               rowR2.left, rowR2.top, rowR2.right, rowR2.bottom,
                               labelR2.left, labelR2.top, labelR2.right, labelR2.bottom,
                               yH, xH, inMulti);
                        idx++;
                        hDbg = TreeView_GetNextVisible(hWnd, hDbg);
                    }
                    fflush(stdout);
#endif
                }

                // Finalise marquee. Release capture, invalidate the
                // marquee rect so the frame erases, and reset anchor
                // to the primary if the selection ended up non-empty.
                //
                // IMPORTANT: set marqueeActive=false BEFORE
                // ReleaseCapture. ReleaseCapture fires
                // WM_CAPTURECHANGED synchronously; if marqueeActive
                // is still true at that moment, the cancellation
                // branch in WM_CAPTURECHANGED rolls multi-set back
                // to marqueePreCtrl (which is empty for non-Ctrl
                // marquee) — undoing the selection the user just
                // made.
                control->marqueeActive = false;
                ReleaseCapture();

                // Full-tree invalidate so the final marquee state's
                // secondary highlights (or lack thereof) repaint
                // every row, not just the marquee rect.
                InvalidateRect(hWnd, NULL, TRUE);

                control->marqueePreCtrl.clear();
                control->marqueeSweptHits.clear();

                if (control->selection != NULL &&
                    control->multiSelection.count(control->selection) > 0)
                {
                    control->selectionAnchor = control->selection;
                }
                else if (!control->multiSelection.empty())
                {
                    control->selectionAnchor = *control->multiSelection.begin();
                }

#ifndef NDEBUG
                printf("[Marquee] commit: %zu selected\n",
                       control->multiSelection.size());
                fflush(stdout);
#endif
                // Notify so the inspector reflects the final state
                // (greys when multi >= 2, ungreys when back to 1).
                NotifyParent(control, ELN_SELCHANGED);
                return 0;
            }
            if (control != NULL && control->dragSource != NULL)
            {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                DropTarget t = UpdateDropFeedback(control, pt);
                ParticleSystem::Emitter* moved = control->dragSource;

                // Tear down visual feedback BEFORE any modal popup
                // (slot picker for reparent) so the ghost and
                // highlight don't linger across it. Keep dragSource
                // set — EmitterList_IsDragging stays true through the
                // popup, which keeps the accelerator gate armed
                // against Ctrl+Z mid-popup.
                EndDragVisual(control);

                bool committed = false;
                if (t.kind == DROP_BETWEEN_GAP && control->system != NULL)
                {
                    committed = control->system->moveEmitterToRootIndex(moved, t.gap);
                }
                else if (t.kind == DROP_ONTO_EMITTER && control->system != NULL && t.targetEmitter != NULL)
                {
                    committed = CommitReparent(hWnd, control, moved, t.targetEmitter);
                }

                EndDragLogical(control);

                if (committed)
                {
                    // Tree shape may have changed — rebuild and re-select
                    // the moved emitter so the user's focus stays on it.
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

        case WM_RBUTTONDOWN:
            // Right-click during a drag would otherwise pop the
            // emitter context menu (via the dialog's WM_NOTIFY
            // NM_RCLICK handler) — confusing UX. Cancel the drag
            // instead. The right-click event is consumed.
            if (control != NULL && control->dragSource != NULL)
            {
                EndDrag(control);
                return 0;
            }
            break;

        case WM_CAPTURECHANGED:
            // Capture stolen (Alt+Tab, focus theft, the slot-picker's
            // TrackPopupMenu, our own ReleaseCapture, ...). Visual
            // teardown is idempotent so this is safe to call multiple
            // times. Logical state (dragSource) is left alone — the
            // mid-flight slot-picker case needs it preserved to keep
            // the accelerator gate armed; WM_LBUTTONUP's flow does
            // the final EndDragLogical.
            if (control != NULL && control->dragSource != NULL
                && (HWND)lParam != hWnd)
            {
                EndDragVisual(control);
            }
            // Marquee cancellation: capture stolen mid-marquee means
            // the user can't release-to-commit. Roll back to the
            // pre-drag selection (for additive case) or clear (for
            // replacement case).
            if (control != NULL && control->marqueeActive
                && (HWND)lParam != hWnd)
            {
                control->marqueeActive = false;
                control->multiSelection = control->marqueePreCtrl;
                control->marqueePreCtrl.clear();
                control->marqueeSweptHits.clear();
                InvalidateRect(hWnd, NULL, FALSE);
            }
            //: capture stolen with hover active. R4 mitigation —
            // clear hover defensively so it doesn't survive a drag /
            // modal that ate our WM_MOUSELEAVE.
            if (control != NULL) ClearBracketHover(hWnd, control);
            break;
    }
    WNDPROC wndProc = (WNDPROC)GetProp(hWnd, L"Old_WindowProc");
    return CallWindowProc(wndProc, hWnd, uMsg, wParam, lParam);
}

// Adds delta to every keyframe value on the TRACK_INDEX track of emitter.
// If the track has no keyframes (default value 0), inserts one at t=0 with value delta.
// Rebuilds the multiset rather than modifying keys in-place since std::multiset elements
// are const-qualified through their iterators.
static void ShiftIndexTrack(ParticleSystem::Emitter* emitter, float delta)
{
    ParticleSystem::Emitter::Track* track = emitter->tracks[ParticleSystem::TRACK_INDEX];
    if (track->keys.empty())
    {
        track->keys.insert(ParticleSystem::Emitter::Track::Key(0.0f, delta));
        return;
    }
    std::vector<ParticleSystem::Emitter::Track::Key> tmp(track->keys.begin(), track->keys.end());
    track->keys.clear();
    for (size_t i = 0; i < tmp.size(); ++i)
        track->keys.insert(ParticleSystem::Emitter::Track::Key(tmp[i].time, tmp[i].value + delta));
}

static INT_PTR CALLBACK IncrementIndexDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            HWND hSpin = GetDlgItem(hDlg, IDC_INCREMENT_SPIN);
            SendMessage(hSpin, UDM_SETRANGE32, 1, 999);
            SendMessage(hSpin, UDM_SETPOS32, 0, 1);
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                {
                    BOOL bOk;
                    int n = GetDlgItemInt(hDlg, IDC_INCREMENT_EDIT, &bOk, FALSE);
                    if (!bOk || n < 1) n = 1;
                    if (n > 999)      n = 999;
                    EndDialog(hDlg, n);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hDlg, 0);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Returns the user-chosen increment (≥1) or 0 if they cancelled.
static int ShowIncrementDialog(HWND hParent)
{
    return (int)DialogBoxParam(GetModuleHandle(NULL),
        MAKEINTRESOURCE(IDD_INCREMENT_INDEX), hParent, IncrementIndexDlgProc, 0);
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

            // Enable double-buffered painting on the tree ().
            // Our NM_CUSTOMDRAW handler paints the secondary-select
            // background for multi-selected rows; double-buffering
            // suppresses flicker on scroll and selection changes.
            SendMessage(control->hTree, TVM_SETEXTENDEDSTYLE,
                        TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);

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

                case NM_CUSTOMDRAW:
                    // Multi-select secondary-row paint + marquee
                    // overlay (). The tree's primary selection is
                    // painted by the default proc; we additionally
                    // fill the background of rows that are in
                    // multiSelection but NOT the primary, and overlay
                    // the marquee rectangle at CDDS_POSTPAINT when
                    // active.
                    if (hdr->hwndFrom == control->hTree)
                    {
                        NMTVCUSTOMDRAW* cd = (NMTVCUSTOMDRAW*)lParam;
                        switch (cd->nmcd.dwDrawStage)
                        {
                            case CDDS_PREPAINT:
                                //: rebuild the bracket layout cache
                                // every paint. The walk is O(N log N)
                                // and N is bounded by visible-tree
                                // expanded-emitter count — under 1 ms
                                // for realistic systems. Always rebuilding
                                // sidesteps the entire cache-staleness
                                // problem class (scroll, expand/collapse,
                                // window resize, group mutation, theme).
                                // The `valid` flag stays in the struct
                                // for future optimization if profiling
                                // surfaces a need.
                                RebuildBracketLayout(control);
                                SetWindowLongPtr(hWnd, DWLP_MSGRESULT,
                                                  CDRF_NOTIFYITEMDRAW |
                                                  CDRF_NOTIFYPOSTPAINT);
                                return TRUE;
                            case CDDS_ITEMPREPAINT:
                            {
                                // Two effects can stack on a single row:
                                //   - multi-select highlight
                                //     (clrTextBk override + CDRF_NEWFONT)
                                //   - hover-group tint (15% alpha
                                //     fill in CDDS_ITEMPOSTPAINT,
                                //     gated by CDRF_NOTIFYPOSTPAINT)
                                // Combine into a single return value
                                // bitwise; CDRF_NEWFONT == 0x02,
                                // CDRF_NOTIFYPOSTPAINT == 0x10, so the
                                // OR carries both flags through to
                                // the default proc cleanly.
                                ParticleSystem::Emitter* e
                                    = (ParticleSystem::Emitter*)cd->nmcd.lItemlParam;
                                DWORD ret = CDRF_DODEFAULT;
                                bool isMultiHighlight =
                                    (e != NULL &&
                                     control->multiSelection.size() >= 2 &&
                                     control->multiSelection.find(e)
                                        != control->multiSelection.end());
                                bool isHoverMember =
                                    (e != NULL &&
                                     control->hoveredGroupId != 0 &&
                                     e->linkGroup == control->hoveredGroupId);
                                if (isMultiHighlight)
                                {
                                    // Multi-select mode: paint EVERY
                                    // member (including the primary)
                                    // with the bright highlight. The
                                    // tree's default paint for the
                                    // primary greys out when the tree
                                    // doesn't have focus.
                                    cd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
                                    cd->clrText   = GetSysColor(COLOR_HIGHLIGHTTEXT);
                                    ret |= CDRF_NEWFONT;
                                }
                                if (isHoverMember)
                                {
                                    ret |= CDRF_NOTIFYPOSTPAINT;
                                }
                                if (ret != CDRF_DODEFAULT)
                                {
                                    SetWindowLongPtr(hWnd, DWLP_MSGRESULT, ret);
                                    return TRUE;
                                }
                                break;
                            }
                            case CDDS_ITEMPOSTPAINT:
                            {
                                // hover-group tint. Painted AFTER
                                // the row's default paint (and after
                                // any multi-select COLOR_HIGHLIGHT
                                // background) so the tint composes via
                                // AlphaBlend over whatever the row is
                                // currently showing.
                                ParticleSystem::Emitter* e
                                    = (ParticleSystem::Emitter*)cd->nmcd.lItemlParam;
                                if (e == NULL ||
                                    control->hoveredGroupId == 0 ||
                                    e->linkGroup != control->hoveredGroupId)
                                    break;
                                COLORREF tint = 0;
                                const BracketLayout& L = control->bracketLayout;
                                for (size_t gi = 0; gi < L.groups.size(); ++gi)
                                {
                                    if (L.groups[gi].groupId
                                        == control->hoveredGroupId)
                                    {
                                        tint = L.groups[gi].colour;
                                        break;
                                    }
                                }
                                // 1x1 source DDB stretched across the
                                // row rect with sourceConstantAlpha=38
                                // (~15%). AlphaFormat=0 → source treated
                                // opaque; blend reduces to
                                //   dst = src * 0.15 + dst * 0.85
                                // which is exactly the linear tint
                                // we want.
                                HDC hdc = cd->nmcd.hdc;
                                HDC hdcMem = CreateCompatibleDC(hdc);
                                HBITMAP hbm = CreateCompatibleBitmap(hdc, 1, 1);
                                HGDIOBJ oldBm = SelectObject(hdcMem, hbm);
                                SetPixel(hdcMem, 0, 0, tint);
                                BLENDFUNCTION bf;
                                bf.BlendOp             = AC_SRC_OVER;
                                bf.BlendFlags          = 0;
                                bf.SourceConstantAlpha = 38;
                                bf.AlphaFormat         = 0;
                                RECT rr = cd->nmcd.rc;
                                AlphaBlend(hdc,
                                            rr.left, rr.top,
                                            rr.right - rr.left,
                                            rr.bottom - rr.top,
                                            hdcMem, 0, 0, 1, 1,
                                            bf);
                                SelectObject(hdcMem, oldBm);
                                DeleteObject(hbm);
                                DeleteDC(hdcMem);
                                break;
                            }
                            case CDDS_POSTPAINT:
                            {
                                //: paint link-group brackets first
                                // (lane lines, then per-member dots +
                                // stubs), then the marquee frame
                                // on top so an active marquee is always
                                // visible over brackets.
                                const BracketLayout& L = control->bracketLayout;
                                if (!L.groups.empty())
                                {
                                    HDC hdc = cd->nmcd.hdc;
                                    RECT clientR;
                                    GetClientRect(hWnd, &clientR);
                                    // Clip to tree client so lines never
                                    // bleed past the bottom or top edges.
                                    HRGN hClip = CreateRectRgn(
                                        clientR.left,  clientR.top,
                                        clientR.right, clientR.bottom);
                                    SelectClipRgn(hdc, hClip);
                                    for (size_t gi = 0; gi < L.groups.size(); ++gi)
                                    {
                                        const BracketLayout::Group& g = L.groups[gi];
                                        // Lane 0 sits just right of
                                        // the longest label; higher
                                        // lanes extend rightward into
                                        // the gutter toward client.right.
                                        const int dotX   = L.rightEdgeOffset
                                                         + g.lane * L.laneWidth;
                                        // hover: thicken the line
                                        // to 2x stroke when this group
                                        // is hovered. The dot stays its
                                        // base size — the user's eye
                                        // tracks the line thickness as
                                        // the primary hover cue.
                                        const bool hovered =
                                            (g.groupId == control->hoveredGroupId);
                                        const int stroke = hovered
                                                         ? L.strokeWidth * 2
                                                         : L.strokeWidth;
                                        HPEN    pen      = CreatePen(PS_SOLID,
                                                                      stroke,
                                                                      g.colour);
                                        HGDIOBJ oldPen   = SelectObject(hdc, pen);
                                        // Lane line connects topmost to
                                        // bottommost dot. LineTo is
                                        // half-open at the endpoint, so
                                        // step one past maxY to ensure
                                        // the bottom pixel paints.
                                        MoveToEx(hdc, dotX, g.minY, NULL);
                                        LineTo  (hdc, dotX, g.maxY + 1);
                                        HBRUSH  brush    = CreateSolidBrush(g.colour);
                                        HGDIOBJ oldBrush = SelectObject(hdc, brush);
                                        for (size_t mi = 0; mi < g.members.size(); ++mi)
                                        {
                                            LONG y = g.members[mi].centreY;
                                            // Stub pointing leftward
                                            // toward the row text.
                                            MoveToEx(hdc, dotX - L.stubLength, y, NULL);
                                            LineTo  (hdc, dotX, y);
                                            // Filled dot at member row.
                                            Ellipse(hdc,
                                                    dotX - L.dotRadius,
                                                    y    - L.dotRadius,
                                                    dotX + L.dotRadius + 1,
                                                    y    + L.dotRadius + 1);
                                        }
                                        SelectObject(hdc, oldBrush);
                                        SelectObject(hdc, oldPen);
                                        DeleteObject(brush);
                                        DeleteObject(pen);
                                    }
                                    SelectClipRgn(hdc, NULL);
                                    DeleteObject(hClip);
                                }

                                if (control->marqueeActive)
                                {
                                    RECT mr;
                                    mr.left   = (std::min)(control->marqueeStart.x,
                                                          control->marqueeCurrent.x);
                                    mr.top    = (std::min)(control->marqueeStart.y,
                                                          control->marqueeCurrent.y);
                                    mr.right  = (std::max)(control->marqueeStart.x,
                                                          control->marqueeCurrent.x);
                                    mr.bottom = (std::max)(control->marqueeStart.y,
                                                          control->marqueeCurrent.y);
                                    // Frame in system-highlight colour;
                                    // 1 px border is plenty against a
                                    // double-buffered tree.
                                    HBRUSH br = CreateSolidBrush(
                                        GetSysColor(COLOR_HIGHLIGHT));
                                    FrameRect(cd->nmcd.hdc, &mr, br);
                                    DeleteObject(br);
                                }
                                break;
                            }
                        }
                    }
                    break;

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

                        // Multi-select bookkeeping (): right-click
                        // OUTSIDE the current multi-set resets the set
                        // to a single-item set on the clicked emitter
                        // (Explorer convention). Right-click INSIDE the
                        // multi-set preserves the set — the user is
                        // about to run a batch action on it.
                        if (tvht.hItem != NULL)
                        {
                            TVITEM ti = { 0 };
                            ti.hItem = tvht.hItem;
                            ti.mask  = TVIF_PARAM;
                            if (TreeView_GetItem(control->hTree, &ti))
                            {
                                ParticleSystem::Emitter* rc
                                    = (ParticleSystem::Emitter*)ti.lParam;
                                if (rc != NULL &&
                                    control->multiSelection.find(rc)
                                        == control->multiSelection.end())
                                {
                                    control->multiSelection.clear();
                                    control->multiSelection.insert(rc);
                                    control->selectionAnchor = rc;
                                    InvalidateRect(control->hTree, NULL, FALSE);
                                }
                            }
                        }
                        else
                        {
                            // Right-click on empty area clears.
                            control->multiSelection.clear();
                            control->selectionAnchor = NULL;
                            InvalidateRect(control->hTree, NULL, FALSE);
                        }
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

                        // Link-group menu items (+). Built
                        // dynamically based on selection size and link state.
                        //
                        // Single-emitter (multiSelection.size() <= 1):
                        //   selection unlinked:
                        //     - "Link with..." popup (other unlinked emitters)
                        //     - "Add to link group..." popup (existing groups)
                        //   selection linked:
                        //     - "Remove from link group"
                        //     - "Dissolve link group"
                        //
                        // Multi-emitter (multiSelection.size() >= 2):
                        //   all unlinked:
                        //     - "Link selected" (creates a new group)
                        //     - "Add selected to link group..." popup
                        //   all in same group:
                        //     - "Dissolve link group"
                        //   mixed: no multi action (right-click individual
                        //     emitters one at a time)
                        //
                        // `linkMenuCandidates` and `linkMenuGroups` carry the
                        // ID → emitter / ID → group mapping for the action
                        // dispatch below.
                        std::vector<ParticleSystem::Emitter*> linkMenuCandidates;
                        std::vector<uint32_t>                 linkMenuGroups;
                        HMENU hLinkWithSub = NULL;
                        HMENU hLinkAddSub  = NULL;

                        // Snapshot multi-set state for menu gating below.
                        //   multiAllUnlinked: every member has linkGroup == 0
                        //   multiOneGroup:    every member is in the SAME non-zero group
                        //   multiSingleGroupPlusUnlinked: exactly one group is
                        //                                 represented, AND ≥1 member
                        //                                 is unlinked. Enables
                        //                                 "Add unlinked to Group N".
                        //   multiGroupId:     the represented group's id (when valid)
                        size_t multiSize        = control->multiSelection.size();
                        bool   multiAllUnlinked = (multiSize >= 2);
                        bool   multiOneGroup    = (multiSize >= 2);
                        uint32_t multiGroupId   = 0;
                        size_t numUnlinkedInMulti = 0;
                        std::set<uint32_t> groupsInMulti;
                        for (auto* e : control->multiSelection)
                        {
                            if (e->linkGroup == 0)
                            {
                                multiOneGroup = false;
                                numUnlinkedInMulti++;
                            }
                            else
                            {
                                multiAllUnlinked = false;
                                groupsInMulti.insert(e->linkGroup);
                            }
                            if (multiGroupId == 0)        multiGroupId = e->linkGroup;
                            else if (e->linkGroup != multiGroupId) multiOneGroup = false;
                        }
                        if (multiGroupId == 0) multiOneGroup = false;
                        bool multiSingleGroupPlusUnlinked
                            = (multiSize >= 2 &&
                               groupsInMulti.size() == 1 &&
                               numUnlinkedInMulti > 0);
                        uint32_t singleRepresentedGroup
                            = multiSingleGroupPlusUnlinked
                              ? *groupsInMulti.begin()
                              : 0;

                        if (control->selection != NULL)
                        {
                            const std::vector<ParticleSystem::Emitter*>& emitters
                                = control->system->getEmitters();
                            std::vector<uint32_t> existingGroups
                                = GetAllLinkGroupIds(*control->system);

                            if (multiSize <= 1 && control->selection->linkGroup == 0)
                            {
                                // Single-emitter unlinked: build "Link with..."
                                // listing other unlinked emitters (cap at the
                                // reserved range).
                                for (size_t i = 0; i < emitters.size(); i++)
                                {
                                    ParticleSystem::Emitter* e = emitters[i];
                                    if (e == control->selection)  continue;
                                    if (e->linkGroup != 0)        continue;
                                    if (linkMenuCandidates.size() >=
                                        (size_t)(ID_EMITTER_LINK_WITH_LAST -
                                                 ID_EMITTER_LINK_WITH_FIRST + 1)) break;
                                    linkMenuCandidates.push_back(e);
                                }
                                if (!linkMenuCandidates.empty())
                                {
                                    hLinkWithSub = CreatePopupMenu();
                                    for (size_t i = 0; i < linkMenuCandidates.size(); i++)
                                    {
                                        std::wstring label
                                            = AnsiToWide(linkMenuCandidates[i]->name);
                                        AppendMenuW(hLinkWithSub, MF_STRING,
                                                    ID_EMITTER_LINK_WITH_FIRST + i,
                                                    label.c_str());
                                    }
                                }

                                // Build "Add to link group..." submenu listing
                                // existing groups (cap at the reserved range).
                                for (size_t i = 0; i < existingGroups.size(); i++)
                                {
                                    if (linkMenuGroups.size() >=
                                        (size_t)(ID_EMITTER_LINK_ADD_LAST -
                                                 ID_EMITTER_LINK_ADD_FIRST + 1)) break;
                                    linkMenuGroups.push_back(existingGroups[i]);
                                }
                            }
                            else if (multiSize >= 2 && multiAllUnlinked)
                            {
                                // Multi-emitter all-unlinked: build the
                                // "Add selected to link group..." submenu
                                // from existing groups (same source as
                                // single case; just operated on the
                                // multi-set at dispatch time).
                                for (size_t i = 0; i < existingGroups.size(); i++)
                                {
                                    if (linkMenuGroups.size() >=
                                        (size_t)(ID_EMITTER_LINK_ADD_LAST -
                                                 ID_EMITTER_LINK_ADD_FIRST + 1)) break;
                                    linkMenuGroups.push_back(existingGroups[i]);
                                }
                            }
                            else if (multiSingleGroupPlusUnlinked)
                            {
                                // Mixed selection but only ONE group is
                                // represented + some unlinked emitters.
                                // Offer "Add unlinked to Group N" — the
                                // already-linked members stay where
                                // they are, the unlinked ones join.
                                linkMenuGroups.push_back(singleRepresentedGroup);
                            }

                            // Realise the linkMenuGroups list into a popup
                            // (shared between single and multi cases).
                            if (!linkMenuGroups.empty())
                            {
                                hLinkAddSub = CreatePopupMenu();
                                for (size_t i = 0; i < linkMenuGroups.size(); i++)
                                {
                                    std::vector<ParticleSystem::Emitter*> mems
                                        = GetLinkGroupMembers(*control->system,
                                                               linkMenuGroups[i]);
                                    wchar_t buf[128];
                                    swprintf(buf, 128, L"Group %u  (%s + %zu more)",
                                             linkMenuGroups[i],
                                             mems.empty() ? L""
                                              : AnsiToWide(mems[0]->name).c_str(),
                                             mems.size() > 1 ? mems.size() - 1 : 0);
                                    AppendMenuW(hLinkAddSub, MF_STRING,
                                                ID_EMITTER_LINK_ADD_FIRST + i,
                                                buf);
                                }
                            }

                            // Stitch the link-group section onto the bottom of
                            // the popup.
                            bool addedSeparator = false;
                            auto ensureSeparator = [&]() {
                                if (!addedSeparator) {
                                    AppendMenuW(hPopupMenu, MF_SEPARATOR, 0, NULL);
                                    addedSeparator = true;
                                }
                            };

                            if (multiSize >= 2 && multiAllUnlinked)
                            {
                                // Multi-emitter, all unlinked: "Link selected"
                                // creates one new group containing all members
                                // of the multi-set.
                                ensureSeparator();
                                wchar_t selLabel[80];
                                swprintf(selLabel, 80,
                                         L"&Link selected (%zu emitters)",
                                         multiSize);
                                AppendMenuW(hPopupMenu, MF_STRING,
                                            ID_EMITTER_LINK_SELECTED, selLabel);
                                if (hLinkAddSub != NULL)
                                {
                                    AppendMenuW(hPopupMenu, MF_POPUP,
                                                (UINT_PTR)hLinkAddSub,
                                                L"&Add selected to link group...");
                                }
                            }
                            else if (multiSize >= 2 && multiOneGroup)
                            {
                                // Multi-emitter, all in the same group:
                                // offer Dissolve directly.
                                ensureSeparator();
                                AppendMenuW(hPopupMenu, MF_STRING,
                                            ID_EMITTER_LINK_DISSOLVE,
                                            L"&Dissolve link group");
                            }
                            else if (multiSingleGroupPlusUnlinked)
                            {
                                // Mixed selection: some unlinked + some
                                // in a single group. Direct menu item
                                // (no submenu — only one target makes
                                // sense). Dispatch via ID_..._ADD_FIRST,
                                // which already filters joiners to
                                // currently-unlinked at dispatch time.
                                ensureSeparator();
                                wchar_t addLabel[160];
                                swprintf(addLabel, 160,
                                         L"Add &unlinked to Group %u (%zu emitter%s)",
                                         singleRepresentedGroup,
                                         numUnlinkedInMulti,
                                         numUnlinkedInMulti == 1 ? L"" : L"s");
                                AppendMenuW(hPopupMenu, MF_STRING,
                                            ID_EMITTER_LINK_ADD_FIRST,
                                            addLabel);
                            }
                            else if (multiSize <= 1)
                            {
                                // Single-emitter path.
                                if (hLinkWithSub != NULL)
                                {
                                    ensureSeparator();
                                    AppendMenuW(hPopupMenu, MF_POPUP,
                                                (UINT_PTR)hLinkWithSub,
                                                L"Link &with...");
                                }
                                if (hLinkAddSub != NULL)
                                {
                                    ensureSeparator();
                                    AppendMenuW(hPopupMenu, MF_POPUP,
                                                (UINT_PTR)hLinkAddSub,
                                                L"&Add to link group...");
                                }
                                if (control->selection->linkGroup != 0)
                                {
                                    ensureSeparator();
                                    // "Remove" label conveys auto-dissolve when
                                    // the group would be reduced to 1 member.
                                    std::vector<ParticleSystem::Emitter*> grp
                                        = GetLinkGroupMembers(*control->system,
                                                               control->selection->linkGroup);
                                    wchar_t removeLabel[80];
                                    if (grp.size() == 2)
                                    {
                                        swprintf(removeLabel, 80,
                                                 L"&Remove from link group (dissolves Group %u)",
                                                 control->selection->linkGroup);
                                    }
                                    else
                                    {
                                        wcscpy_s(removeLabel, 80, L"&Remove from link group");
                                    }
                                    AppendMenuW(hPopupMenu, MF_STRING,
                                                ID_EMITTER_LINK_REMOVE, removeLabel);
                                    AppendMenuW(hPopupMenu, MF_STRING,
                                                ID_EMITTER_LINK_DISSOLVE,
                                                L"&Dissolve link group");
                                }
                            }
                            // (multiSize >= 2 mixed: no menu items added;
                            //  user must right-click individual emitters
                            //  one at a time.)
                        }

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
                            case ID_EMITTER_DUPLICATE:           EmitterList_DuplicateEmitter(hWnd); break;
                            case ID_EMITTER_DUPLICATE_INC_INDEX: EmitterList_DuplicateEmitter(hWnd, 1.0f); break;
                            case ID_EMITTER_DUPLICATE_INC_INDEX_N:
                            {
                                int n = ShowIncrementDialog(hWnd);
                                if (n > 0) EmitterList_DuplicateEmitter(hWnd, (float)n);
                                break;
                            }
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

                            case ID_EMITTER_LINK_REMOVE:
                                if (control->selection != NULL &&
                                    control->selection->linkGroup != 0)
                                {
                                    // Capture members of the soon-to-be-modified
                                    // group BEFORE the leave so the refresh pass
                                    // hits exactly the right rows (LeaveLinkGroup
                                    // may auto-dissolve a second member).
                                    std::vector<ParticleSystem::Emitter*> affected
                                        = GetLinkGroupMembers(*control->system,
                                                               control->selection->linkGroup);
                                    LeaveLinkGroup(*control->system, control->selection);
                                    for (size_t i = 0; i < affected.size(); i++)
                                    {
                                        HTREEITEM hi = FindTreeItemByEmitter(
                                            control->hTree,
                                            TreeView_GetRoot(control->hTree),
                                            affected[i]);
                                        RefreshEmitterTreeText(control->hTree, hi, affected[i]);
                                    }
                                    NotifyParent(control, ELN_LISTCHANGED);
                                }
                                break;

                            case ID_EMITTER_LINK_DISSOLVE:
                                if (control->selection != NULL &&
                                    control->selection->linkGroup != 0)
                                {
                                    uint32_t gid = control->selection->linkGroup;
                                    std::vector<ParticleSystem::Emitter*> mems
                                        = GetLinkGroupMembers(*control->system, gid);
                                    DissolveLinkGroup(*control->system, gid);
                                    for (size_t i = 0; i < mems.size(); i++)
                                    {
                                        HTREEITEM hi = FindTreeItemByEmitter(
                                            control->hTree,
                                            TreeView_GetRoot(control->hTree),
                                            mems[i]);
                                        RefreshEmitterTreeText(control->hTree, hi, mems[i]);
                                    }
                                    NotifyParent(control, ELN_LISTCHANGED);
                                }
                                break;

                            case ID_EMITTER_LINK_SELECTED:
                                // Multi-emitter: create a new group from
                                // every member of the multi-selection set.
                                // Menu gating guarantees all members are
                                // currently unlinked.
                                if (control->multiSelection.size() >= 2)
                                {
                                    // Flatten multi-set into a vector
                                    // with the selectionAnchor first
                                    // (canonical) when available, else
                                    // topmost in tree order. The
                                    // anchor is the most recently
                                    // plain- or Ctrl-clicked emitter,
                                    // so the natural rule is "the
                                    // emitter you most recently
                                    // clicked governs the group."
                                    std::vector<ParticleSystem::Emitter*> mems;
                                    bool anchorIsCanonical = false;
                                    if (control->selectionAnchor != NULL &&
                                        control->multiSelection.count(
                                            control->selectionAnchor) > 0)
                                    {
                                        mems.push_back(control->selectionAnchor);
                                        anchorIsCanonical = true;
                                    }
                                    const std::vector<ParticleSystem::Emitter*>& all
                                        = control->system->getEmitters();
                                    for (size_t i = 0; i < all.size(); i++)
                                    {
                                        if (control->multiSelection.count(all[i]) > 0 &&
                                            all[i] != control->selectionAnchor)
                                            mems.push_back(all[i]);
                                    }
                                    if (mems.size() >= 2)
                                    {
                                        // Pre-diff each follower against
                                        // the canonical. If ANY follower
                                        // would have non-empty diff, show
                                        // a confirmation listing all
                                        // affected fields across the set.
                                        std::vector<std::string> combined;
                                        for (size_t i = 1; i < mems.size(); i++)
                                        {
                                            std::vector<std::string> d
                                                = DiffNonExemptParams(*mems[0], *mems[i]);
                                            for (size_t j = 0; j < d.size(); j++)
                                            {
                                                if (std::find(combined.begin(),
                                                              combined.end(), d[j])
                                                        == combined.end())
                                                    combined.push_back(d[j]);
                                            }
                                        }
                                        wchar_t srcDesc[160];
                                        swprintf(srcDesc, 160,
                                                 L"\"%s\" (%s)",
                                                 AnsiToWide(mems[0]->name).c_str(),
                                                 anchorIsCanonical
                                                  ? L"your most recently clicked emitter"
                                                  : L"the topmost selected emitter");
                                        wchar_t victimDesc[128];
                                        swprintf(victimDesc, 128,
                                                 L"%zu other emitter(s)",
                                                 mems.size() - 1);
                                        bool proceed = ConfirmLinkOverwrite(
                                            hWnd, L"Link selected",
                                            victimDesc, srcDesc, combined);

                                        if (proceed)
                                        {
                                            uint32_t newId
                                                = CreateLinkGroup(*control->system,
                                                                   mems);
                                            if (newId != 0)
                                            {
                                                for (size_t i = 0; i < mems.size(); i++)
                                                {
                                                    HTREEITEM hi
                                                        = FindTreeItemByEmitter(
                                                            control->hTree,
                                                            TreeView_GetRoot(control->hTree),
                                                            mems[i]);
                                                    RefreshEmitterTreeText(
                                                        control->hTree, hi, mems[i]);
                                                }
                                                NotifyParent(control, ELN_LISTCHANGED);
                                            }
                                        }
                                    }
                                }
                                break;

                            default:
                                // Dynamic menu-ID dispatch for the "Link with..."
                                // and "Add to link group..." submenus.
                                if (id >= ID_EMITTER_LINK_WITH_FIRST &&
                                    id <= ID_EMITTER_LINK_WITH_LAST)
                                {
                                    size_t idx = (size_t)(id - ID_EMITTER_LINK_WITH_FIRST);
                                    if (idx < linkMenuCandidates.size() &&
                                        control->selection != NULL)
                                    {
                                        ParticleSystem::Emitter* partner
                                            = linkMenuCandidates[idx];

                                        // Pre-diff. CreateLinkGroup always
                                        // anchors on members[0] (the
                                        // right-clicked emitter), so the
                                        // partner is the one whose values
                                        // get overwritten. Spell that out.
                                        std::vector<std::string> diffs
                                            = DiffNonExemptParams(*control->selection,
                                                                   *partner);
                                        std::wstring victimName
                                            = AnsiToWide(partner->name);
                                        std::wstring sourceDesc = L"\"";
                                        sourceDesc += AnsiToWide(control->selection->name);
                                        sourceDesc += L"\"";
                                        bool proceed = ConfirmLinkOverwrite(
                                            hWnd, L"Link with...",
                                            victimName, sourceDesc, diffs);

                                        if (proceed)
                                        {
                                            std::vector<ParticleSystem::Emitter*> mems;
                                            mems.push_back(control->selection);
                                            mems.push_back(partner);
                                            uint32_t newId
                                                = CreateLinkGroup(*control->system, mems);
                                            if (newId != 0)
                                            {
                                                for (size_t k = 0; k < mems.size(); k++)
                                                {
                                                    HTREEITEM hi = FindTreeItemByEmitter(
                                                        control->hTree,
                                                        TreeView_GetRoot(control->hTree),
                                                        mems[k]);
                                                    RefreshEmitterTreeText(control->hTree,
                                                                            hi, mems[k]);
                                                }
                                                NotifyParent(control, ELN_LISTCHANGED);
                                            }
                                        }
                                    }
                                }
                                else if (id >= ID_EMITTER_LINK_ADD_FIRST &&
                                         id <= ID_EMITTER_LINK_ADD_LAST)
                                {
                                    size_t idx = (size_t)(id - ID_EMITTER_LINK_ADD_FIRST);
                                    if (idx < linkMenuGroups.size() &&
                                        control->selection != NULL)
                                    {
                                        uint32_t target = linkMenuGroups[idx];

                                        // JoinLinkGroup anchors on the
                                        // group's canonical member (the
                                        // first in tree order), so the
                                        // joiner(s) are the ones whose
                                        // values get overwritten. Spell
                                        // that out.
                                        std::vector<ParticleSystem::Emitter*> grp
                                            = GetLinkGroupMembers(*control->system, target);
                                        if (grp.empty()) break;

                                        // Decide which set of emitters
                                        // joins: the multi-set (when
                                        // 2+ selected and gating put us
                                        // here) or just the primary.
                                        std::vector<ParticleSystem::Emitter*> joiners;
                                        if (control->multiSelection.size() >= 2)
                                        {
                                            const std::vector<ParticleSystem::Emitter*>& all
                                                = control->system->getEmitters();
                                            for (size_t i = 0; i < all.size(); i++)
                                            {
                                                if (control->multiSelection.count(all[i]) > 0
                                                    && all[i]->linkGroup == 0)
                                                    joiners.push_back(all[i]);
                                            }
                                        }
                                        else
                                        {
                                            joiners.push_back(control->selection);
                                        }

                                        // Combined diff across all joiners
                                        // vs the canonical member.
                                        std::vector<std::string> combined;
                                        for (size_t i = 0; i < joiners.size(); i++)
                                        {
                                            std::vector<std::string> d
                                                = DiffNonExemptParams(*joiners[i], *grp[0]);
                                            for (size_t j = 0; j < d.size(); j++)
                                            {
                                                if (std::find(combined.begin(),
                                                              combined.end(), d[j])
                                                        == combined.end())
                                                    combined.push_back(d[j]);
                                            }
                                        }

                                        wchar_t victimDesc[128];
                                        if (joiners.size() == 1)
                                        {
                                            swprintf(victimDesc, 128, L"\"%s\"",
                                                     AnsiToWide(joiners[0]->name).c_str());
                                        }
                                        else
                                        {
                                            swprintf(victimDesc, 128,
                                                     L"%zu selected emitter(s)",
                                                     joiners.size());
                                        }
                                        wchar_t gbuf[96];
                                        swprintf(gbuf, 96,
                                                 L"Group %u's existing values (currently set by \"",
                                                 target);
                                        std::wstring sourceDesc = gbuf;
                                        sourceDesc += AnsiToWide(grp[0]->name);
                                        sourceDesc += L"\")";
                                        bool proceed = ConfirmLinkOverwrite(
                                            hWnd, L"Add to link group",
                                            victimDesc, sourceDesc, combined);

                                        if (proceed)
                                        {
                                            size_t joined = 0;
                                            for (size_t i = 0; i < joiners.size(); i++)
                                            {
                                                if (JoinLinkGroup(*control->system,
                                                                   joiners[i], target))
                                                {
                                                    HTREEITEM hi = FindTreeItemByEmitter(
                                                        control->hTree,
                                                        TreeView_GetRoot(control->hTree),
                                                        joiners[i]);
                                                    RefreshEmitterTreeText(
                                                        control->hTree, hi, joiners[i]);
                                                    joined++;
                                                }
                                            }
                                            if (joined > 0)
                                                NotifyParent(control, ELN_LISTCHANGED);
                                        }
                                    }
                                }
                                break;
                        }

                        // The static menu is LoadMenu'd once and reused across
                        // right-clicks, so we must remove every dynamic item
                        // (link entries + the separator) before returning,
                        // otherwise they pile up on the next popup. DeleteMenu
                        // also destroys any attached submenu (per MSDN), so no
                        // explicit DestroyMenu on the popups — that would be
                        // a double-free. Walk from end, stop at the original
                        // Delete item which is always the last static entry.
                        (void)hLinkWithSub; (void)hLinkAddSub;
                        {
                            int n = GetMenuItemCount(hPopupMenu);
                            while (n > 0)
                            {
                                UINT mid = GetMenuItemID(hPopupMenu, n - 1);
                                if (mid == (UINT)ID_EDIT_DELETE) break;
                                DeleteMenu(hPopupMenu, n - 1, MF_BYPOSITION);
                                n--;
                            }
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

                    // Multi-select bookkeeping (). Mouse clicks
                    // already updated the set via WM_LBUTTONDOWN in
                    // EmitterTreeViewWindowProc. Keyboard nav resets
                    // the multi-set to {newPrimary}. Programmatic
                    // selection (e.g. tree rebuild after load, drag-
                    // drop completion) re-seeds the multi-set only
                    // when it would otherwise drift out of invariant
                    // (empty set or set not containing the new
                    // primary).
                    if (nmtv->action == TVC_BYKEYBOARD)
                    {
                        control->multiSelection.clear();
                        if (control->selection != NULL)
                            control->multiSelection.insert(control->selection);
                        control->selectionAnchor = control->selection;
                        InvalidateRect(control->hTree, NULL, FALSE);
                    }
                    else if (control->selection != NULL &&
                             (control->multiSelection.empty() ||
                              control->multiSelection.find(control->selection)
                                  == control->multiSelection.end()))
                    {
                        // Invariant repair: multi-set must contain
                        // the primary. Drag-drop completion and
                        // initial-load TreeView_SelectItem both reach
                        // here with TVC_UNKNOWN.
                        control->multiSelection.clear();
                        control->multiSelection.insert(control->selection);
                        control->selectionAnchor = control->selection;
                        InvalidateRect(control->hTree, NULL, FALSE);
                    }

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

                    // Refuse if a label edit is in progress (drag
                    // mid-rename would be confusing) or if there's
                    // only one emitter in the system (nothing to drop
                    // onto). Children-as-source is now allowed since
                    // reparent landed in this PR; UpdateDropFeedback
                    // refuses between-gap drops when source is a
                    // child, so the user gets IDC_NO instead of an
                    // unexpected reorder gesture.
                    if (src == NULL) break;
                    if (TreeView_GetEditControl(control->hTree) != NULL) break;
                    if (control->system != NULL && control->system->getEmitters().size() < 2) break;

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

                    // The tree's display text may carry a `[L<n>] ` link-
                    // group prefix; the edit control inherits that prefix
                    // by default. Replace its content with the bare name
                    // so the user edits just the identifier they own.
                    if (control->selection != NULL && control->selection->linkGroup != 0)
                    {
                        std::wstring bare = AnsiToWide(control->selection->name);
                        SetWindowText(hEdit, bare.c_str());
                    }
                    break;
                }

                case TVN_ENDLABELEDIT:
                {
                    NMTVDISPINFO* nmtvdi = (NMTVDISPINFO*)lParam;
                    if (nmtvdi->item.pszText != NULL)
                    {
                        // Defensive: if the user managed to type a `[L<n>] `
                        // prefix manually (or paste from the displayed text
                        // elsewhere), strip it before persisting.
                        std::wstring edited = nmtvdi->item.pszText;
                        StripLinkGroupPrefix(edited);
                        control->selection->name = WideToAnsi(edited.c_str());

                        // Rebuild the display text with the (possibly
                        // restored) link-group prefix so the tree row
                        // reflects current group state after rename.
                        std::wstring display = FormatEmitterDisplayName(control->selection);
                        nmtvdi->item.pszText = (LPWSTR)display.c_str();
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
        control->dragDropHighlight = NULL;
        control->dragScrollTimer   = 0;
        control->dragScrollDir     = 0;
        control->selectionAnchor   = NULL;
        control->marqueeActive     = false;
        control->marqueeAdditive   = false;
        control->marqueeStart.x    = 0;
        control->marqueeStart.y    = 0;
        control->marqueeCurrent    = control->marqueeStart;
        // multiSelection and marqueePreCtrl are default-constructed

        // bracket layout cache + hover state.
        control->bracketLayout.numLanes       = 0;
        control->bracketLayout.laneWidth      = 0;
        control->bracketLayout.dotRadius      = 0;
        control->bracketLayout.stubLength     = 0;
        control->bracketLayout.strokeWidth    = 0;
        control->bracketLayout.rightEdgeOffset = 0;
        control->bracketLayout.scrollOriginY  = 0;
        control->bracketLayout.hcMode         = false;
        control->bracketLayout.valid          = false;
        control->bracketLayout.prevRightEdgeOffset = -1;   // first paint forces no-op shift
        control->bracketLayout.prevLaneWidth       = -1;
        control->bracketLayout.prevNumLanes        = -1;
        control->hoveredGroupId               = 0;
        control->mouseTrackingArmed           = false;

#ifndef NDEBUG
        DebugVerifyBracketPalette();
#endif

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
			wstring text = FormatEmitterDisplayName(&onLife);

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
			wstring text = FormatEmitterDisplayName(&onDeath);

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
    control->selectionAnchor = NULL;
    control->multiSelection.clear();
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
			    wstring name = FormatEmitterDisplayName(emitters[i]);
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
        //: any prior system's bracket layout + hover state is now
        // stale. Invalidate cache and clear hover; next paint rebuilds.
        InvalidateBracketLayout(control);
        control->hoveredGroupId = 0;
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
    if (control == NULL || control->system == NULL) return;
    if (control->selection == NULL && control->multiSelection.empty()) return;

    // (Q4 follow-up): delete every emitter in multiSelection,
    // not just the primary. Single-select case is the multi-set
    // of size 1 — same code path.
    //
    // multiSelection contains the primary by invariant. We snapshot
    // before iterating because deleteEmitter recursively deletes a
    // subtree, so a target later in the list may already be gone
    // (parent was deleted in an earlier iteration, child cascaded).
    // Re-querying the system's emitter vector each iteration filters
    // out cascade-deleted targets without touching dangling pointers.
    //
    // ParticleSystem::deleteEmitter doesn't fire any notification;
    // the single ELN_LISTCHANGED at the end groups all N deletions
    // into one undo step (main.cpp's ELN_LISTCHANGED handler calls
    // CaptureUndo with coalesceKey=0, which never folds). So a
    // single Ctrl-Z after bracket-select-then-Delete restores all
    // N emitters together — matches Q5: undo restores the data,
    // multi-set stays empty (user re-selects).
    std::vector<ParticleSystem::Emitter*> targets;
    if (!control->multiSelection.empty())
    {
        targets.assign(control->multiSelection.begin(),
                       control->multiSelection.end());
    }
    else
    {
        targets.push_back(control->selection);
    }
    for (size_t i = 0; i < targets.size(); ++i)
    {
        ParticleSystem::Emitter* e = targets[i];
        if (e == NULL) continue;
        const std::vector<ParticleSystem::Emitter*>& list
            = control->system->getEmitters();
        if (std::find(list.begin(), list.end(), e) == list.end())
            continue;   // cascade-deleted by an earlier parent
        control->system->deleteEmitter(e);
    }

    // Tree rebuild + state reset. OnParticleSystemChange clears
    // multiSelection / selection / selectionAnchor and re-auto-selects
    // the first root (if any remain), so we don't need to touch them
    // ourselves.
    OnParticleSystemChange(control, control->system);
    InvalidateBracketLayout(control);
    control->hoveredGroupId = 0;
    NotifyParent(control, ELN_LISTCHANGED);
    NotifyParent(control, ELN_SELCHANGED);
}

void EmitterList_DuplicateEmitter(HWND hWnd, float indexDelta)
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

    // Shift the atlas index track before NotifyParent fires the undo snapshot,
    // so the increment and the duplicate land in the same undo step.
    if (indexDelta != 0.0f)
        ShiftIndexTrack(pEmitter, indexDelta);

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

size_t EmitterList_GetMultiSelectionSize(HWND hWnd)
{
    EmitterListControl* control
        = (EmitterListControl*)(LONG_PTR)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    if (control != NULL)
    {
        return control->multiSelection.size();
    }
    return 0;
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
        wstring name = FormatEmitterDisplayName(&control->system->getEmitter(control->selection->index));

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
