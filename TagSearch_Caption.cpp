#ifdef TAGSEARCH_CAPTION_LAYOUT_TEST_ONLY
#include "tests/TagSearch_Layout.h"
#else
#include "TagSearch_Core.h"
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <vector>

namespace tagsearch {

namespace {

bool Overlaps(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

std::size_t CountOverlappingWindows(std::span<const RECT> bounds) {
    std::size_t count = 0;
    for (const auto& rect : bounds) {
        if (std::any_of(bounds.begin(), bounds.end(), [&](const RECT& other) {
                return &rect != &other && Overlaps(rect, other);
            })) {
            ++count;
        }
    }
    return count;
}

bool PlaceCaptionCloud(const RECT& work, std::span<const LONG> widths, LONG height,
                       std::span<const std::size_t> order, std::span<RECT> bounds,
                       std::mt19937& random) {
    const LONG gap = std::max(2L, height / 4);
    const LONG workWidth = work.right - work.left;
    const LONG workHeight = work.bottom - work.top;
    const double area = std::accumulate(widths.begin(), widths.end(), 0.0, [&](double sum, LONG width) {
        return sum + (std::clamp(width, 1L, workWidth) + gap) * static_cast<double>(height + gap);
    });
    if (area > static_cast<double>(workWidth) * workHeight) return false;

    const double radius = std::sqrt(area);
    const double centerX = work.left + workWidth / 2.0;
    const double centerY = work.top + workHeight / 2.0;
    std::normal_distribution<double> cloudX(centerX, radius * 0.55);
    std::normal_distribution<double> cloudY(centerY, std::max(static_cast<double>(height), radius * 0.34));
    std::vector<RECT> placed;
    placed.reserve(widths.size());
    for (const auto index : order) {
        const LONG width = std::clamp(widths[index], 1L, workWidth);
        bool placedTag = false;
        for (int attempt = 0; attempt < 128; ++attempt) {
            const double xCenter = cloudX(random);
            const double yCenter = cloudY(random);
            const LONG x = static_cast<LONG>(std::clamp(xCenter - width / 2.0,
                static_cast<double>(work.left), static_cast<double>(work.right - width)));
            const LONG y = static_cast<LONG>(std::clamp(yCenter - height / 2.0,
                static_cast<double>(work.top), static_cast<double>(work.bottom - height)));
            const RECT candidate{x, y, x + width, y + height};
            const RECT padded{x - gap, y - gap, candidate.right + gap, candidate.bottom + gap};
            if (std::none_of(placed.begin(), placed.end(), [&](const RECT& rect) { return Overlaps(rect, padded); })) {
                bounds[index] = candidate;
                placed.push_back(candidate);
                placedTag = true;
                break;
            }
        }
        if (!placedTag) return false;
    }
    return true;
}

} // namespace

std::size_t LayoutCaptionWindows(const RECT& work, std::span<const LONG> widths, LONG height,
                                 std::span<RECT> bounds, std::mt19937& random) {
    const LONG workWidth = work.right - work.left;
    const LONG workHeight = work.bottom - work.top;
    if (widths.empty() || workWidth <= 0 || workHeight <= 0 || height <= 0) return 0;

    height = std::min(height, workHeight);
    std::vector<std::size_t> order(widths.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::shuffle(order.begin(), order.end(), random);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return widths[a] > widths[b]; });
    if (PlaceCaptionCloud(work, widths, height, order, bounds, random)) return 0;

    // Pack the full work area more tightly when the cloud cannot fit all titles.
    const LONG rowCount = workHeight / height;
    const LONG top = work.top + (workHeight - rowCount * height) / 2;
    std::vector<LONG> rowTops;
    for (LONG row = 0; row < rowCount; ++row) rowTops.push_back(top + row * height);
    const LONG center = work.top + (workHeight - height) / 2;
    std::stable_sort(rowTops.begin(), rowTops.end(), [&](LONG a, LONG b) {
        return std::abs(a - center) < std::abs(b - center);
    });

