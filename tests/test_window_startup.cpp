#include "TagSearch_Core.h"

#include <commctrl.h>

#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

namespace fs = std::filesystem;
constexpr DWORD launchTimeout = 8000;

void Expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool WaitUntil(Predicate predicate) {
    const auto deadline = GetTickCount64() + launchTimeout;
    do {
        if (predicate()) return true;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return false;
}

struct TestCopy {
    fs::path directory = fs::temp_directory_path() /
        (L"TagSearch startup test " + std::to_wstring(GetCurrentProcessId()) + L" " +
         std::to_wstring(GetTickCount64()));
    fs::path executable = directory / (directory.filename().wstring() + L".exe");

    explicit TestCopy(const fs::path& source) {
        fs::create_directory(directory);
        try {
            fs::copy_file(source, executable);
        } catch (...) {
            std::error_code error;
            fs::remove_all(directory, error);
            throw;
        }
    }

    ~TestCopy() {
        std::error_code error;
        PWSTR programs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) {
            auto shortcut = fs::path(programs) / executable.filename();
            shortcut.replace_extension(L".lnk");
            fs::remove(shortcut, error);
        }
        CoTaskMemFree(programs);
        fs::remove_all(directory, error);
    }
};

struct Process {
    PROCESS_INFORMATION info{};

    explicit Process(const fs::path& executable, const tagsearch::Folders& folders = {}) {
        STARTUPINFOW startup{sizeof(startup)};
        auto commandLine = L"\"" + executable.wstring() + L"\"";
        for (const auto& folder : folders) {
            commandLine += L" \"" + folder.wstring() + L"\"";
        }
        Expect(CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                             nullptr, nullptr, &startup, &info) != FALSE,
               "CreateProcessW should launch TagSearch");
        CloseHandle(info.hThread);
    }

    ~Process() {
        if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(info.hProcess, 1);
            WaitForSingleObject(info.hProcess, 1000);
        }
        CloseHandle(info.hProcess);
    }

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    void ExpectSuccessfulExit(const char* message) const {
        DWORD code = 1;
        Expect(WaitForSingleObject(info.hProcess, launchTimeout) == WAIT_OBJECT_0 &&
               GetExitCodeProcess(info.hProcess, &code) && code == 0, message);
    }
};

HWND WaitForPanel(const Process& process, const wchar_t* className = L"TagSearchTreeView", bool visible = true) {
    HWND window = nullptr;
    Expect(WaitUntil([&] {
        window = FindWindowW(className, nullptr);
        DWORD processId = 0;
        return window && GetWindowThreadProcessId(window, &processId) &&
               processId == process.info.dwProcessId && (IsWindowVisible(window) != FALSE) == visible;
    }), "the launch should create its own window with the expected visibility");
    return window;
}

void WaitForTitle(HWND window, std::wstring_view expected) {
    Expect(WaitUntil([&] {
        std::array<wchar_t, 128> title{};
        GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        return std::wstring_view(title.data()) == expected;
    }), "the scan should finish with the expected folder title");
}

void ClosePanel(HWND window, const Process& process) {
    Expect(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE, "the existing panel should close normally");
    process.ExpectSuccessfulExit("closing the panel should release the application instance");
}

void CheckStartup(const fs::path& executable) {
    // Use a distinct executable name so the normal shortcut does not get modified.
    const TestCopy copy(executable);
    const Process first(copy.executable);
    const HWND window = WaitForPanel(first);

    const auto styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    Expect((styles & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_APPWINDOW)) ==
           (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE), "the panel should have the task switcher exclusion styles");
    const HWND owner = GetWindow(window, GW_OWNER);
    DWORD ownerPid = 0;
    Expect(owner && GetWindowThreadProcessId(owner, &ownerPid) &&
           ownerPid == first.info.dwProcessId && !IsWindowVisible(owner),
           "the panel should be owned by an invisible window in the same process");

    Expect(ShowWindowAsync(window, SW_HIDE) && WaitUntil([&] { return !IsWindowVisible(window); }),
           "the test should hide the first panel before reopening");
    const Process second(copy.executable);
    second.ExpectSuccessfulExit("the second launch should forward activation and exit");
    Expect(WaitUntil([&] { return IsWindowVisible(window); }),
           "the first panel should be shown again without changing its HWND");

    WaitForTitle(window, L"默认目录");
    ClosePanel(window, first);
}

