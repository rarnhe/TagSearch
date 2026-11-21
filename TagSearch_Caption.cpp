#ifdef TAGSEARCH_CAPTION_LAYOUT_TEST_ONLY
#include "tests/TagSearch_Layout.h"
#else
#include "TagSearch_Core.h"
#endif

#include <algorithm>
#include <random>

namespace tagsearch {

std::vector<RECT> CaptionSlots(const RECT& workArea, SIZE slot, LONG gap, std::mt19937& random) {
    const LONG width = workArea.right - workArea.left;
    const LONG height = workArea.bottom - workArea.top;
    if (gap < 0 || slot.cx <= 0 || slot.cy <= 0 || slot.cx > width || slot.cy > height) return {};

    const LONG columns = 1 + (width - slot.cx) / (slot.cx + gap);
    const LONG rows = 1 + (height - slot.cy) / (slot.cy + gap);
    const LONG spanX = slot.cx + (columns - 1) * (slot.cx + gap);
    const LONG spanY = slot.cy + (rows - 1) * (slot.cy + gap);
    const LONG originX = workArea.left + (width - spanX) / 2;
    const LONG originY = workArea.top + (height - spanY) / 2;
    std::vector<RECT> slots;
    for (LONG row = 0; row < rows; ++row) {
        for (LONG column = 0; column < columns; ++column) {
            const LONG x = originX + column * (slot.cx + gap);
            const LONG y = originY + row * (slot.cy + gap);
            slots.push_back({x, y, x + slot.cx, y + slot.cy});
        }
    }
    std::shuffle(slots.begin(), slots.end(), random);
    return slots;
}

} // namespace tagsearch

#ifndef TAGSEARCH_CAPTION_LAYOUT_TEST_ONLY

#include <dwmapi.h>

#include <windowsx.h>
#include <cmath>
#include <iterator>
#include <map>
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

namespace {

constexpr wchar_t kControllerClass[] = L"TagSearchCaptionController";
constexpr wchar_t kTagClass[] = L"TagSearchFloatingTag";
constexpr wchar_t kMutexName[] = L"Local\\TagSearch.Caption.AEF63BC1-F7FC-4837-B810-4326C404D56E";
constexpr UINT kActivate = WM_APP + 1;
constexpr UINT kScanComplete = WM_APP + 2;
constexpr UINT kReflow = WM_APP + 3;
constexpr UINT kChangePage = WM_APP + 4;
constexpr UINT kSetTagMode = WM_APP + 5;
constexpr DWORD kTileStyle = WS_POPUP | WS_CAPTION;
constexpr DWORD kTileExStyle = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
using tagsearch::TagOperator;

struct TagMode {
    TagOperator operation;
    const wchar_t* label;
    COLORREF color;
};

constexpr TagMode kTagModes[]{
    {TagOperator::And, L"AND", RGB(190, 235, 199)},
    {TagOperator::Or, L"OR", RGB(248, 202, 219)},
    {TagOperator::Not, L"NOT", RGB(210, 212, 216)},
};

struct Caption;

struct FloatingTag {
    Caption& caption;
    std::wstring text;
    TagOperator operation = TagOperator::And;
    HWND window = nullptr;
    bool pressed = false;
    bool dragging = false;
    POINT start{};
    POINT last{};
    bool positioned = false;
};

struct ScanResult {
    tagsearch::Folders folders;
    tagsearch::Tags tags;
    bool success = false;
};

struct Caption {
    HINSTANCE instance;
    HWND controller = nullptr;
    std::map<UINT, HFONT> tagFonts;
    std::vector<std::unique_ptr<FloatingTag>> tags;
    std::vector<RECT> slots;
    std::mt19937 random{std::random_device{}()};
    tagsearch::Folders folders;
    std::size_t page = 0;
    std::size_t pageCount = 1;
    ScanResult scan;
    std::jthread scanner;

    ~Caption() {
        if (controller) DestroyWindow(controller);
        for (const auto& entry : tagFonts) DeleteObject(entry.second);
    }
};

int Scale(int logicalPixels, UINT dpi) {
    return MulDiv(logicalPixels, dpi, USER_DEFAULT_SCREEN_DPI);
}

UINT WindowDpi(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

RECT PrimaryWorkArea() {
    MONITORINFO monitor{sizeof(monitor)};
    const POINT origin{};
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor);
    return monitor.rcWork;
}

int TileHeight(UINT dpi) {
    RECT frame{0, 0, 0, Scale(30, dpi)};
    AdjustWindowRectExForDpi(&frame, kTileStyle, FALSE, kTileExStyle, dpi);
    return frame.bottom - frame.top;
}

HFONT MakeFont(int pixels, int weight, const wchar_t* family) {
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH, family);
}

