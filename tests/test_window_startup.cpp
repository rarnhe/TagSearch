#include "TagSearch_Core.h"

#include <shlobj.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

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

HWND WaitForPanel(const Process& process, const wchar_t* className = L"TagSearchTreeView") {
    HWND window = nullptr;
    Expect(WaitUntil([&] {
        window = FindWindowW(className, nullptr);
        DWORD processId = 0;
        return window && GetWindowThreadProcessId(window, &processId) &&
               processId == process.info.dwProcessId && IsWindowVisible(window);
    }), "the launch should show its own panel");
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

struct VisibleTags {
    HWND anchor;
    std::vector<HWND> windows;
    std::vector<RECT> bounds;
    tagsearch::Tags titles;
};

BOOL CALLBACK RecordVisibleTags(HWND window, LPARAM parameter) {
    auto& tags = *reinterpret_cast<VisibleTags*>(parameter);
    RECT rect{};
    if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == tags.anchor &&
        GetWindowRect(window, &rect)) {
        std::array<wchar_t, 128> title{};
        GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        tags.windows.push_back(window);
        tags.bounds.push_back(rect);
        tags.titles.emplace_back(title.data());
    }
    return TRUE;
}

bool Within(const RECT& rect, const RECT& area) {
    return rect.left >= area.left && rect.top >= area.top &&
           rect.right <= area.right && rect.bottom <= area.bottom;
}

void RecordCaptionPage(DWORD threadId, VisibleTags& tags) {
    tags.windows.clear();
    tags.bounds.clear();
    tags.titles.clear();
    EnumThreadWindows(threadId, RecordVisibleTags, reinterpret_cast<LPARAM>(&tags));
}

void CheckTagBounds(const VisibleTags& tags, const RECT& work, const RECT& anchor) {
    for (std::size_t i = 0; i < tags.bounds.size(); ++i) {
        const auto& rect = tags.bounds[i];
        RECT overlap{};
        Expect(Within(rect, work), "the tag window must stay inside the primary work area");
        Expect(!IntersectRect(&overlap, &rect, &anchor), "the tag must not cover the refresh window");
        for (std::size_t j = 0; j < i; ++j) {
            Expect(!IntersectRect(&overlap, &rect, &tags.bounds[j]), "the laid-out tag windows must not overlap");
        }
    }
}

void CheckCaptionLayout(const fs::path& executable, const fs::path& fixture) {
    const TestCopy copy(executable);
    const auto copiedFixture = copy.directory / L"tagged.jpg";
    fs::copy_file(fixture, copiedFixture);
    const Process first(copy.executable, {copy.directory});
    const HWND anchor = WaitForPanel(first, L"TagSearchCaptionController");
    WaitForTitle(anchor, L"↺");
    Expect(GetWindow(anchor, GW_OWNER) == nullptr,
           "the refresh window should be the visible root without a hidden owner");
    RECT anchorBounds{};
    MONITORINFO monitor{sizeof(monitor)};
    const POINT origin{};
    Expect(GetWindowRect(anchor, &anchorBounds) &&
           GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor) &&
           Within(anchorBounds, monitor.rcWork),
           "the visible refresh window should start entirely inside the primary work area");

    DWORD_PTR result = 0;
    Expect(SendMessageTimeoutW(anchor, WM_CONTEXTMENU, 0, -1, SMTO_ABORTIFHUNG,
                               1000, &result) && result == 0,
           "the refresh window should dismiss the context-menu message without opening a menu");

    VisibleTags tags{anchor};
    const DWORD threadId = GetWindowThreadProcessId(anchor, nullptr);
    Expect(WaitUntil([&] {
        RecordCaptionPage(threadId, tags);
        return std::find(tags.titles.begin(), tags.titles.end(), L"alpha") != tags.titles.end() &&
               std::find(tags.titles.begin(), tags.titles.end(), L"项目 & 资料") != tags.titles.end();
    }), "the background scan should show both real labels read from the JPEG fixture");

    CheckTagBounds(tags, monitor.rcWork, anchorBounds);

    const auto originalWindows = tags.windows;
    const auto& work = monitor.rcWork;
    const LONG anchorWidth = anchorBounds.right - anchorBounds.left;
    const LONG anchorHeight = anchorBounds.bottom - anchorBounds.top;
    const LONG middle = work.left + (work.right - work.left) / 2;
    const LONG x = anchorBounds.left < middle ? work.right - anchorWidth : work.left;
    const LONG y = work.bottom - anchorHeight;
    constexpr UINT moveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE;
    Expect(SetWindowPos(anchor, nullptr, x, y, 0, 0, moveFlags) &&
           GetWindowRect(anchor, &anchorBounds),
           "the test should move the refresh window to another corner");

    const HWND tag = originalWindows.front();
    RECT tagBounds{};
    RECT overlap{};
    Expect(SetWindowPos(tag, nullptr, anchorBounds.left, anchorBounds.top, 0, 0, moveFlags) &&
           GetWindowRect(tag, &tagBounds) && IntersectRect(&overlap, &tagBounds, &anchorBounds),
           "the test should put an existing tag on top of the moved refresh window");

    Expect(fs::remove(copiedFixture), "the test should remove the scanned file before using refresh");
    Expect(SendMessageTimeoutW(anchor, WM_LBUTTONUP, 0, 0, SMTO_ABORTIFHUNG, 1000, &result),
           "clicking the refresh window should arrange the current windows immediately");
    RecordCaptionPage(threadId, tags);
    Expect(tags.windows.size() == originalWindows.size() &&
           std::is_permutation(originalWindows.begin(), originalWindows.end(), tags.windows.begin()),
           "clicking refresh should keep the existing tag windows even after removing the scanned file");
    CheckTagBounds(tags, work, anchorBounds);

    const Process second(copy.executable);
    second.ExpectSuccessfulExit("Caption's duplicate launch should forward activation and exit");
    Expect(FindWindowW(L"TagSearchCaptionController", L"↺") == anchor,
           "the original refresh window should survive a repeated launch");
    ClosePanel(anchor, first);
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
        } else if (mode == L"caption" && argc == 4) {
            CheckCaptionLayout(argv[2], argv[3]);
        } else {
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
