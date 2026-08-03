// UIA inspector — emits the Win32 UI Automation subtree rooted at a
// given HWND as JSON to stdout. Used by Playwright a11y specs.
//
// CLI: uia_inspector.exe --hwnd 0xNNNN --capture <id> [--depth N]
//   --hwnd     target window handle (hex)
//   --capture  surface identifier (informational; logged to stderr)
//   --depth    max tree depth (default 8)
//
// Exit codes: 0 success; 1 bad args; 2 UIA init failed; 3 HWND invalid.
//
// Emitted properties (per the normalizer's allowlist):
//   Name, ControlType, AutomationId, ClassName,
//   IsKeyboardFocusable, IsEnabled, IsOffscreen, children.
//
// ClassName is what the normalizer keys off for wrapper stripping
// (Chrome_WidgetWin_1, BrowserRootView, etc. per probe findings).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>

// Tiny ComPtr replacement — we don't want to depend on ATL (which isn't
// always installed with the C++ workload). Just RAII over IUnknown*.
template <typename T>
class ComPtr {
public:
    ComPtr() : p_(nullptr) {}
    ~ComPtr() { if (p_) p_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* operator->() const { return p_; }
    T** operator&() { return &p_; }
    operator T*() const { return p_; }
    T* get() const { return p_; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* release() { T* t = p_; p_ = nullptr; return t; }
    void attach(T* t) { if (p_) p_->Release(); p_ = t; }
private:
    T* p_;
};

// Tiny BSTR holder.
class BStr {
public:
    BStr() : b_(nullptr) {}
    ~BStr() { if (b_) SysFreeString(b_); }
    BStr(const BStr&) = delete;
    BStr& operator=(const BStr&) = delete;
    BSTR* operator&() { return &b_; }
    operator BSTR() const { return b_; }
    BSTR get() const { return b_; }
private:
    BSTR b_;
};

static std::string EscapeJson(const std::wstring& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (wchar_t wc : s) {
        if (wc < 0x80) {
            char c = static_cast<char>(wc);
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8]; sprintf_s(buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        } else {
            char buf[8];
            int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, buf, sizeof(buf), nullptr, nullptr);
            if (n > 0) out.append(buf, n);
        }
    }
    return out;
}

static std::string BstrToUtf8(BSTR b) {
    if (!b) return {};
    return EscapeJson(std::wstring(b, SysStringLen(b)));
}

static const wchar_t* ControlTypeName(CONTROLTYPEID id) {
    switch (id) {
        case UIA_ButtonControlTypeId: return L"Button";
        case UIA_CheckBoxControlTypeId: return L"CheckBox";
        case UIA_ComboBoxControlTypeId: return L"ComboBox";
        case UIA_EditControlTypeId: return L"Edit";
        case UIA_HyperlinkControlTypeId: return L"Hyperlink";
        case UIA_ImageControlTypeId: return L"Image";
        case UIA_ListItemControlTypeId: return L"ListItem";
        case UIA_ListControlTypeId: return L"List";
        case UIA_MenuControlTypeId: return L"Menu";
        case UIA_MenuBarControlTypeId: return L"MenuBar";
        case UIA_MenuItemControlTypeId: return L"MenuItem";
        case UIA_ProgressBarControlTypeId: return L"ProgressBar";
        case UIA_RadioButtonControlTypeId: return L"RadioButton";
        case UIA_ScrollBarControlTypeId: return L"ScrollBar";
        case UIA_SliderControlTypeId: return L"Slider";
        case UIA_SpinnerControlTypeId: return L"Spinner";
        case UIA_StatusBarControlTypeId: return L"StatusBar";
        case UIA_TabControlTypeId: return L"Tab";
        case UIA_TabItemControlTypeId: return L"TabItem";
        case UIA_TextControlTypeId: return L"Text";
        case UIA_ToolBarControlTypeId: return L"ToolBar";
        case UIA_ToolTipControlTypeId: return L"ToolTip";
        case UIA_TreeControlTypeId: return L"Tree";
        case UIA_TreeItemControlTypeId: return L"TreeItem";
        case UIA_CustomControlTypeId: return L"Custom";
        case UIA_GroupControlTypeId: return L"Group";
        case UIA_ThumbControlTypeId: return L"Thumb";
        case UIA_DataGridControlTypeId: return L"DataGrid";
        case UIA_DataItemControlTypeId: return L"DataItem";
        case UIA_DocumentControlTypeId: return L"Document";
        case UIA_SplitButtonControlTypeId: return L"SplitButton";
        case UIA_WindowControlTypeId: return L"Window";
        case UIA_PaneControlTypeId: return L"Pane";
        case UIA_HeaderControlTypeId: return L"Header";
        case UIA_HeaderItemControlTypeId: return L"HeaderItem";
        case UIA_TableControlTypeId: return L"Table";
        case UIA_TitleBarControlTypeId: return L"TitleBar";
        case UIA_SeparatorControlTypeId: return L"Separator";
        default: return L"Unknown";
    }
}

static void EmitNode(IUIAutomationElement* elem,
                     IUIAutomationTreeWalker* walker,
                     int depth, int maxDepth,
                     std::ostringstream& out,
                     const char* indent) {
    if (!elem) { out << "null"; return; }

    out << "{\n";

    BStr name;
    elem->get_CurrentName(&name);
    out << indent << "  \"Name\": \"" << BstrToUtf8(name) << "\",\n";

    CONTROLTYPEID ctid = 0;
    elem->get_CurrentControlType(&ctid);
    out << indent << "  \"ControlType\": \""
        << EscapeJson(ControlTypeName(ctid)) << "\",\n";

    BStr autoId;
    elem->get_CurrentAutomationId(&autoId);
    out << indent << "  \"AutomationId\": \"" << BstrToUtf8(autoId) << "\",\n";

    BStr className;
    elem->get_CurrentClassName(&className);
    out << indent << "  \"ClassName\": \"" << BstrToUtf8(className) << "\",\n";

    BOOL focusable = FALSE;
    elem->get_CurrentIsKeyboardFocusable(&focusable);
    out << indent << "  \"IsKeyboardFocusable\": "
        << (focusable ? "true" : "false") << ",\n";

    BOOL enabled = FALSE;
    elem->get_CurrentIsEnabled(&enabled);
    out << indent << "  \"IsEnabled\": "
        << (enabled ? "true" : "false") << ",\n";

    BOOL offscreen = FALSE;
    elem->get_CurrentIsOffscreen(&offscreen);
    out << indent << "  \"IsOffscreen\": "
        << (offscreen ? "true" : "false") << ",\n";

    out << indent << "  \"children\": [";

    if (depth >= maxDepth) {
        out << "]\n" << indent << "}";
        return;
    }

    ComPtr<IUIAutomationElement> child;
    walker->GetFirstChildElement(elem, &child);
    bool first = true;
    while (child) {
        if (!first) out << ",";
        first = false;
        out << "\n" << indent << "    ";
        std::string deeper(indent);
        deeper += "    ";
        EmitNode(child, walker, depth + 1, maxDepth, out, deeper.c_str());
        ComPtr<IUIAutomationElement> next;
        walker->GetNextSiblingElement(child, &next);
        // Move next -> child for the loop iteration.
        child.reset();
        child.attach(next.release());
    }
    if (!first) out << "\n" << indent << "  ";
    out << "]\n" << indent << "}";
}

int wmain(int argc, wchar_t* argv[]) {
    HWND hwnd = nullptr;
    std::wstring capture;
    int maxDepth = 8;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--hwnd") == 0 && i + 1 < argc) {
            hwnd = reinterpret_cast<HWND>(
                static_cast<intptr_t>(wcstoull(argv[++i], nullptr, 16)));
        } else if (wcscmp(argv[i], L"--capture") == 0 && i + 1 < argc) {
            capture = argv[++i];
        } else if (wcscmp(argv[i], L"--depth") == 0 && i + 1 < argc) {
            maxDepth = static_cast<int>(wcstol(argv[++i], nullptr, 10));
        } else if (wcscmp(argv[i], L"--help") == 0) {
            wprintf(L"uia_inspector --hwnd 0xNNNN --capture <id> [--depth N]\n");
            return 0;
        }
    }

    if (!hwnd) { fprintf(stderr, "missing --hwnd\n"); return 1; }
    if (!IsWindow(hwnd)) { fprintf(stderr, "invalid HWND\n"); return 3; }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { fprintf(stderr, "CoInitializeEx failed\n"); return 2; }

    ComPtr<IUIAutomation> uia;
    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    if (FAILED(hr)) { fprintf(stderr, "CUIAutomation create failed\n"); return 2; }

    ComPtr<IUIAutomationTreeWalker> walker;
    hr = uia->get_ControlViewWalker(&walker);
    if (FAILED(hr) || !walker) {
        fprintf(stderr, "get_ControlViewWalker failed\n"); return 2;
    }

    ComPtr<IUIAutomationElement> root;
    hr = uia->ElementFromHandle(hwnd, &root);
    if (FAILED(hr) || !root) { fprintf(stderr, "ElementFromHandle failed\n"); return 3; }

    // Warmup: Chromium/WebView2 uses lazy Blink accessibility
    // initialization: the inner React DOM UIA tree is not built until a
    // UIA client has triggered cross-process activation of the Blink
    // accessibility subsystem. GetFocusedElement() forces a cross-process
    // roundtrip that wakes up the Blink a11y provider. Without this, the
    // BrowserView subtree returns empty children even though the React UI
    // is fully rendered and interactive. After the warmup we sleep 300 ms
    // to give the renderer time to build and expose its subtree.
    {
        ComPtr<IUIAutomationElement> focused;
        uia->GetFocusedElement(&focused);
        // Also try ElementFromPoint at the center of the target window —
        // belt-and-suspenders in case focus is elsewhere.
        RECT wr;
        if (GetWindowRect(hwnd, &wr)) {
            POINT center = {
                (wr.left + wr.right) / 2,
                (wr.top + wr.bottom) / 2
            };
            ComPtr<IUIAutomationElement> atPoint;
            uia->ElementFromPoint(center, &atPoint);
        }
        Sleep(300);
    }

    fprintf(stderr, "[A11Y-CAPTURE] surface=%ls hwnd=0x%llx\n",
            capture.c_str(),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));

    std::ostringstream out;
    EmitNode(root, walker, 0, maxDepth, out, "");

    fputs(out.str().c_str(), stdout);
    fputc('\n', stdout);

    CoUninitialize();
    return 0;
}
