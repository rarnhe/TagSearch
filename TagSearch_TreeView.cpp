#include "TagSearch_Core.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <future>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

// Each executable supplies this externally linked entry point for the CRT.
int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE previousInstance,
    _In_ LPWSTR commandLine,
    _In_ int showCommand);

namespace tagsearch::treeview {
namespace {

using Microsoft::WRL::ComPtr;
constexpr wchar_t kWindowClass[] = L"TagSearchTreeView";
constexpr wchar_t kMutexName[] = L"Local\\TagSearch.TreeView.391B8D68-6A89-4B0A-8C47-DFFA52EB7FA3";
constexpr wchar_t kDefaultTitle[] = L"默认目录";
constexpr wchar_t kScanningSuffix[] = L" — 正在读取标签…";
constexpr wchar_t kScanFailedSuffix[] = L" — 无法读取标签";
constexpr UINT kActivate = WM_APP + 1;
constexpr UINT kScanComplete = WM_APP + 2;
constexpr UINT kFoldersResolved = WM_APP + 3;
constexpr wchar_t kAppId[] = L"MyCustom.HookTool.X";
constexpr int kOpenTag = 1;
constexpr int kHideTag = 2;

struct StartupScan {
    Folders folders;
    Tags tags;
    bool succeeded = false;
};

struct WindowState {
    Folders folders;
    Tags tags;
    HWND tree = nullptr;
    HFONT font = nullptr;
    RECT dock{};
    std::wstring title = kDefaultTitle;
    std::future<std::wstring> folderTitle;
    StartupScan startupScan;
    std::jthread scanner;

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

