#include "TagSearch_Core.h"

#include <shlobj.h>

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

    explicit Process(const fs::path& executable) {
        STARTUPINFOW startup{sizeof(startup)};
        // Pass only the quoted executable name, as a double-click would.
        auto commandLine = L"\"" + executable.wstring() + L"\"";
        Expect(CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0,
                             nullptr, nullptr, &startup, &info) != FALSE,
               "CreateProcessW should launch TreeView");
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

void CheckStartup(const fs::path& executable) {
    // Use a distinct executable name so the normal shortcut does not get modified.
    const TestCopy copy(executable);
    const Process first(copy.executable);
    HWND window = nullptr;
    Expect(WaitUntil([&] {
        window = FindWindowW(L"TagSearchTreeView", nullptr);
        DWORD processId = 0;
        return window && GetWindowThreadProcessId(window, &processId) &&
               processId == first.info.dwProcessId && IsWindowVisible(window);
    }), "the first launch without arguments should show its own panel");

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

    Expect(WaitUntil([&] {
        std::array<wchar_t, 128> title{};
        GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
        return std::wstring_view(title.data()) == L"TagSearch · TreeView";
    }), "the default directory scan should finish after the no-argument launch");
    Expect(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE, "the existing panel should close normally");
    first.ExpectSuccessfulExit("closing the panel should release the application instance");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        CheckStartup(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