HFONT TagFont(Caption& caption, UINT dpi) {
    if (const auto found = caption.tagFonts.find(dpi); found != caption.tagFonts.end()) {
        return found->second;
    }
    const HFONT font = MakeFont(Scale(22, dpi), FW_BOLD, L"LiSu");
    if (!font) {
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    // The Windows font mapper supplies a fallback when LiSu is unavailable.
    caption.tagFonts.emplace(dpi, font);
    return font;
}

struct Palette {
    COLORREF background;
    COLORREF text;
    BYTE opacity;
};

double Luminance(COLORREF color) {
    const auto linear = [](int channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(GetRValue(color)) +
           0.7152 * linear(GetGValue(color)) +
           0.0722 * linear(GetBValue(color));
}

COLORREF TagColor(TagOperator operation) {
    return kTagModes[static_cast<std::size_t>(operation)].color;
}

Palette TilePalette(COLORREF background) {
    HIGHCONTRASTW highContrast{sizeof(highContrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
        (highContrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT), 255};
    }
    const COLORREF text = Luminance(background) >= 0.179 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    return {background, text, 225};
}

void ApplyPalette(HWND window, COLORREF color) {
    const auto palette = TilePalette(color);
    SetLayeredWindowAttributes(window, 0, palette.opacity, LWA_ALPHA);
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &palette.background, sizeof(COLORREF));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &palette.text, sizeof(COLORREF));
    InvalidateRect(window, nullptr, FALSE);
}

void PaintTile(HWND window, Caption& caption, std::wstring_view text, COLORREF color) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const auto palette = TilePalette(color);
    SetDCBrushColor(dc, palette.background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const UINT dpi = WindowDpi(window);
    const auto previous = SelectObject(dc, TagFont(caption, dpi));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, palette.text);
    InflateRect(&client, -Scale(6, dpi), 0);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &client,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, previous);
    EndPaint(window, &paint);
}