    struct Row {
        LONG usedWidth = 0;
        std::vector<std::size_t> packed;
        std::vector<std::size_t> overflow;
    };
    std::vector<Row> rows(rowCount);
    std::multimap<LONG, std::size_t> available;
    for (std::size_t row = 0; row < rows.size(); ++row) available.emplace(workWidth, row);

    const auto windowWidth = [&](std::size_t index) { return std::clamp(widths[index], 1L, workWidth); };
    std::uniform_int_distribution<std::size_t> randomRow(0, rows.size() - 1);
    for (auto index : order) {
        const LONG width = windowWidth(index);
        const auto fit = available.lower_bound(width);
        if (fit == available.end()) {
            rows[randomRow(random)].overflow.push_back(index);
            continue;
        }
        const auto [remaining, row] = *fit;
        rows[row].packed.push_back(index);
        rows[row].usedWidth += width;
        available.erase(fit);
        available.emplace(remaining - width, row);
    }

    for (std::size_t row = 0; row < rows.size(); ++row) {
        auto& [usedWidth, packed, overflow] = rows[row];
        const LONG y = rowTops[row];
        std::shuffle(packed.begin(), packed.end(), random);
        LONG x = work.left + (workWidth - usedWidth) / 2;
        for (auto index : packed) {
            const LONG right = x + windowWidth(index);
            bounds[index] = {x, y, right, y + height};
            x = right;
        }
        for (auto index : overflow) {
            const LONG width = windowWidth(index);
            x = work.left + std::uniform_int_distribution<LONG>(0, workWidth - width)(random);
            bounds[index] = {x, y, x + width, y + height};
        }
    }
    return CountOverlappingWindows(bounds);
}

} // namespace tagsearch

#ifndef TAGSEARCH_CAPTION_LAYOUT_TEST_ONLY

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <cwchar>
#include <memory>
#include <optional>
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
constexpr UINT kOverlapNotice = WM_APP + 4;
constexpr UINT kSetTagMode = WM_APP + 5;
constexpr UINT kOverlapIcon = 1;
constexpr DWORD kTagStyle = WS_POPUP | WS_CAPTION;
constexpr DWORD kTagExStyle = WS_EX_TOOLWINDOW;
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
    std::vector<tagsearch::TagSearchTerm> clickTerms;
};

struct ScanResult {
    tagsearch::Folders folders;
    tagsearch::Tags tags;
    bool success = false;
};

struct Caption {
    HINSTANCE instance;
    HWND controller = nullptr;
    std::vector<std::unique_ptr<FloatingTag>> tags;
    std::mt19937 random{std::random_device{}()};
    tagsearch::Folders folders;
    std::size_t overlapCount = 0;
    bool overlapNoticeActive = false;
    ScanResult scan;
    std::jthread scanner;

    void DismissOverlapNotice() {
        if (!overlapNoticeActive) return;
        NOTIFYICONDATAW notice{sizeof(notice)};
        notice.hWnd = controller;
        notice.uID = kOverlapIcon;
        Shell_NotifyIconW(NIM_DELETE, &notice);
        KillTimer(controller, kOverlapIcon);
        overlapNoticeActive = false;
    }

    ~Caption() {
        DismissOverlapNotice();
        if (controller) DestroyWindow(controller);
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

void ApplyTagColor(HWND window, TagOperator operation) {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                              (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const COLORREF color = highContrast ? DWMWA_COLOR_DEFAULT : kTagModes[static_cast<std::size_t>(operation)].color;
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &color, sizeof(color));
}

void UpdateOverlapNotice(Caption& caption) {
    std::vector<RECT> visibleBounds;
    for (const auto& tag : caption.tags) {
        RECT rect{};
        if (IsWindowVisible(tag->window) && GetWindowRect(tag->window, &rect)) visibleBounds.push_back(rect);
    }
    const auto count = tagsearch::CountOverlappingWindows(visibleBounds);
    if (count == caption.overlapCount) return;

    caption.overlapCount = count;
    caption.DismissOverlapNotice();
    if (count == 0) return;
    NOTIFYICONDATAW notice{sizeof(notice)};
    notice.hWnd = caption.controller;
    notice.uID = kOverlapIcon;
    notice.uFlags = NIF_ICON | NIF_MESSAGE;
    notice.uCallbackMessage = kOverlapNotice;
    notice.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!Shell_NotifyIconW(NIM_ADD, &notice)) return;

    caption.overlapNoticeActive = true;
    notice.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &notice);
    notice.uFlags = NIF_INFO;
    notice.dwInfoFlags = NIIF_INFO;
    wcscpy_s(notice.szInfoTitle, L"TagSearch");
    const auto message = L"可能发生重叠的标签个数 「" + std::to_wstring(count) + L"」";
    wcscpy_s(notice.szInfo, message.c_str());
    if (!Shell_NotifyIconW(NIM_MODIFY, &notice) ||
        !SetTimer(caption.controller, kOverlapIcon, 60000, nullptr)) {
        caption.DismissOverlapNotice();
    }
}