void CheckFixedWindowAndTitle(const fs::path& executable) {
    const TestCopy copy(executable);
    const auto firstFolder = copy.directory / L"文件夹 1";
    const auto secondFolder = copy.directory / L"文件夹2";
    fs::create_directory(firstFolder);
    fs::create_directory(secondFolder);

    // Explicit default paths and duplicate arguments must also stay out of the title.
    auto folders = tagsearch::ResolveSearchFolders(L"TagSearch_TreeView.exe");
    folders.insert(folders.begin(), firstFolder);
    folders.push_back(secondFolder);
    folders.push_back(firstFolder);
    const Process process(copy.executable, folders);
    const HWND window = WaitForPanel(process);
    WaitForTitle(window, L"文件夹 1·文件夹2");

    constexpr DWORD requestedStyle = WS_POPUP | WS_CAPTION | WS_MINIMIZEBOX;
    constexpr DWORD applicationStyleBits = requestedStyle | WS_SYSMENU | WS_THICKFRAME |
                                           WS_MAXIMIZEBOX | WS_CHILD;
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    Expect((style & applicationStyleBits) == requestedStyle,
           "the panel should use the exact requested application style");

    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    const POINT origin{};
    Expect(GetWindowRect(window, &bounds) &&
           GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor),
           "the test should read the panel and primary work area rectangles");
    Expect(bounds.right == monitor.rcWork.right && bounds.top == monitor.rcWork.top,
           "the panel should be docked to the primary work area's top-right corner");

    POINT clientOrigin{};
    Expect(ClientToScreen(window, &clientOrigin) && clientOrigin.y > bounds.top,
           "the panel should have a visible non-client caption area");
    const LPARAM captionPoint = MAKELPARAM(bounds.left + (bounds.right - bounds.left) / 2,
                                          bounds.top + (clientOrigin.y - bounds.top) / 2);
    DWORD_PTR result = 0;
    Expect(SendMessageTimeoutW(window, WM_NCHITTEST, 0, captionPoint, SMTO_ABORTIFHUNG,
                               1000, &result) && result == HTCLIENT,
           "the caption should not be hit-tested as a drag target");
    Expect(SendMessageTimeoutW(window, WM_SYSCOMMAND, SC_MOVE, 0, SMTO_ABORTIFHUNG,
                               1000, &result) && result == 0,
           "the system move command should be handled without entering a move loop");

    Expect(SetWindowPos(window, nullptr, bounds.left - 40, bounds.top + 40, 0, 0,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE),
           "the test should be able to request a move");
    RECT after{};
    Expect(GetWindowRect(window, &after) && EqualRect(&bounds, &after),
           "the docked position should survive a move request");
    ClosePanel(window, process);
}

void CheckTreeBlankClick(const fs::path& executable, const fs::path& fixture) {
    const TestCopy copy(executable);
    fs::copy_file(fixture, copy.directory / L"tagged.jpg");
    const Process process(copy.executable, {copy.directory});
    const HWND panel = WaitForPanel(process);
    WaitForTitle(panel, copy.directory.filename().wstring());
    const HWND tree = FindWindowExW(panel, nullptr, WC_TREEVIEWW, nullptr);
    if (!tree) throw std::runtime_error("the panel should contain a tree view");

    RECT client{};
    const auto count = SendMessageW(tree, TVM_GETCOUNT, 0, 0);
    Expect(GetClientRect(tree, &client) && count > 0 &&
           count < SendMessageW(tree, TVM_GETVISIBLECOUNT, 0, 0),
           "the fixture should provide rows and a blank area below them");

    const auto caret = [&] { return SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0); };
    const auto clickBlank = [&] {
        const auto point = MAKELPARAM(client.right / 2, client.bottom - 2);
        DWORD_PTR result = 0;
        Expect(SendMessageTimeoutW(tree, WM_LBUTTONDOWN, MK_LBUTTON, point, SMTO_ABORTIFHUNG, 1000, &result) &&
               SendMessageTimeoutW(tree, WM_LBUTTONUP, 0, point, SMTO_ABORTIFHUNG, 1000, &result),
               "the tree should handle a click in its blank area");
    };

    Expect(caret() == 0, "initial TreeView display should leave the caret unselected");
    clickBlank();
    Expect(caret() == 0, "a blank click should not select the first row");
    DWORD_PTR result = 0;
    Expect(SendMessageTimeoutW(panel, WM_KEYDOWN, VK_DOWN, 0, SMTO_ABORTIFHUNG, 1000, &result),
           "keyboard navigation should enter the tree");
    const auto selected = caret();
    Expect(selected != 0, "keyboard navigation should still select a row");
    clickBlank();
    Expect(caret() == selected, "a blank click should preserve an existing selection");

    ClosePanel(panel, process);
}