    if (const auto icon = ReadDesktopIniIcon(exe.parent_path())) {
        link->SetIconLocation(icon->first.c_str(), icon->second);
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

HTREEITEM ItemAt(HWND tree, POINT point) {
    TVHITTESTINFO hit{};
    hit.pt = point;
    TreeView_HitTest(tree, &hit);
    return (hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT)) ? hit.hItem : nullptr;
}

HTREEITEM ItemAtScreenPoint(HWND tree, POINT point) {
    return ScreenToClient(tree, &point) ? ItemAt(tree, point) : nullptr;
}

LRESULT CALLBACK TreeMouseProc(HWND tree, UINT message, WPARAM wParam, LPARAM lParam,
                               UINT_PTR subclassId, DWORD_PTR) {
    switch (message) {
    case WM_MOUSEACTIVATE: {
        POINT point{};
        if (LOWORD(lParam) == HTCLIENT && GetCursorPos(&point) && !ItemAtScreenPoint(tree, point)) {
            return MA_NOACTIVATE;
        }
        break;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        // The native control can move its caret to the first row when a blank click gives it focus.
        if (!ItemAt(tree, {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) return 0;
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(tree, TreeMouseProc, subclassId);
        break;
    }
    return DefSubclassProc(tree, message, wParam, lParam);
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

void PopulateTree(WindowState& state) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = TVI_ROOT;
    // Inserting in reverse at the front avoids repeatedly walking the sibling list.
    insert.hInsertAfter = TVI_FIRST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    SendMessageW(state.tree, WM_SETREDRAW, FALSE, 0);
    for (std::size_t count = state.tags.size(); count > 0; --count) {
        const std::size_t i = count - 1;
        insert.item.pszText = state.tags[i].data();
        insert.item.lParam = static_cast<LPARAM>(i);
        TreeView_InsertItem(state.tree, &insert);
    }
    SendMessageW(state.tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.tree, nullptr, TRUE);
}

RECT DockBounds(UINT dpi) {
    const POINT origin{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        return {};
    }

    const auto& work = monitor.rcWork;
    const int width = std::min(MulDiv(350, dpi, USER_DEFAULT_SCREEN_DPI),
                               static_cast<int>(work.right - work.left));
    return {work.right - width, work.top, work.right, work.bottom};
}

void UpdateDock(HWND window, WindowState& state, UINT dpi) {
    const auto dock = DockBounds(dpi);
    if (IsRectEmpty(&dock)) return;

    state.dock = dock;
    if (!IsIconic(window)) {
        SetWindowPos(window, nullptr, dock.left, dock.top, dock.right - dock.left,
                     dock.bottom - dock.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void ActivateWindow(HWND window, WindowState& state) {
    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
        UpdateDock(window, state, GetDpiForWindow(window));
    }
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (SetForegroundWindow(window)) {
        SetFocus(TreeView_GetSelection(state.tree) ? state.tree : window);
    }
}

std::wstring FolderTitle(const Folders& folders, std::size_t defaultCount) {
    std::wstring title;
    for (std::size_t i = defaultCount; i < folders.size(); ++i) {
        const auto& folder = folders[i];
        const auto name = folder.filename();
        if (!title.empty()) title += L'·';
        title += name.empty() ? folder.native() : name.native();
    }
    if (title.empty()) return kDefaultTitle;
    return title;
}

void UpdateTitle(HWND window, const WindowState& state, std::wstring_view suffix = {}) {
    auto text = state.title;
    text.append(suffix);
    SetWindowTextW(window, text.c_str());
}

void ScanOnStartup(HWND window, StartupScan& scan, std::promise<std::wstring>& title,
                   std::stop_token stop) {
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        try {
            std::size_t defaultCount = 0;
            scan.folders = ResolveSearchFolders(GetCommandLineW(), &defaultCount);
            if (!stop.stop_requested()) {
                title.set_value(FolderTitle(scan.folders, defaultCount));
                PostMessageW(window, kFoldersResolved, 0, 0);
                scan.tags = ScanTagsWithDesktopIniCache(scan.folders, defaultCount, stop);
                scan.succeeded = !stop.stop_requested();
                if (scan.succeeded) RegisterAppShortcut();
            }
        } catch (...) {
            scan.succeeded = false;
        }
        CoUninitialize();
    }
    if (!stop.stop_requested()) PostMessageW(window, kScanComplete, 0, 0);
}

void ApplyAccentBorder(HWND window) {
    const COLORREF color = WindowAccentColor();
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &color, sizeof(color));
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
        if (!state->tree || !SetWindowSubclass(state->tree, TreeMouseProc, 0, 0)) return -1;
        UpdateFont(window, *state);
        LayoutTree(window, *state);
        ApplyAccentBorder(window);
        return 0;
    }
    case kFoldersResolved:
        if (state->folderTitle.valid()) {
            state->title = state->folderTitle.get();
            UpdateTitle(window, *state, kScanningSuffix);
        }
        return 0;
    case kScanComplete:
        if (state->scanner.joinable()) {
            state->scanner.join();
            if (state->startupScan.succeeded) {
                state->folders = std::move(state->startupScan.folders);
                state->tags = std::move(state->startupScan.tags);
                PopulateTree(*state);
                UpdateTitle(window, *state);
            } else {
                UpdateTitle(window, *state, kScanFailedSuffix);
            }
        }
        return 0;
    case kActivate:
        ActivateWindow(window, *state);
        return 0;
    case WM_MOUSEACTIVATE:
        // Keep normal click-to-focus, including keyboard navigation after a click.
        return HIWORD(lParam) == WM_MOUSEMOVE ? MA_NOACTIVATE : MA_ACTIVATE;
    case WM_NCHITTEST: {
        const auto hit = DefWindowProcW(window, message, wParam, lParam);
        return hit == HTCAPTION ? HTCLIENT : hit;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MOVE) return 0;
        break;
    case WM_WINDOWPOSCHANGING: {
        auto& position = *reinterpret_cast<WINDOWPOS*>(lParam);
        if ((position.flags & SWP_NOMOVE) == 0 && !IsIconic(window)) {
            position.x = state->dock.left;
            position.y = state->dock.top;
        }
        break;
    }
    case WM_SIZE:
        LayoutTree(window, *state);
        return 0;
    case WM_DPICHANGED:
        UpdateDock(window, *state, HIWORD(wParam));
        UpdateFont(window, *state);
        LayoutTree(window, *state);
        return 0;
    case WM_DISPLAYCHANGE:
        UpdateDock(window, *state, GetDpiForWindow(window));
        break;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA) UpdateDock(window, *state, GetDpiForWindow(window));
        [[fallthrough]];
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        ApplyAccentBorder(window);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_TAB || wParam == VK_UP || wParam == VK_DOWN || wParam == VK_HOME ||
            wParam == VK_END || wParam == VK_PRIOR || wParam == VK_NEXT) {
            const auto previous = TreeView_GetSelection(state->tree);
            SetFocus(state->tree);
            const bool firstArrow = !previous && (wParam == VK_UP || wParam == VK_DOWN);
            // If focusing an empty caret already selected the first row, do not advance again.
            if (wParam != VK_TAB && (!firstArrow || !TreeView_GetSelection(state->tree))) {
                SendMessageW(state->tree, message, wParam, lParam);
            }
            return 0;
        }
        break;
    case WM_NOTIFY: {
        const auto* notice = reinterpret_cast<const NMHDR*>(lParam);
        if (notice->hwndFrom != state->tree) break;
        if (notice->code == NM_CLICK) {
            POINT point{};
            if (GetCursorPos(&point)) SearchItem(window, *state, ItemAtScreenPoint(state->tree, point));
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
        const HTREEITEM item = keyboard ? TreeView_GetSelection(state->tree) : ItemAtScreenPoint(state->tree, point);
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
        state->scanner.request_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int Run(HINSTANCE instance, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 1;

    WindowState state;

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass)) return 1;

    state.dock = DockBounds(GetDpiForSystem());
    if (IsRectEmpty(&state.dock)) return 1;
    const auto& dock = state.dock;
    // Keep the owner hidden and create the panel as its owned popup.
    constexpr DWORD toolStyles = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    const HWND owner = CreateWindowExW(toolStyles, L"STATIC", nullptr, WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!owner) return 1;

    const std::wstring startingTitle = state.title + kScanningSuffix;
    const HWND window = CreateWindowExW(toolStyles, kWindowClass, startingTitle.c_str(),
        WS_POPUP | WS_CAPTION | WS_MINIMIZEBOX,
        dock.left, dock.top, dock.right - dock.left, dock.bottom - dock.top,
        owner, nullptr, instance, &state);
    if (!window) {
        DestroyWindow(owner);
        return 1;
    }

    ShowWindow(window, showCommand);
    ActivateWindow(window, state);
    try {
        std::promise<std::wstring> folderTitle;
        state.folderTitle = folderTitle.get_future();
        state.scanner = std::jthread(
            [window, &state, title = std::move(folderTitle)](std::stop_token stop) mutable {
                ScanOnStartup(window, state.startupScan, title, stop);
            });
    } catch (const std::system_error&) {
        UpdateTitle(window, state, kScanFailedSuffix);
    }

    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(owner);
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}

int RunSingleInstance(HINSTANCE instance, int showCommand) {
    HANDLE rawMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const DWORD mutexStatus = GetLastError();
    const std::unique_ptr<void, decltype(&CloseHandle)> mutex(rawMutex, &CloseHandle);
    if (!mutex) return 1;

    if (mutexStatus == ERROR_ALREADY_EXISTS) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        for (;;) {
            const DWORD wait = WaitForSingleObject(mutex.get(), 0);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) break;
            if (wait != WAIT_TIMEOUT || GetTickCount64() >= deadline) return 1;

            if (const HWND existing = FindWindowW(kWindowClass, nullptr)) {
                DWORD processId = 0;
                if (GetWindowThreadProcessId(existing, &processId)) {
                    // A shell-launched copy can transfer its foreground privilege to the owner.
                    AllowSetForegroundWindow(processId);
                    if (PostMessageW(existing, kActivate, 0, 0)) return 0;
                }
            }
            Sleep(25);
        }
    }

    int result = 1;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        result = Run(instance, showCommand);
        CoUninitialize();
    }
    ReleaseMutex(mutex.get());
    return result;
}

} // namespace
} // namespace tagsearch::treeview

_Use_decl_annotations_
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    return tagsearch::treeview::RunSingleInstance(instance, showCommand);
}