bool PastDragThreshold(HWND window, const FloatingTag& tag, POINT point) {
    const UINT dpi = WindowDpi(window);
    return std::abs(point.x - tag.start.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
           std::abs(point.y - tag.start.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi);
}

std::vector<tagsearch::TagSearchTerm> StackedTagTerms(
    std::span<const tagsearch::TagWindowBounds> backToFront, std::size_t selected) {
    std::vector<bool> included(backToFront.size());
    std::vector<std::size_t> pending{selected};
    included[selected] = true;
    for (std::size_t next = 0; next < pending.size(); ++next) {
        const auto& rect = backToFront[pending[next]].bounds;
        for (std::size_t i = 0; i < backToFront.size(); ++i) {
            if (!included[i] && tagsearch::Overlaps(rect, backToFront[i].bounds)) {
                included[i] = true;
                pending.push_back(i);
            }
        }
    }

    std::vector<tagsearch::TagSearchTerm> terms;
    terms.reserve(pending.size());
    for (std::size_t i = 0; i < backToFront.size(); ++i) {
        if (included[i]) terms.push_back({std::wstring(backToFront[i].tag), backToFront[i].operation});
    }
    return terms;
}

std::vector<tagsearch::TagSearchTerm> TermsAtCurrentZOrder(const FloatingTag& tag) {
    std::vector<tagsearch::TagWindowBounds> windows;
    std::optional<std::size_t> selected;
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        RECT rect{};
        if (GetWindow(hwnd, GW_OWNER) != tag.caption.controller ||
            !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &rect)) continue;
        const auto* other = reinterpret_cast<const FloatingTag*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!other) continue;
        if (other == &tag) selected = windows.size();
        windows.push_back({rect, other->text, other->operation});
    }
    if (!selected) return {{tag.text, tag.operation}};
    std::reverse(windows.begin(), windows.end());
    return StackedTagTerms(windows, windows.size() - 1 - *selected);
}