void CheckTreeCache(const fs::path& executable, const fs::path& fixture) {
    const TestCopy copy(executable);
    const auto image = copy.directory / L"tagged.jpg";
    const auto ini = copy.directory / L"desktop.ini";
    fs::copy_file(fixture, image);

    const auto countTags = [&](const Process& process) {
        const HWND panel = WaitForPanel(process);
        WaitForTitle(panel, copy.directory.filename().wstring());
        const HWND tree = FindWindowExW(panel, nullptr, WC_TREEVIEWW, nullptr);
        if (!tree) throw std::runtime_error("the panel should contain a tree view");
        const auto count = SendMessageW(tree, TVM_GETCOUNT, 0, 0);
        ClosePanel(panel, process);
        return count;
    };

    const Process cold(copy.executable, {copy.directory});
    const auto count = countTags(cold);
    Expect(count >= 2, "the cold TreeView launch should show the fixture tags");
    std::array<wchar_t, 256> keywords{};
    GetPrivateProfileStringW(L"{F29F85E0-4FF9-1068-AB91-08002B27B3D9}", L"Prop5", L"",
                             keywords.data(), static_cast<DWORD>(keywords.size()), ini.c_str());
    const std::wstring_view value(keywords.data());
    Expect(value.starts_with(L"31,") && value.find(L"项目 & 资料") != std::wstring_view::npos,
           "TreeView should write the requested directory's Unicode tag cache");

    fs::remove(image);
    const Process warm(copy.executable, {copy.directory});
    Expect(countTags(warm) == count, "TreeView should recover the same rows from desktop.ini on next launch");
}

struct VisibleTags {
    HWND controller;
    bool unexpectedVisibleWindow = false;
    std::vector<HWND> windows;
    std::vector<RECT> bounds;
    tagsearch::Tags titles;
};

BOOL CALLBACK RecordVisibleTags(HWND window, LPARAM parameter) {
    auto& tags = *reinterpret_cast<VisibleTags*>(parameter);
    if (!IsWindowVisible(window)) return TRUE;
    std::array<wchar_t, 128> className{};
    GetClassNameW(window, className.data(), static_cast<int>(className.size()));
    if (GetWindow(window, GW_OWNER) != tags.controller || std::wstring_view(className.data()) != L"TagSearchFloatingTag") {
        tags.unexpectedVisibleWindow = true;
        return TRUE;
    }
    RECT rect{};
    if (GetWindowRect(window, &rect)) {
        std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
        const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        title.resize(static_cast<std::size_t>(length));
        tags.windows.push_back(window);
        tags.bounds.push_back(rect);
        tags.titles.push_back(std::move(title));
    }
    return TRUE;
}

bool Within(const RECT& rect, const RECT& area) {
    return rect.left >= area.left && rect.top >= area.top &&
           rect.right <= area.right && rect.bottom <= area.bottom;
}

void ReadVisibleCaptionTags(DWORD threadId, VisibleTags& tags) {
    tags.unexpectedVisibleWindow = false;
    tags.windows.clear();
    tags.bounds.clear();
    tags.titles.clear();
    EnumThreadWindows(threadId, RecordVisibleTags, reinterpret_cast<LPARAM>(&tags));
}