bool PastDragThreshold(HWND window, const FloatingTag& tag, POINT point) {
    const UINT dpi = WindowDpi(window);
    return std::abs(point.x - tag.start.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
           std::abs(point.y - tag.start.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi);
}

void ActivateTag(HWND window, const FloatingTag& tag) {
    // Use current screen coordinates so dragging a tag changes the next search.
    RECT selected{};
    std::vector<tagsearch::TagWindowBounds> visibleWindows;
    if (GetWindowRect(window, &selected)) {
        for (const auto& other : tag.caption.tags) {
            RECT bounds{};
            if (other->window != window && IsWindowVisible(other->window) &&
                GetWindowRect(other->window, &bounds)) {
                visibleWindows.push_back({bounds, other->text, other->operation});
            }
        }
    }
    const auto terms = tagsearch::TermsForOverlappingWindows(
        {selected, tag.text, tag.operation}, visibleWindows);
    if (!tagsearch::OpenTagQuery(window, terms, tag.caption.folders)) {
        MessageBoxW(window, L"无法在文件资源管理器中打开标签搜索。", L"TagSearch", MB_OK | MB_ICONWARNING);
    }
}

void ShowTagMenu(HWND window, TagOperator operation, LPARAM position) {
    const HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (const auto& mode : kTagModes) {
        AppendMenuW(menu, MF_STRING | (operation == mode.operation ? MF_CHECKED : MF_UNCHECKED),
                    static_cast<UINT_PTR>(mode.operation) + 1, mode.label);
    }

    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    if (point.x == -1 && point.y == -1) {
        RECT bounds{};
        GetWindowRect(window, &bounds);
        point = {bounds.left, bounds.bottom};
    }
    const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                        point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (command > 0) PostMessageW(window, kSetTagMode, command, 0);
}

void ChangePage(HWND controller, WPARAM key) {
    PostMessageW(controller, kChangePage, key, 0);
}

LRESULT CALLBACK TagsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* tag = reinterpret_cast<FloatingTag*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!tag) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
    case WM_SYSCOLORCHANGE:
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ApplyPalette(window, TagColor(tag->operation));
        return 0;
    case WM_PAINT:
        PaintTile(window, tag->caption, tag->text, TagColor(tag->operation));
        return 0;
    case WM_DPICHANGED: {
        const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_NCLBUTTONDOWN:
        if (wParam != HTCAPTION) break;
        [[fallthrough]];
    case WM_LBUTTONDOWN:
        if (GetCursorPos(&tag->start)) {
            tag->last = tag->start;
            tag->pressed = true;
            tag->dragging = false;
            SetCapture(window);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (tag->pressed) {
            POINT point{};
            if (GetCursorPos(&point) && (tag->dragging || PastDragThreshold(window, *tag, point))) {
                tag->dragging = true;
                RECT bounds{};
                const RECT work = PrimaryWorkArea();
                if (GetWindowRect(window, &bounds) && !IsRectEmpty(&work)) {
                    const LONG maxX = std::max(work.left, work.right - (bounds.right - bounds.left));
                    const LONG maxY = std::max(work.top, work.bottom - (bounds.bottom - bounds.top));
                    const LONG x = std::clamp(bounds.left + point.x - tag->last.x, work.left, maxX);
                    const LONG y = std::clamp(bounds.top + point.y - tag->last.y, work.top, maxY);
                    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
                }
                tag->last = point;
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (tag->pressed) {
            POINT point{};
            const bool clicked = !tag->dragging && GetCursorPos(&point) &&
                                 !PastDragThreshold(window, *tag, point);
            tag->pressed = false;
            ReleaseCapture();
            if (clicked) ActivateTag(window, *tag);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        tag->pressed = false;
        tag->dragging = false;
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            if ((HIWORD(lParam) & KF_REPEAT) == 0) ActivateTag(window, *tag);
            return 0;
        }
        if (wParam == VK_PRIOR || wParam == VK_NEXT) {
            ChangePage(tag->caption.controller, wParam);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            PostMessageW(tag->caption.controller, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        ChangePage(tag->caption.controller, GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? VK_PRIOR : VK_NEXT);
        return 0;
    case WM_NCRBUTTONUP:
    case WM_CONTEXTMENU:
        ShowTagMenu(window, tag->operation, lParam);
        return 0;
    case kSetTagMode:
        if (wParam > 0 && wParam <= std::size(kTagModes)) {
            tag->operation = kTagModes[wParam - 1].operation;
            ApplyPalette(window, TagColor(tag->operation));
        }
        return 0;
    case WM_CLOSE:
        PostMessageW(tag->caption.controller, WM_CLOSE, 0, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::unique_ptr<FloatingTag> CreateTag(Caption& caption, const std::wstring& text, const RECT& work) {
    auto tag = std::make_unique<FloatingTag>(FloatingTag{caption, text});
    tag->window = CreateWindowExW(kTileExStyle, kTagClass, text.c_str(), kTileStyle,
        work.left, work.top, 1, 1, caption.controller, nullptr, caption.instance, tag.get());
    return tag->window ? std::move(tag) : nullptr;
}

void BuildTagLayout(Caption& caption) {
    const RECT work = PrimaryWorkArea();
    SetWindowPos(caption.controller, nullptr, work.left, work.top, 0, 0,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    const UINT dpi = WindowDpi(caption.controller);
    const LONG width = std::min<LONG>(Scale(230, dpi), work.right - work.left);
    const LONG height = std::min<LONG>(TileHeight(dpi), work.bottom - work.top);
    caption.slots = tagsearch::CaptionSlots(work, {width, height}, Scale(8, dpi), caption.random);
    for (const auto& tag : caption.tags) tag->positioned = false;
}

void LayoutTags(Caption& caption) {
    const UINT dpi = WindowDpi(caption.controller);
    const auto capacity = caption.slots.size();
    caption.pageCount = capacity && !caption.tags.empty() ? 1 + (caption.tags.size() - 1) / capacity : 1;
    caption.page = std::min(caption.page, caption.pageCount - 1);
    const std::size_t first = caption.page * capacity;
    const std::size_t last = first + std::min(capacity, caption.tags.size() - first);

    const HDC dc = GetDC(caption.controller);
    const auto oldFont = dc ? SelectObject(dc, TagFont(caption, dpi)) : nullptr;
    for (std::size_t i = 0; i < caption.tags.size(); ++i) {
        auto& tag = *caption.tags[i];
        if (i < first || i >= last) {
            ShowWindow(tag.window, SW_HIDE);
            continue;
        }

        if (!tag.positioned) {
            const RECT& slot = caption.slots[i - first];
            SIZE text{};
            if (dc) GetTextExtentPoint32W(dc, tag.text.c_str(), static_cast<int>(tag.text.size()), &text);
            const int width = std::min(static_cast<int>(slot.right - slot.left),
                std::max(Scale(52, dpi), static_cast<int>(text.cx) + Scale(36, dpi)));
            const LONG x = slot.left + (slot.right - slot.left - width) / 2;
            SetWindowPos(tag.window, nullptr, x, slot.top, width, slot.bottom - slot.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            tag.positioned = true;
        }
        ShowWindow(tag.window, SW_SHOWNA);
    }
    if (dc) {
        SelectObject(dc, oldFont);
        ReleaseDC(caption.controller, dc);
    }
}

void ActivateTags(Caption& caption) {
    LayoutTags(caption);
    // Preserve the stacking order of overlapping tags when bringing the page forward.
    std::vector<HWND> windows;
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (GetWindow(window, GW_OWNER) == caption.controller && IsWindowVisible(window)) {
            windows.push_back(window);
        }
    }
    for (auto i = windows.rbegin(); i != windows.rend(); ++i) {
        if (IsIconic(*i)) ShowWindow(*i, SW_RESTORE);
        SetWindowPos(*i, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (!windows.empty()) SetForegroundWindow(windows.front());
}

void ScanInBackground(Caption& caption, std::stop_token stop) {
    auto& scan = caption.scan;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        try {
            scan.folders = tagsearch::ResolveSearchFolders(GetCommandLineW());
            scan.tags = tagsearch::ScanTags(scan.folders, stop);
            scan.success = !stop.stop_requested();
        } catch (...) {
            scan.success = false;
        }
        CoUninitialize();
    }
    // The controller is owned by the UI thread and outlives the scanner.
    if (!stop.stop_requested()) PostMessageW(caption.controller, kScanComplete, 0, 0);
}

bool StartStartupScan(Caption& caption) {
    try {
        caption.scanner = std::jthread([&caption](std::stop_token stop) {
            ScanInBackground(caption, stop);
        });
    } catch (const std::system_error&) {
        return false;
    }
    return true;
}

void ShowScanResult(Caption& caption) {
    if (!caption.scanner.joinable()) return;
    caption.scanner.join();
    if (!caption.scan.success) {
        PostQuitMessage(1);
        return;
    }

    caption.folders = std::move(caption.scan.folders);
    const RECT work = PrimaryWorkArea();
    for (const auto& text : caption.scan.tags) {
        if (auto tag = CreateTag(caption, text, work)) caption.tags.push_back(std::move(tag));
    }
    const bool scanWasEmpty = caption.scan.tags.empty();
    caption.scan = {};
    if (caption.tags.empty()) {
        PostQuitMessage(scanWasEmpty ? 0 : 1);
        return;
    }
    BuildTagLayout(caption);
    ActivateTags(caption);
}

LRESULT CALLBACK ControllerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* caption = reinterpret_cast<Caption*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!caption) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case kScanComplete:
        ShowScanResult(*caption);
        return 0;
    case kActivate:
        ActivateTags(*caption);
        return 0;
    case WM_SETTINGCHANGE:
        if (wParam != SPI_SETWORKAREA) break;
        [[fallthrough]];
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        PostMessageW(window, kReflow, 0, 0);
        return 0;
    case kReflow:
        if (!caption->tags.empty()) {
            BuildTagLayout(*caption);
            LayoutTags(*caption);
        }
        return 0;
    case kChangePage:
        if (wParam == VK_PRIOR && caption->page > 0) {
            --caption->page;
            ActivateTags(*caption);
        } else if (wParam == VK_NEXT && caption->page + 1 < caption->pageCount) {
            ++caption->page;
            ActivateTags(*caption);
        }
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int Run(HINSTANCE instance) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Caption caption{instance};

    WNDCLASSW controllerClass{};
    controllerClass.hInstance = instance;
    controllerClass.lpfnWndProc = ControllerProc;
    controllerClass.lpszClassName = kControllerClass;
    if (!RegisterClassW(&controllerClass)) return 1;

    WNDCLASSW tagClass{};
    tagClass.hInstance = instance;
    tagClass.lpfnWndProc = TagsProc;
    tagClass.lpszClassName = kTagClass;
    tagClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    if (!RegisterClassW(&tagClass)) return 1;

    const RECT work = PrimaryWorkArea();
    if (IsRectEmpty(&work)) return 1;
    caption.controller = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kControllerClass, nullptr, WS_POPUP, work.left, work.top, 1, 1,
        nullptr, nullptr, instance, &caption);
    if (!caption.controller || !StartStartupScan(caption)) return 1;

    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (caption.scanner.joinable()) {
        caption.scanner.request_stop();
        caption.scanner.join();
    }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}

int RunSingleInstance(HINSTANCE instance) {
    HANDLE rawMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const DWORD status = GetLastError();
    const std::unique_ptr<void, decltype(&CloseHandle)> mutex(rawMutex, &CloseHandle);
    if (!mutex) return 1;

    if (status == ERROR_ALREADY_EXISTS) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        for (;;) {
            const DWORD wait = WaitForSingleObject(mutex.get(), 0);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) break;
            if (wait != WAIT_TIMEOUT || GetTickCount64() >= deadline) return 1;

            if (const HWND existing = FindWindowW(kControllerClass, nullptr)) {
                DWORD pid = 0;
                if (GetWindowThreadProcessId(existing, &pid)) {
                    AllowSetForegroundWindow(pid);
                    if (PostMessageW(existing, kActivate, 0, 0)) return 0;
                }
            }
            Sleep(25);
        }
    }

    int result = 1;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        result = Run(instance);
        CoUninitialize();
    }
    ReleaseMutex(mutex.get());
    return result;
}

} // namespace

_Use_decl_annotations_
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    return RunSingleInstance(instance);
}

#endif // TAGSEARCH_CAPTION_LAYOUT_TEST_ONLY
