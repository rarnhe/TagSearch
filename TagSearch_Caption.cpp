#include "TagSearchCore.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace {

constexpr wchar_t kPanelClass[] = L"TagSearchCaption";
constexpr wchar_t kTagClass[] = L"TagSearchFloatingTag";
constexpr UINT kRefresh = WM_APP + 1;
constexpr UINT kReflow = WM_APP + 2;
constexpr INT_PTR kPrevious = 1;
constexpr INT_PTR kNext = 2;

struct Panel;

struct FloatingTag {
    Panel& panel;
    std::wstring text;
    bool isRefresh;
    HWND window = nullptr;
    bool pressed = false;
    bool dragging = false;
    POINT start{};
    POINT last{};
    bool positioned = false;
};

struct Panel {
    HINSTANCE instance;
    tagsearch::Folders folders;
    HWND window = nullptr;
    HWND status = nullptr;
    HWND previous = nullptr;
    HWND next = nullptr;
    HMONITOR monitor = nullptr;
    HFONT font = nullptr;
    std::map<UINT, HFONT> tagFonts;
    std::unique_ptr<FloatingTag> refresh;
    std::vector<std::unique_ptr<FloatingTag>> tags;
    std::size_t discoveredTags = 0;
    std::size_t page = 0;

    ~Panel() {
        if (window && IsWindow(window)) DestroyWindow(window);
        if (font) DeleteObject(font);
        for (const auto& [dpi, tagFont] : tagFonts) {
            DeleteObject(tagFont);
        }
    }
};

int Scale(int logicalPixels, UINT dpi) {
    return MulDiv(logicalPixels, dpi, USER_DEFAULT_SCREEN_DPI);
}

UINT WindowDpi(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

RECT WorkArea(HWND window) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
    return monitor.rcWork;
}

HFONT MakeFont(int pixels, int weight, const wchar_t* family) {
    return CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH, family);
}

HFONT TagFont(Panel& panel, UINT dpi) {
    if (const auto found = panel.tagFonts.find(dpi); found != panel.tagFonts.end()) {
        return found->second;
    }
    const HFONT font = MakeFont(Scale(22, dpi), FW_BOLD, L"LiSu");
    if (!font) {
        return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    // The Windows font mapper supplies a fallback when LiSu is unavailable.
    panel.tagFonts.emplace(dpi, font);
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

Palette TagPalette() {
    HIGHCONTRASTW highContrast{sizeof(highContrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
        (highContrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT), 255};
    }

    DWORD color = 0;
    BOOL opaque = FALSE;
    const COLORREF accent = SUCCEEDED(DwmGetColorizationColor(&color, &opaque))
        ? RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF)
        : GetSysColor(COLOR_ACTIVECAPTION);
    const bool light = Luminance(accent) >= 0.179;
    const auto tint = [light](int channel) { return light ? (channel + 255) / 2 : channel / 2; };
    return {RGB(tint(GetRValue(accent)), tint(GetGValue(accent)), tint(GetBValue(accent))),
            light ? RGB(0, 0, 0) : RGB(255, 255, 255), 225};
}

void ApplyPalette(HWND window) {
    const auto palette = TagPalette();
    SetLayeredWindowAttributes(window, 0, palette.opacity, LWA_ALPHA);
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &palette.background, sizeof(COLORREF));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &palette.text, sizeof(COLORREF));
    InvalidateRect(window, nullptr, FALSE);
}