void CheckCaptionFrame(HWND window, const RECT& bounds) {
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    constexpr DWORD nativeStyle = WS_POPUP | WS_CAPTION;
    constexpr DWORD extraStyles = WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CHILD;
    Expect((style & (nativeStyle | extraStyles)) == nativeStyle,
           "a tag should use only the native popup caption style");
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    constexpr DWORD popupStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    Expect((extended & popupStyle) == popupStyle && !(extended & WS_EX_TRANSPARENT),
           "a tag should have a layered tool window surface and remain clickable");
    RECT client{};
    Expect(GetClientRect(window, &client) && client.right == bounds.right - bounds.left &&
           client.bottom == bounds.bottom - bounds.top,
           "the title should occupy the client area of the caption-only window");
    const auto captionPoint = MAKELPARAM(bounds.left + client.right / 2, bounds.top + client.bottom / 2);
    DWORD_PTR hit = 0;
    Expect(SendMessageTimeoutW(window, WM_NCHITTEST, 0, captionPoint, SMTO_ABORTIFHUNG, 1000, &hit) &&
           hit == HTCAPTION, "Windows should hit-test the tag as a native title bar");
}

void CheckTagBounds(const VisibleTags& tags, const RECT& work) {
    for (std::size_t i = 0; i < tags.bounds.size(); ++i) {
        const auto& rect = tags.bounds[i];
        Expect(!IsRectEmpty(&rect) && Within(rect, work),
               "the tag window must stay inside the primary work area");
        CheckCaptionFrame(tags.windows[i], rect);
    }
}

std::vector<DWORD> PrintCaption(HWND window) {
    RECT client{};
    Expect(GetClientRect(window, &client) && client.right > 0 && client.bottom > 0,
           "the rendered caption should have a client rectangle");
    std::vector<DWORD> pixels(static_cast<std::size_t>(client.right) * client.bottom);
    const HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) throw std::runtime_error("the caption snapshot needs a memory DC");

    BITMAPINFO image{};
    image.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    image.bmiHeader.biWidth = client.right;
    image.bmiHeader.biHeight = -client.bottom;
    image.bmiHeader.biPlanes = 1;
    image.bmiHeader.biBitCount = 32;
    image.bmiHeader.biCompression = BI_RGB;

    void* data = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &image, DIB_RGB_COLORS, &data, nullptr, 0);
    if (!bitmap || !data) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        throw std::runtime_error("the caption snapshot needs a bitmap");
    }
    const auto previous = SelectObject(dc, bitmap);
    const bool printed = PrintWindow(window, dc, PW_CLIENTONLY) != FALSE;
    GdiFlush();
    if (printed) std::memcpy(pixels.data(), data, pixels.size() * sizeof(DWORD));
    if (previous) SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    Expect(printed, "PrintWindow should render the title onto the bitmap");
    return pixels;
}

void CheckCaptionPaint(HWND window) {
    const auto send = [window](UINT message, WPARAM parameter = 0) {
        DWORD_PTR result = 0;
        Expect(SendMessageTimeoutW(window, message, parameter, 0, SMTO_ABORTIFHUNG, 1000, &result),
               "the caption should handle the test input");
    };
    const auto checkColors = [](const std::vector<DWORD>& pixels, DWORD background) {
        Expect(pixels.front() == (0xff000000 | background),
               "the operator background should use the opaque PR #8 color");
        Expect(std::any_of(pixels.begin(), pixels.end(), [](DWORD pixel) { return (pixel >> 24) == 1; }),
               "the center of a glyph should reveal the desktop while retaining hit testing");
        Expect(std::none_of(pixels.begin(), pixels.end(), [](DWORD pixel) { return (pixel >> 24) == 0; }),
               "the title should be clickable through its glyphs");
    };

    constexpr UINT setTagMode = WM_APP + 5;
    constexpr DWORD backgrounds[]{0xbeebc7, 0xf8cadb, 0xd2d4d8};
    for (std::size_t i = 0; i < std::size(backgrounds); ++i) {
        send(setTagMode, i + 1);
        checkColors(PrintCaption(window), backgrounds[i]);
    }

    send(setTagMode, 1);
    const auto bold = PrintCaption(window);
    RECT client{};
    Expect(GetClientRect(window, &client) && client.right > 0, "the caption should have a width");
    LONG left = client.right;
    LONG right = 0;
    for (std::size_t i = 0; i < bold.size(); ++i) {
        if ((bold[i] >> 24) == 255) continue;
        const LONG x = static_cast<LONG>(i % client.right);
        left = std::min(left, x);
        right = std::max(right, x + 1);
    }
    const LONG maxPadding = MulDiv(8, GetDpiForWindow(window), USER_DEFAULT_SCREEN_DPI);
    Expect(left <= maxPadding && client.right - right <= maxPadding,
           "the caption should fit its title with small side margins");

    send(WM_NCLBUTTONDOWN, HTCAPTION);
    const auto pressed = PrintCaption(window);
    checkColors(pressed, backgrounds[0]);
    const auto glyphPixels = [](const std::vector<DWORD>& pixels) {
        return std::count_if(pixels.begin(), pixels.end(), [](DWORD pixel) { return (pixel >> 24) < 255; });
    };
    Expect(glyphPixels(bold) > glyphPixels(pressed),
           "pressing the bold label should switch to the lighter font weight");
    send(WM_CANCELMODE);
    Expect(PrintCaption(window) == bold, "canceling the press should restore the bold title");
}