void ActivateTag(HWND window, const FloatingTag& tag, std::vector<tagsearch::TagSearchTerm> terms) {
    if (terms.empty()) terms = TermsAtCurrentZOrder(tag);
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

LRESULT CALLBACK TagsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* tag = reinterpret_cast<FloatingTag*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!tag) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
    case WM_SYSCOLORCHANGE:
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ApplyTagColor(window, tag->operation);
        break;
    case WM_MOUSEACTIVATE:
        // Mouse activation can raise a lower window before the click is dispatched.
        if (LOWORD(lParam) == HTCAPTION &&
            (HIWORD(lParam) == WM_NCLBUTTONDOWN || HIWORD(lParam) == WM_LBUTTONDOWN)) {
            tag->clickTerms = TermsAtCurrentZOrder(*tag);
        }
        break;
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
                    SetWindowPos(window, HWND_TOP, x, y, 0, 0,
                                 SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOOWNERZORDER);
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
            const bool dragged = std::exchange(tag->dragging, false);
            auto terms = std::move(tag->clickTerms);
            tag->pressed = false;
            ReleaseCapture();
            if (clicked) ActivateTag(window, *tag, std::move(terms));
            if (dragged) UpdateOverlapNotice(tag->caption);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        tag->pressed = false;
        tag->dragging = false;
        tag->clickTerms.clear();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            if ((HIWORD(lParam) & KF_REPEAT) == 0) ActivateTag(window, *tag, {});
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            PostMessageW(tag->caption.controller, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_NCRBUTTONUP:
    case WM_CONTEXTMENU:
        ShowTagMenu(window, tag->operation, lParam);
        return 0;
    case kSetTagMode:
        if (wParam > 0 && wParam <= std::size(kTagModes)) {
            tag->operation = kTagModes[wParam - 1].operation;
            ApplyTagColor(window, tag->operation);
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
    tag->window = CreateWindowExW(kTagExStyle, kTagClass, text.c_str(), kTagStyle,
        work.left, work.top, 1, 1, caption.controller, nullptr, caption.instance, tag.get());
    return tag->window ? std::move(tag) : nullptr;
}

bool PositionTags(Caption& caption) {
    const RECT work = PrimaryWorkArea();
    if (IsRectEmpty(&work)) return false;
    SetWindowPos(caption.controller, nullptr, work.left, work.top, 0, 0,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    const UINT dpi = WindowDpi(caption.controller);
    RECT frame{};
    if (!AdjustWindowRectExForDpi(&frame, kTagStyle, FALSE, kTagExStyle, dpi) ||
        frame.bottom <= frame.top) return false;

    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    const HFONT font = SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)
        ? CreateFontIndirectW(&metrics.lfSmCaptionFont) : nullptr;
    const HDC dc = GetDC(caption.controller);
    const auto oldFont = dc && font ? SelectObject(dc, font) : nullptr;
    std::vector<LONG> widths;
    widths.reserve(caption.tags.size());
    constexpr int titlePadding = 6;
    const LONG frameWidth = frame.right - frame.left;
    for (const auto& tag : caption.tags) {
        SIZE text{};
        if (dc) GetTextExtentPoint32W(dc, tag->text.c_str(), static_cast<int>(tag->text.size()), &text);
        widths.push_back(text.cx + frameWidth + 2 * Scale(titlePadding, dpi));
    }
    if (dc) {
        if (oldFont) SelectObject(dc, oldFont);
        ReleaseDC(caption.controller, dc);
    }
    if (font) DeleteObject(font);

    std::vector<RECT> bounds(widths.size());
    const LONG height = frame.bottom - frame.top;
    tagsearch::LayoutCaptionWindows(work, widths, height, bounds, caption.random);
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const auto& rect = bounds[i];
        if (!SetWindowPos(caption.tags[i]->window, nullptr, rect.left, rect.top,
                          rect.right - rect.left, rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER)) {
            return false;
        }
    }
    return true;
}

void ActivateTags(Caption& caption) {
    for (const auto& tag : caption.tags) {
        ShowWindow(tag->window, IsIconic(tag->window) ? SW_RESTORE : SW_SHOWNA);
    }
    // Keep the stacking order when bringing overlapping tags to the foreground.
    std::vector<HWND> windows;
    for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
        if (GetWindow(window, GW_OWNER) == caption.controller && IsWindowVisible(window)) {
            windows.push_back(window);
        }
    }
    for (auto i = windows.rbegin(); i != windows.rend(); ++i) {
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
        auto tag = CreateTag(caption, text, work);
        if (!tag) {
            PostQuitMessage(1);
            return;
        }
        caption.tags.push_back(std::move(tag));
    }
    caption.scan = {};
    if (caption.tags.empty()) {
        PostQuitMessage(0);
        return;
    }
    if (!PositionTags(caption)) {
        PostQuitMessage(1);
        return;
    }
    ActivateTags(caption);
    UpdateOverlapNotice(caption);
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
        if (!caption->tags.empty() && PositionTags(*caption)) UpdateOverlapNotice(*caption);
        return 0;
    case kOverlapNotice:
        if (LOWORD(lParam) == NIN_BALLOONHIDE || LOWORD(lParam) == NIN_BALLOONTIMEOUT ||
            LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
            caption->DismissOverlapNotice();
        }
        return 0;
    case WM_TIMER:
        if (wParam != kOverlapIcon) break;
        caption->DismissOverlapNotice();
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