void PaintTag(HWND window, FloatingTag& tag) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const auto palette = TagPalette();
    SetDCBrushColor(dc, palette.background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const auto previous = SelectObject(dc, TagFont(tag.panel, WindowDpi(window)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, palette.text);
    InflateRect(&client, -Scale(6, WindowDpi(window)), 0);
    DrawTextW(dc, tag.text.c_str(), -1, &client,
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
    if (tag.isRefresh) {
        PostMessageW(tag.panel.window, kRefresh, 0, 0);
        return;
    }

    // Use current screen coordinates so dragging a tag changes the next search.
    RECT selected{};
    std::vector<tagsearch::TagWindowBounds> visibleWindows;
    if (GetWindowRect(window, &selected)) {
        for (const auto& other : tag.panel.tags) {
            RECT bounds{};
            if (other->window != window && IsWindowVisible(other->window) &&
                GetWindowRect(other->window, &bounds)) {
                visibleWindows.push_back({bounds, other->text});
            }
        }
    }
    const auto tags = tagsearch::TagsForOverlappingWindows({selected, tag.text}, visibleWindows);
    if (!tagsearch::OpenTagSearch(window, tags, tag.panel.folders)) {
        MessageBoxW(window, L"无法在文件资源管理器中打开标签搜索。", L"TagSearch", MB_OK | MB_ICONWARNING);
    }
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
        ApplyPalette(window);
        return 0;
    case WM_PAINT:
        PaintTag(window, *tag);
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
                GetWindowRect(window, &bounds);
                SetWindowPos(window, nullptr, bounds.left + point.x - tag->last.x,
                             bounds.top + point.y - tag->last.y, 0, 0,
                             SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
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
            if ((lParam & (1L << 30)) == 0) ActivateTag(window, *tag);
            return 0;
        }
        break;
    case WM_CLOSE:
        // The panel manages the tag windows for the lifetime of this scan.
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::unique_ptr<FloatingTag> CreateTag(Panel& panel, std::wstring text, bool refresh = false) {
    auto tag = std::make_unique<FloatingTag>(FloatingTag{panel, std::move(text), refresh});
    const RECT work = WorkArea(panel.window);
    tag->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, kTagClass, tag->text.c_str(),
        WS_POPUP | WS_CAPTION, work.left, work.top, 1, 1, panel.window, nullptr, panel.instance, tag.get());
    return tag->window ? std::move(tag) : nullptr;
}

void LayoutPanel(Panel& panel) {
    const UINT dpi = WindowDpi(panel.window);
    const HFONT font = MakeFont(Scale(15, dpi), FW_NORMAL, L"Segoe UI");
    if (font) {
        for (const HWND child : {panel.status, panel.previous, panel.next}) {
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        if (panel.font) DeleteObject(panel.font);
        panel.font = font;
    }
    MoveWindow(panel.status, Scale(12, dpi), Scale(9, dpi), Scale(265, dpi), Scale(23, dpi), TRUE);
    MoveWindow(panel.previous, Scale(12, dpi), Scale(39, dpi), Scale(78, dpi), Scale(27, dpi), TRUE);
    MoveWindow(panel.next, Scale(98, dpi), Scale(39, dpi), Scale(78, dpi), Scale(27, dpi), TRUE);
}

void LayoutTags(Panel& panel) {
    const UINT dpi = WindowDpi(panel.window);
    const RECT work = WorkArea(panel.window);
    const int gap = Scale(8, dpi);
    const int cellWidth = std::max(1, std::min(Scale(230, dpi), int(work.right - work.left) - 2 * gap));
    constexpr DWORD style = WS_POPUP | WS_CAPTION;
    RECT frame{0, 0, cellWidth, Scale(30, dpi)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_TOOLWINDOW, dpi);
    const int cellHeight = frame.bottom - frame.top;
    const int top = std::min(work.top + Scale(122, dpi), work.bottom - cellHeight - gap);
    const int rows = std::max(1, (static_cast<int>(work.bottom) - top) / (cellHeight + gap));
    const int columns = std::max(1, int(work.right - work.left - gap) / (cellWidth + gap));
    // Reserve the first slot for refresh. Leave at least one additional slot on small displays.
    const std::size_t capacity = static_cast<std::size_t>(std::max(1, rows * columns - 1));
    const std::size_t pages = std::max<std::size_t>(1, (panel.tags.size() + capacity - 1) / capacity);
    panel.page = std::min(panel.page, pages - 1);
    const std::size_t first = panel.page * capacity;
    const std::size_t last = std::min(first + capacity, panel.tags.size());

    const HDC dc = GetDC(panel.window);
    const auto oldFont = dc ? SelectObject(dc, TagFont(panel, dpi)) : nullptr;
    const auto place = [&](FloatingTag& tag, std::size_t slot) {
        if (!tag.positioned) {
            SIZE text{};
            if (dc) GetTextExtentPoint32W(dc, tag.text.c_str(), static_cast<int>(tag.text.size()), &text);
            const int width = std::min(cellWidth, std::max(Scale(52, dpi), static_cast<int>(text.cx) + Scale(36, dpi)));
            const int column = static_cast<int>(slot / rows);
            const int row = static_cast<int>(slot % rows);
            const int right = work.right - gap - column * (cellWidth + gap);
            const int x = std::max(static_cast<int>(work.left), right - width);
            const int y = top + row * (cellHeight + gap);
            SetWindowPos(tag.window, nullptr, x, y, width, cellHeight, SWP_NOACTIVATE | SWP_NOZORDER);
            tag.positioned = true;
        }
        if (!IsIconic(panel.window) && IsWindowVisible(panel.window)) ShowWindow(tag.window, SW_SHOWNA);
    };

    if (panel.refresh) place(*panel.refresh, 0);
    for (std::size_t i = 0; i < panel.tags.size(); ++i) {
        if (i >= first && i < last) {
            place(*panel.tags[i], i - first + (panel.refresh ? 1 : 0));
        } else {
            ShowWindow(panel.tags[i]->window, SW_HIDE);
        }
    }
    if (dc) {
        SelectObject(dc, oldFont);
        ReleaseDC(panel.window, dc);
    }

    std::wstring status = L"标签 " + std::to_wstring(panel.discoveredTags) +
        L"    ·    目录 " + std::to_wstring(panel.folders.size());
    if (pages > 1) status += L"    ·    " + std::to_wstring(panel.page + 1) + L"/" + std::to_wstring(pages);
    if (panel.discoveredTags != panel.tags.size()) status += L"    ·    部分窗口未创建";
    SetWindowTextW(panel.status, status.c_str());
    EnableWindow(panel.previous, panel.page > 0);
    EnableWindow(panel.next, panel.page + 1 < pages);
}

void RefreshTags(Panel& panel) {
    SetWindowTextW(panel.status, L"正在读取标签…");
    UpdateWindow(panel.window);
    const auto tags = tagsearch::ScanTags(panel.folders);
    for (const auto& tag : panel.tags) DestroyWindow(tag->window);
    panel.tags.clear();
    panel.discoveredTags = tags.size();
    panel.page = 0;
    if (panel.refresh) panel.refresh->positioned = false;

    for (const auto& text : tags) {
        if (auto tag = CreateTag(panel, text)) {
            panel.tags.push_back(std::move(tag));
        }
    }
    LayoutTags(panel);
}

LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!panel) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        panel->window = window;
        panel->monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
        panel->status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                                      0, 0, 0, 0, window, nullptr, panel->instance, nullptr);
        panel->previous = CreateWindowW(L"BUTTON", L"上一页", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        0, 0, 0, 0, window, reinterpret_cast<HMENU>(kPrevious), panel->instance, nullptr);
        panel->next = CreateWindowW(L"BUTTON", L"下一页", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(kNext), panel->instance, nullptr);
        if (!panel->status || !panel->previous || !panel->next) return -1;
        LayoutPanel(*panel);
        return 0;
    case WM_DPICHANGED: {
        const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
        LayoutPanel(*panel);
        PostMessageW(window, kReflow, 0, 0);
        return 0;
    }
    case WM_MOVE:
        if (!IsIconic(window)) {
            const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
            if (monitor != panel->monitor) {
                panel->monitor = monitor;
                PostMessageW(window, kReflow, 0, 0);
            }
        }
        break;
    case WM_SETTINGCHANGE:
        if (wParam != SPI_SETWORKAREA) break;
        [[fallthrough]];
    case WM_DISPLAYCHANGE:
        PostMessageW(window, kReflow, 0, 0);
        return 0;
    case kReflow:
        if (panel->refresh) panel->refresh->positioned = false;
        for (auto& tag : panel->tags) tag->positioned = false;
        LayoutTags(*panel);
        return 0;
    case kRefresh:
        RefreshTags(*panel);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) break;
        if (LOWORD(wParam) == kPrevious && panel->page > 0) {
            --panel->page;
        } else if (LOWORD(wParam) == kNext && IsWindowEnabled(panel->next)) {
            ++panel->page;
        } else {
            break;
        }
        LayoutTags(*panel);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        panel->window = nullptr;
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int Run(HINSTANCE instance, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Panel panel{instance, tagsearch::ResolveSearchFolders(GetCommandLineW())};

    WNDCLASSW panelClass{};
    panelClass.hInstance = instance;
    panelClass.lpfnWndProc = PanelProc;
    panelClass.lpszClassName = kPanelClass;
    panelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    panelClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassW(&panelClass)) return 1;

    WNDCLASSW tagClass{};
    tagClass.hInstance = instance;
    tagClass.lpfnWndProc = TagsProc;
    tagClass.lpszClassName = kTagClass;
    tagClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    if (!RegisterClassW(&tagClass)) return 1;

    const auto work = WorkArea(nullptr);
    const UINT dpi = GetDpiForSystem();
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT frame{0, 0, Scale(290, dpi), Scale(77, dpi)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const HWND window = CreateWindowW(kPanelClass, L"TagSearch · Caption", style,
        work.right - width, work.top, width, height, nullptr, nullptr, instance, &panel);
    if (!window) return 1;

    panel.refresh = CreateTag(panel, L"↺", true);
    if (!panel.refresh) return 1;
    ShowWindow(window, showCommand);
    RefreshTags(panel);
    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (GetAncestor(message.hwnd, GA_ROOT) != window || !IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    const int result = Run(instance, showCommand);
    CoUninitialize();
    return result;
}