bool HasIntersection(const std::vector<RECT>& bounds) {
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            RECT intersection{};
            if (IntersectRect(&intersection, &bounds[i], &bounds[j])) return true;
        }
    }
    return false;
}

void CheckCaptionLayout(const fs::path& executable, const fs::path& fixture) {
    const TestCopy copy(executable);
    fs::copy_file(fixture, copy.directory / L"tagged.jpg");
    const Process first(copy.executable, {copy.directory});
    const HWND controller = WaitForPanel(first, L"TagSearchCaptionController", false);
    MONITORINFO monitor{sizeof(monitor)};
    const POINT origin{};
    Expect(GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor),
           "the test should read the primary work area");

    VisibleTags tags{controller};
    const DWORD threadId = GetWindowThreadProcessId(controller, nullptr);
    Expect(WaitUntil([&] {
        ReadVisibleCaptionTags(threadId, tags);
        return std::find(tags.titles.begin(), tags.titles.end(), L"alpha") != tags.titles.end() &&
               std::find(tags.titles.begin(), tags.titles.end(), L"项目 & 资料") != tags.titles.end();
    }), "the background scan should show both labels from the JPEG fixture");
    Expect(!tags.unexpectedVisibleWindow && !IsWindowVisible(controller),
           "only real tag windows should be visible after the scan");
    CheckTagBounds(tags, monitor.rcWork);
    Expect(!HasIntersection(tags.bounds), "two short tags should fit without overlap");
    Expect(std::find(tags.windows.begin(), tags.windows.end(), GetForegroundWindow()) == tags.windows.end(),
           "initial Caption display should leave the foreground with the previous application");

    const auto alpha = std::find(tags.titles.begin(), tags.titles.end(), L"alpha");
    CheckCaptionPaint(tags.windows[static_cast<std::size_t>(alpha - tags.titles.begin())]);

    const auto originalWindows = tags.windows;
    const HWND tag = originalWindows.front();
    Expect(ShowWindowAsync(tag, SW_HIDE) && WaitUntil([&] { return !IsWindowVisible(tag); }),
           "the test should hide a tag before starting Caption again");
    const Process second(copy.executable);
    second.ExpectSuccessfulExit("Caption's duplicate launch should forward activation and exit");
    Expect(WaitUntil([&] { return IsWindowVisible(tag); }),
           "the duplicate launch should restore the existing tag window");
    ReadVisibleCaptionTags(threadId, tags);
    Expect(!tags.unexpectedVisibleWindow &&
           std::is_permutation(originalWindows.begin(), originalWindows.end(), tags.windows.begin(), tags.windows.end()),
           "activation should restore the original tags and keep the controller hidden");
    CheckTagBounds(tags, monitor.rcWork);
    ClosePanel(controller, first);
}

