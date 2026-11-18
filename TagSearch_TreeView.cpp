#include "TagSearchCore.h"

#include <commctrl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>

namespace tagsearch::treeview {
namespace {

using Microsoft::WRL::ComPtr;
constexpr wchar_t kWindowClass[] = L"TagSearchTreeView";
constexpr wchar_t kAppId[] = L"MyCustom.HookTool.X";
constexpr int kOpenTag = 1;
constexpr int kHideTag = 2;

struct WindowState {
    Folders folders;
    Tags tags;
    HWND tree = nullptr;
    HFONT font = nullptr;

    ~WindowState() {
        if (font) {
            DeleteObject(font);
        }
    }
};

void RegisterAppShortcut() {
    std::array<wchar_t, MAX_PATH> exePath{};
    const DWORD length = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    if (length == 0 || length == exePath.size()) {
        return;
    }

    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
        return;
    }
    const auto exe = std::filesystem::path(exePath.data());
    auto shortcut = std::filesystem::path(programs) / exe.filename();
    CoTaskMemFree(programs);
    shortcut.replace_extension(L".lnk");
    if (PathFileExistsW(shortcut.c_str())) {
        return;
    }

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.GetAddressOf()))) ||
        FAILED(link->SetPath(exe.c_str()))) {
        return;
    }

    const auto config = exe.parent_path() / L"desktop.ini";
    std::array<wchar_t, MAX_PATH> icon{};
    if (GetPrivateProfileStringW(L".ShellClassInfo", L"IconResource", L"",
                                 icon.data(), static_cast<DWORD>(icon.size()), config.c_str())) {
        const int index = PathParseIconLocationW(icon.data());
        if (icon[0]) {
            link->SetIconLocation(icon.data(), index);
        }
    }

    ComPtr<IPropertyStore> properties;
    PROPVARIANT appId{};
    if (SUCCEEDED(link.As(&properties)) && SUCCEEDED(InitPropVariantFromString(kAppId, &appId))) {
        if (SUCCEEDED(properties->SetValue(PKEY_AppUserModel_ID, appId))) {
            properties->Commit();
        }
    }
    PropVariantClear(&appId);

    ComPtr<IPersistFile> file;
    if (SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(shortcut.c_str(), TRUE))) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

void LayoutTree(HWND window, WindowState& state) {
    RECT client{};
    GetClientRect(window, &client);
    const auto dpi = GetDpiForWindow(window);
    const int padding = MulDiv(10, dpi, USER_DEFAULT_SCREEN_DPI);
    MoveWindow(state.tree, padding, padding,
               std::max(0L, client.right - 2 * padding),
               std::max(0L, client.bottom - 2 * padding), TRUE);
}

void UpdateFont(HWND window, WindowState& state) {
    const auto dpi = GetDpiForWindow(window);
    const HFONT font = CreateFontW(-MulDiv(18, dpi, USER_DEFAULT_SCREEN_DPI), 0, 0, 0,
                                   FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH, L"Segoe UI");
    if (font) {
        SendMessageW(state.tree, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (state.font) DeleteObject(state.font);
        state.font = font;
    }
}

HTREEITEM ItemAt(HWND tree, POINT screenPoint) {
    ScreenToClient(tree, &screenPoint);
    TVHITTESTINFO hit{};
    hit.pt = screenPoint;
    TreeView_HitTest(tree, &hit);
    return (hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT)) ? hit.hItem : nullptr;
}

void SearchItem(HWND window, const WindowState& state, HTREEITEM item) {
    if (!item) {
        return;
    }
    TVITEMW value{};
    value.mask = TVIF_PARAM;
    value.hItem = item;
    if (TreeView_GetItem(state.tree, &value) &&
        !OpenTagSearch(window, {state.tags[static_cast<std::size_t>(value.lParam)]}, state.folders)) {
        MessageBoxW(window, L"无法在文件资源管理器中打开标签搜索。", L"TagSearch", MB_OK | MB_ICONWARNING);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!state) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_CREATE: {
        state->tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
            0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        if (!state->tree) return -1;
        UpdateFont(window, *state);
        LayoutTree(window, *state);

        TVINSERTSTRUCTW insert{};
        insert.hParent = TVI_ROOT;
        // Inserting in reverse at the front avoids repeatedly walking the sibling list.
        insert.hInsertAfter = TVI_FIRST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        for (std::size_t count = state->tags.size(); count > 0; --count) {
            const std::size_t i = count - 1;
            insert.item.pszText = state->tags[i].data();
            insert.item.lParam = static_cast<LPARAM>(i);
            TreeView_InsertItem(state->tree, &insert);
        }
        return 0;
    }
    case WM_SIZE:
        LayoutTree(window, *state);
        return 0;
    case WM_DPICHANGED: {
        const auto& position = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, position.left, position.top,
                     position.right - position.left, position.bottom - position.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateFont(window, *state);
        LayoutTree(window, *state);
        return 0;
    }
    case WM_NOTIFY: {
        const auto* notice = reinterpret_cast<const NMHDR*>(lParam);
        if (notice->hwndFrom != state->tree) break;
        if (notice->code == NM_CLICK) {
            POINT point{};
            if (GetCursorPos(&point)) SearchItem(window, *state, ItemAt(state->tree, point));
        } else if (notice->code == TVN_KEYDOWN &&
                   reinterpret_cast<const NMTVKEYDOWN*>(lParam)->wVKey == VK_RETURN) {
            SearchItem(window, *state, TreeView_GetSelection(state->tree));
        }
        return 0;
    }
    case WM_CONTEXTMENU: {
        if (reinterpret_cast<HWND>(wParam) != state->tree) break;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const bool keyboard = point.x == -1 && point.y == -1;
        const HTREEITEM item = keyboard ? TreeView_GetSelection(state->tree) : ItemAt(state->tree, point);
        if (!item) return 0;
        TreeView_SelectItem(state->tree, item);
        if (keyboard) {
            RECT bounds{};
            if (!TreeView_GetItemRect(state->tree, item, &bounds, TRUE)) return 0;
            point = {bounds.left, bounds.bottom};
            ClientToScreen(state->tree, &point);
        }
        const HMENU menu = CreatePopupMenu();
        if (!menu) return 0;
        AppendMenuW(menu, MF_STRING, kOpenTag, L"打开搜索");
        AppendMenuW(menu, MF_STRING, kHideTag, L"隐藏标签");
        const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                            point.x, point.y, 0, window, nullptr);
        DestroyMenu(menu);
        if (command == kOpenTag) SearchItem(window, *state, item);
        if (command == kHideTag) TreeView_DeleteItem(state->tree, item);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int Run(HINSTANCE instance, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 1;

    RegisterAppShortcut();
    WindowState state;
    state.folders = ResolveSearchFolders(GetCommandLineW());
    state.tags = ScanTags(state.folders);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass)) return 1;

    POINT origin{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor)) return 1;
    const auto& work = monitor.rcWork;
    const int width = std::min(MulDiv(350, GetDpiForSystem(), USER_DEFAULT_SCREEN_DPI),
                               static_cast<int>(work.right - work.left));
    const HWND window = CreateWindowExW(0, kWindowClass, L"TagSearch · TreeView",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        work.right - width, work.top, width, work.bottom - work.top,
        nullptr, nullptr, instance, &state);
    if (!window) return 1;

    ShowWindow(window, showCommand);
    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}

} // namespace tagsearch::treeview

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    const int result = tagsearch::treeview::Run(instance, showCommand);
    CoUninitialize();
    return result;
}