void WriteTagFixture(const fs::path& source, const fs::path& target, const tagsearch::Tags& titles) {
    fs::copy_file(source, target);
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    Expect(SUCCEEDED(SHGetPropertyStoreFromParsingName(target.c_str(), nullptr, GPS_READWRITE,
                                                       IID_PPV_ARGS(store.GetAddressOf()))),
           "the crowded JPEG fixture should support writing keywords");
    std::vector<PCWSTR> names;
    for (const auto& title : titles) names.push_back(title.c_str());
    PROPVARIANT value{};
    const auto initialized = InitPropVariantFromStringVector(names.data(), static_cast<ULONG>(names.size()), &value);
    const auto written = SUCCEEDED(initialized) ? store->SetValue(PKEY_Keywords, value) : initialized;
    PropVariantClear(&value);
    Expect(SUCCEEDED(written) && SUCCEEDED(store->Commit()), "the crowded JPEG fixture should persist its tags");
}

void CheckCrowdedCaption(const fs::path& executable, const fs::path& fixture) {
    const TestCopy copy(executable);
    MONITORINFO monitor{sizeof(monitor)};
    const POINT origin{};
    Expect(GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor),
           "the test should read the primary work area");
    const UINT dpi = GetDpiForSystem();
    RECT frame{};
    Expect(AdjustWindowRectExForDpi(&frame, WS_POPUP | WS_CAPTION, FALSE, WS_EX_TOOLWINDOW, dpi) &&
           frame.bottom > frame.top, "the test should read the native caption height");
    const auto& work = monitor.rcWork;
    const LONG rows = (work.bottom - work.top) / (frame.bottom - frame.top);
    const LONG titleWidth = MulDiv(600, dpi, USER_DEFAULT_SCREEN_DPI);
    const LONG count = rows * (1 + (work.right - work.left) / titleWidth) + 8;
    constexpr std::wstring_view prefix = L"Crowded test tag ";

    Expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "the fixture writer requires COM");
    try {
        constexpr LONG tagsPerImage = 8;
        for (LONG start = 0; start < count; start += tagsPerImage) {
            tagsearch::Tags titles;
            for (LONG i = start; i < std::min(count, start + tagsPerImage); ++i) {
                titles.push_back(std::wstring(prefix) + std::to_wstring(i) + L" " + std::wstring(120, L'M'));
            }
            WriteTagFixture(fixture, copy.directory / (std::to_wstring(start) + L".jpg"), titles);
        }
    } catch (...) {
        CoUninitialize();
        throw;
    }
    CoUninitialize();

    const Process process(copy.executable, {copy.directory});
    const HWND controller = WaitForPanel(process, L"TagSearchCaptionController", false);
    const DWORD threadId = GetWindowThreadProcessId(controller, nullptr);
    VisibleTags tags{controller};
    Expect(WaitUntil([&] {
        ReadVisibleCaptionTags(threadId, tags);
        return std::count_if(tags.titles.begin(), tags.titles.end(),
                             [&](const std::wstring& title) { return title.starts_with(prefix); }) == count;
    }), "all crowded tags should be visible at the same time");
    Expect(!tags.unexpectedVisibleWindow && !IsWindowVisible(controller),
           "a crowded scan should still show only real tag windows");
    CheckTagBounds(tags, work);
    Expect(HasIntersection(tags.bounds), "overflow tags should overlap within the work area");
    ClosePanel(controller, process);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        const std::wstring_view mode = argv[1];
        if (mode == L"startup" && argc == 3) {
            CheckStartup(argv[2]);
        } else if (mode == L"fixed-window" && argc == 3) {
            CheckFixedWindowAndTitle(argv[2]);
        } else if (mode == L"treeview-blank-click" && argc == 4) {
            CheckTreeBlankClick(argv[2], argv[3]);
        } else if (mode == L"treeview-cache" && argc == 4) {
            CheckTreeCache(argv[2], argv[3]);
        } else if (mode == L"caption" && argc == 4) {
            CheckCaptionLayout(argv[2], argv[3]);
        } else if (mode == L"caption-crowded" && argc == 4) {
            CheckCrowdedCaption(argv[2], argv[3]);
        } else {
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
