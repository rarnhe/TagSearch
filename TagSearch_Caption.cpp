#include "TagSearch_Core.h"

#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>
#include <random>
#include <utility>
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

std::span<const std::filesystem::path> CaptionNoticeIconFolders(
    std::span<const std::filesystem::path> folders, std::size_t defaultCount, bool hasFolderArguments) {
    return hasFolderArguments ? folders.subspan(defaultCount) : folders.first(defaultCount);
}

HICON CreateCaptionNoticeTransportIcon() {
    constexpr int size = 16;
    using Mask = std::array<BYTE, size * size / 8>;
    Mask andMask{};
    andMask.fill(0xff);
    const Mask xorMask{};
    return CreateIcon(GetModuleHandleW(nullptr), size, size, 1, 1, andMask.data(), xorMask.data());
}

void ConfigureCaptionNoticeIcon(NOTIFYICONDATAW& notice, HICON icon, HICON transportIcon) {
    // Explorer needs a tray entry to send the balloon, even for NIIF_NONE.
    notice.hIcon = icon ? icon : transportIcon;
    notice.hBalloonIcon = icon;
    notice.dwInfoFlags = icon ? NIIF_USER : NIIF_NONE;
    if (notice.hIcon) notice.uFlags |= NIF_ICON;
    else notice.uFlags &= ~NIF_ICON;

    const auto iconHandle = reinterpret_cast<LPARAM>(icon);
    SendMessageW(notice.hWnd, WM_SETICON, ICON_SMALL, iconHandle);
    SendMessageW(notice.hWnd, WM_SETICON, ICON_BIG, iconHandle);
}

namespace {
constexpr wchar_t kCaptionAppId[] = L"TagSearch.Caption";
}

HRESULT SaveCaptionShortcut(const std::filesystem::path& executable, const std::filesystem::path& shortcut,
                             const IconResource& icon) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(link.GetAddressOf()));
    if (FAILED(hr)) return hr;

    ComPtr<IPersistFile> file;
    hr = link.As(&file);
    if (FAILED(hr)) return hr;

    std::error_code error;
    const bool existing = std::filesystem::exists(shortcut, error);
    if (error) return E_FAIL;
    if (existing) {
        std::wstring target(32768, L'\0');
        hr = file->Load(shortcut.c_str(), STGM_READWRITE);
        if (FAILED(hr)) return hr;
        hr = link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, 0);
        if (FAILED(hr)) return hr;
        if (!std::filesystem::equivalent(target.c_str(), executable, error)) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
    }

    hr = link->SetPath(executable.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(executable.parent_path().c_str());
    if (SUCCEEDED(hr)) hr = link->SetIconLocation(icon.first.c_str(), icon.second);
    if (FAILED(hr)) return hr;

    ComPtr<IPropertyStore> properties;
    hr = link.As(&properties);
    if (FAILED(hr)) return hr;
    PROPVARIANT appId{};
    hr = InitPropVariantFromString(kCaptionAppId, &appId);
    if (SUCCEEDED(hr)) hr = properties->SetValue(PKEY_AppUserModel_ID, appId);
    PropVariantClear(&appId);
    if (SUCCEEDED(hr)) hr = properties->Commit();
    if (SUCCEEDED(hr)) hr = file->Save(shortcut.c_str(), TRUE);
    return hr;
}

} // namespace tagsearch

#ifndef TAGSEARCH_CAPTION_LAYOUT_TEST_ONLY

#include <dwmapi.h>
#include <windowsx.h>
#include <future>
#include <iterator>
#include <memory>
#include <system_error>
#include <thread>
#include <type_traits>

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
constexpr UINT kNoticeEvent = WM_APP + 4;
constexpr UINT kSetTagMode = WM_APP + 5;
constexpr UINT kScanStarted = WM_APP + 6;
constexpr std::size_t kNoticeBodyLimit = std::extent_v<decltype(NOTIFYICONDATAW::szInfo)> - 1;
constexpr DWORD kTagStyle = WS_POPUP | WS_CAPTION;
constexpr DWORD kTagExStyle = WS_EX_TOOLWINDOW;
constexpr int kTitlePadding = 6;
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
    bool moved = false;
    POINT start{};
    POINT last{};
    std::vector<tagsearch::TagSearchTerm> clickTerms;
};

struct ScanResult {
    tagsearch::Folders folders;
    tagsearch::Tags tags;
    bool success = false;
};

using tagsearch::IconResource;
using OwnedFont = std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)>;
using OwnedIcon = std::unique_ptr<std::remove_pointer_t<HICON>, decltype(&DestroyIcon)>;
using NoticeIcons = std::map<IconResource, OwnedIcon>;

struct ScanNoticeData {
    tagsearch::Folders folders;
    NoticeIcons icons;
};

OwnedIcon ExtractCaptionIcon(const IconResource& resource) {
    HICON icon = nullptr;
    ExtractIconExW(resource.first.c_str(), resource.second, nullptr, &icon, 1);
    return OwnedIcon(icon, &DestroyIcon);
}

void RegisterCaptionShortcut() {
    std::wstring executableName(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executableName.data(), static_cast<DWORD>(executableName.size()));
    if (!length || length >= executableName.size()) return;
    executableName.resize(length);
    const std::filesystem::path executable(executableName);
    const auto resource = tagsearch::ReadDesktopIniIcon(executable.parent_path());
    if (!resource || !ExtractCaptionIcon(*resource)) return;

    PWSTR programs = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> programsPath(programs, &CoTaskMemFree);
    if (FAILED(result) || !programsPath) return;

    auto shortcut = std::filesystem::path(programsPath.get()) / executable.filename();
    shortcut.replace_extension(L".lnk");
    if (SUCCEEDED(tagsearch::SaveCaptionShortcut(executable, shortcut, *resource))) {
        SetCurrentProcessExplicitAppUserModelID(tagsearch::kCaptionAppId);
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, shortcut.c_str(), nullptr);
    }
}

NoticeIcons LoadNoticeIcons(std::span<const std::filesystem::path> folders, std::stop_token stop) {
    NoticeIcons icons;
    for (const auto& folder : folders) {
        if (stop.stop_requested()) break;
        auto resource = tagsearch::ReadDesktopIniIcon(folder);
        if (!resource || icons.contains(*resource)) continue;

        auto icon = ExtractCaptionIcon(*resource);
        if (icon) icons.emplace(std::move(*resource), std::move(icon));
    }
    return icons;
}

struct Caption {
    HINSTANCE instance;
    HWND controller = nullptr;
    OwnedFont titleFont{nullptr, &DeleteObject};
    OwnedFont boldTitleFont{nullptr, &DeleteObject};
    std::vector<std::unique_ptr<FloatingTag>> tags;
    std::mt19937 random{std::random_device{}()};
    tagsearch::Folders folders;
    UINT scanIcon = 0;
    UINT overlapIcon = 0;
    UINT lastNoticeIcon = 0;
    bool exitAfterNotices = false;
    ScanResult scan;
    NoticeIcons noticeIcons;
    OwnedIcon noticeTransportIcon{tagsearch::CreateCaptionNoticeTransportIcon(), &DestroyIcon};
    std::future<ScanNoticeData> scanNotice;
    std::jthread scanner;

    void DismissNotice(UINT& icon) {
        if (!icon) return;
        NOTIFYICONDATAW notice{sizeof(notice)};
        notice.hWnd = controller;
        notice.uID = std::exchange(icon, 0);
        Shell_NotifyIconW(NIM_DELETE, &notice);
        KillTimer(controller, notice.uID);
    }

    void QuitIfNoNotices() const {
        if (exitAfterNotices && !scanIcon && !overlapIcon) PostQuitMessage(0);
    }

    bool FinishNotice(UINT icon) {
        if (!icon) return false;
        if (icon == scanIcon) DismissNotice(scanIcon);
        else if (icon == overlapIcon) DismissNotice(overlapIcon);
        else return false;
        QuitIfNoNotices();
        return true;
    }

    ~Caption() {
        DismissNotice(scanIcon);
        DismissNotice(overlapIcon);
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

HFONT CreateCaptionFont(UINT dpi, bool bold) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) return nullptr;
    metrics.lfSmCaptionFont.lfWeight = bold ? FW_BOLD : FW_NORMAL;
    return CreateFontIndirectW(&metrics.lfSmCaptionFont);
}

void PaintCaption(HWND window, HDC dc, const FloatingTag& tag) {
    RECT caption{};
    if (!GetClientRect(window, &caption) || IsRectEmpty(&caption)) return;

    const COLORREF background = kTagModes[static_cast<std::size_t>(tag.operation)].color;
    SetBkColor(dc, background);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &caption, L"", 0, nullptr);

    const auto& font = tag.pressed ? tag.caption.boldTitleFont : tag.caption.titleFont;
    const auto previousFont = font ? SelectObject(dc, font.get()) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    const int padding = Scale(kTitlePadding, WindowDpi(window));
    caption.left += padding;
    caption.right -= padding;
    DrawTextW(dc, tag.text.c_str(), -1, &caption,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (previousFont) SelectObject(dc, previousFont);
}

void RedrawCaption(HWND window) {
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

std::vector<tagsearch::TagWindowBounds> VisibleTagsInZOrder(const Caption& caption) {
    std::vector<tagsearch::TagWindowBounds> windows;
    windows.reserve(caption.tags.size());
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        RECT rect{};
        if (GetWindow(hwnd, GW_OWNER) != caption.controller ||
            !IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &rect)) continue;
        const auto* tag = reinterpret_cast<const FloatingTag*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (tag) windows.push_back({rect, tag->text, tag->operation});
    }
    std::reverse(windows.begin(), windows.end());
    return windows;
}

using TagGroup = std::vector<std::size_t>;

TagGroup CollectOverlapGroup(std::span<const tagsearch::TagWindowBounds> windows, std::size_t selected,
                             std::vector<bool>& visited) {
    TagGroup group{selected};
    visited[selected] = true;
    for (std::size_t next = 0; next < group.size(); ++next) {
        const auto& rect = windows[group[next]].bounds;
        for (std::size_t i = 0; i < windows.size(); ++i) {
            if (!visited[i] && tagsearch::Overlaps(rect, windows[i].bounds)) {
                visited[i] = true;
                group.push_back(i);
            }
        }
    }
    std::sort(group.begin(), group.end());
    return group;
}

std::vector<TagGroup> OverlapGroups(std::span<const tagsearch::TagWindowBounds> windows) {
    std::vector<TagGroup> groups;
    std::vector<bool> visited(windows.size());
    for (std::size_t i = 0; i < windows.size(); ++i) {
        if (visited[i]) continue;
        auto group = CollectOverlapGroup(windows, i, visited);
        if (group.size() > 1) groups.push_back(std::move(group));
    }
    return groups;
}

template <typename FitsLine>
bool AppendNoticeLine(std::wstring& message, std::wstring line, std::size_t limit, const FitsLine& fitsLine) {
    constexpr std::wstring_view ellipsis = L"...";
    const std::size_t separator = message.empty() ? 0 : 1;
    const std::size_t available = limit - message.size();
    if (available < separator + ellipsis.size()) return false;

    std::replace_if(line.begin(), line.end(), [](wchar_t ch) {
        return ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == 0x2028 || ch == 0x2029;
    }, L' ');

    const auto lineLimit = available - separator;
    if (line.size() > lineLimit || !fitsLine(line)) {
        line.resize(std::min(line.size(), lineLimit - ellipsis.size()));
        for (;;) {
            if (!line.empty() && ((line.back() >= 0xd800 && line.back() <= 0xdbff) || line.back() == L'·')) {
                line.pop_back();
                continue;
            }
            line += ellipsis;
            if (line.size() == ellipsis.size() || fitsLine(line)) break;
            line.resize(line.size() - ellipsis.size() - 1);
        }
    }

    if (separator) message += L'\n';
    message += line;
    return true;
}

template <typename FitsLine>
std::wstring FormatOverlapLines(std::span<const tagsearch::TagWindowBounds> windows,
                                std::span<const TagGroup> groups, std::size_t limit, const FitsLine& fitsLine) {
    std::wstring message;
    for (const auto& group : groups) {
        std::wstring line;
        for (const auto index : group) {
            if (!line.empty()) line += L'·';
            line += windows[index].tag;
        }
        if (!AppendNoticeLine(message, std::move(line), limit, fitsLine)) break;
    }
    return message;
}

template <typename FormatBody>
std::wstring MeasureNoticeLines(HWND window, const FormatBody& formatBody) {
    const UINT dpi = WindowDpi(window);
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    const HFONT font = SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)
        ? CreateFontIndirectW(&metrics.lfMessageFont) : nullptr;
    const HDC dc = GetDC(window);
    const auto original = dc && font ? SelectObject(dc, font) : nullptr;
    // Leave room for the Shell's icon and margins.
    const int lineWidth = Scale(220, dpi);
    const auto message = formatBody([&](std::wstring_view line) {
        SIZE text{};
        if (dc && GetTextExtentPoint32W(dc, line.data(), static_cast<int>(line.size()), &text)) {
            return text.cx <= lineWidth;
        }
        return line.size() <= 24;
    });
    if (dc) {
        if (original) SelectObject(dc, original);
        ReleaseDC(window, dc);
    }
    if (font) DeleteObject(font);
    return message;
}

std::wstring OverlapNoticeBody(HWND window, std::span<const tagsearch::TagWindowBounds> windows,
                               std::span<const TagGroup> groups, std::size_t limit) {
    return MeasureNoticeLines(window, [&](const auto& fitsLine) {
        return FormatOverlapLines(windows, groups, limit, fitsLine);
    });
}

void ShowNotice(Caption& caption, UINT& icon, const std::wstring& title, std::wstring body) {
    caption.DismissNotice(icon);
    if (body.size() > kNoticeBodyLimit) {
        body.resize(kNoticeBodyLimit - 3);
        if (body.back() >= 0xd800 && body.back() <= 0xdbff) body.pop_back();
        body += L"...";
    }

    NOTIFYICONDATAW notice{sizeof(notice)};
    notice.hWnd = caption.controller;
    // A queued callback from an older balloon must not dismiss a newer one.
    caption.lastNoticeIcon = caption.lastNoticeIcon % 0xffff + 1;
    notice.uID = caption.lastNoticeIcon;
    notice.uFlags = NIF_MESSAGE;
    notice.uCallbackMessage = kNoticeEvent;
    HICON iconForNotice = nullptr;
    if (!caption.noticeIcons.empty()) {
        const auto offset = std::uniform_int_distribution<std::size_t>(0, caption.noticeIcons.size() - 1)(caption.random);
        iconForNotice = std::next(caption.noticeIcons.begin(), static_cast<std::ptrdiff_t>(offset))->second.get();
    }
    tagsearch::ConfigureCaptionNoticeIcon(notice, iconForNotice, caption.noticeTransportIcon.get());
    if (!Shell_NotifyIconW(NIM_ADD, &notice)) return;

    icon = notice.uID;
    notice.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &notice);
    notice.uFlags = NIF_INFO;
    wcscpy_s(notice.szInfoTitle, title.c_str());
    wcscpy_s(notice.szInfo, body.c_str());
    if (!Shell_NotifyIconW(NIM_MODIFY, &notice) ||
        !SetTimer(caption.controller, notice.uID, 60000, nullptr)) {
        caption.DismissNotice(icon);
    }
}

void UpdateOverlapNotice(Caption& caption, bool initialLayout = false) {
    const auto windows = VisibleTagsInZOrder(caption);
    const auto groups = OverlapGroups(windows);
    if (groups.empty() && !initialLayout) {
        caption.DismissNotice(caption.overlapIcon);
        return;
    }

    const auto title = L"重叠标签组 「" + std::to_wstring(groups.size()) + L"」";
    auto body = groups.empty()
        ? L"统计标记数量 「" + std::to_wstring(caption.tags.size()) + L"」"
        : OverlapNoticeBody(caption.controller, windows, groups, kNoticeBodyLimit);
    ShowNotice(caption, caption.overlapIcon, title, std::move(body));
}

tagsearch::Folders ScanNoticeArguments(const wchar_t* commandLine) {
    int count = 0;
    const std::unique_ptr<LPWSTR, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(commandLine, &count), &LocalFree);
    tagsearch::Folders folders;
    for (int i = 1; arguments && i < count; ++i) {
        const std::filesystem::path input(arguments.get()[i]);
        if (input.empty()) continue;

        std::error_code error;
        auto absolute = std::filesystem::absolute(input, error);
        auto folder = (error ? input : absolute).lexically_normal();
        folder.make_preferred();
        if (!folder.has_filename() && folder.has_relative_path()) folder = folder.parent_path();
        folders.push_back(std::move(folder));
    }
    return folders;
}

template <typename FitsLine>
std::wstring FormatFolderNoticeLines(std::span<const std::filesystem::path> folders,
                                     std::size_t limit, const FitsLine& fitsLine) {
    std::wstring message;
    for (const auto& folder : folders) {
        const auto name = folder.filename();
        auto line = (name.empty() ? folder : name).wstring() + L'-' + folder.wstring();
        if (!AppendNoticeLine(message, std::move(line), limit, fitsLine)) break;
    }
    return message;
}

std::wstring ScanNoticeBody(HWND window, const tagsearch::Folders& folders) {
    if (folders.empty()) return L"没有可读取的默认目录";
    return MeasureNoticeLines(window, [&](const auto& fitsLine) {
        return FormatFolderNoticeLines(folders, kNoticeBodyLimit, fitsLine);
    });
}

bool PastDragThreshold(HWND window, const FloatingTag& tag, POINT point) {
    const UINT dpi = WindowDpi(window);
    return std::abs(point.x - tag.start.x) >= GetSystemMetricsForDpi(SM_CXDRAG, dpi) ||
           std::abs(point.y - tag.start.y) >= GetSystemMetricsForDpi(SM_CYDRAG, dpi);
}

std::vector<tagsearch::TagSearchTerm> TermsAtCurrentZOrder(const FloatingTag& tag) {
    const auto windows = VisibleTagsInZOrder(tag.caption);
    const auto selected = std::find_if(windows.begin(), windows.end(), [&](const auto& window) {
        return window.tag == tag.text;
    });
    if (selected == windows.end()) return {{tag.text, tag.operation}};

    std::vector<bool> visited(windows.size());
    const auto group = CollectOverlapGroup(windows, static_cast<std::size_t>(selected - windows.begin()), visited);
    std::vector<tagsearch::TagSearchTerm> terms;
    terms.reserve(group.size());
    for (const auto index : group) terms.push_back({std::wstring(windows[index].tag), windows[index].operation});
    return terms;
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
    case WM_CREATE: {
        // Keep a DWM frame around the title, whose colors and font are painted in the client area.
        constexpr MARGINS frame{1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(window, &frame);
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wParam) return 0;
        break;
    case WM_NCHITTEST: {
        const auto hit = DefWindowProcW(window, message, wParam, lParam);
        return hit == HTCLIENT ? HTCAPTION : hit;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        if (dc) PaintCaption(window, dc, *tag);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (wParam) PaintCaption(window, reinterpret_cast<HDC>(wParam), *tag);
        return 0;
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
        if (!tag->pressed && GetCursorPos(&tag->start)) {
            tag->last = tag->start;
            tag->dragging = false;
            tag->moved = false;
            SetCapture(window);
            tag->pressed = GetCapture() == window;
            if (tag->pressed) RedrawCaption(window);
            else tag->clickTerms.clear();
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
                    if ((x != bounds.left || y != bounds.top) &&
                        SetWindowPos(window, HWND_TOP, x, y, 0, 0,
                                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOOWNERZORDER)) {
                        tag->moved = true;
                    }
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
            const bool moved = std::exchange(tag->moved, false);
            auto terms = std::move(tag->clickTerms);
            tag->pressed = false;
            ReleaseCapture();
            RedrawCaption(window);
            if (clicked) ActivateTag(window, *tag, std::move(terms));
            if (moved) UpdateOverlapNotice(tag->caption);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED: {
        const bool wasPressed = std::exchange(tag->pressed, false);
        tag->dragging = false;
        tag->moved = false;
        tag->clickTerms.clear();
        if (wasPressed) RedrawCaption(window);
        return 0;
    }
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
            RedrawCaption(window);
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

bool PositionTags(Caption& caption, bool firstShow = false) {
    const RECT work = PrimaryWorkArea();
    if (IsRectEmpty(&work)) return false;
    SetWindowPos(caption.controller, nullptr, work.left, work.top, 0, 0,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
    const UINT dpi = WindowDpi(caption.controller);
    RECT frame{};
    if (!AdjustWindowRectExForDpi(&frame, kTagStyle, FALSE, kTagExStyle, dpi) ||
        frame.bottom <= frame.top) return false;

    caption.titleFont.reset(CreateCaptionFont(dpi, false));
    caption.boldTitleFont.reset(CreateCaptionFont(dpi, true));
    const HFONT font = caption.titleFont.get();
    const HFONT bold = caption.boldTitleFont.get();
    const HDC dc = GetDC(caption.controller);
    const auto oldFont = dc && font ? SelectObject(dc, font) : nullptr;
    std::vector<LONG> widths;
    widths.reserve(caption.tags.size());
    const LONG frameWidth = frame.right - frame.left;
    for (const auto& tag : caption.tags) {
        SIZE text{};
        if (dc) GetTextExtentPoint32W(dc, tag->text.c_str(), static_cast<int>(tag->text.size()), &text);
        widths.push_back(text.cx);
    }
    if (dc && bold) {
        const auto previousFont = SelectObject(dc, bold);
        for (std::size_t i = 0; i < caption.tags.size(); ++i) {
            const auto& text = caption.tags[i]->text;
            SIZE extent{};
            if (GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent)) {
                widths[i] = std::max(widths[i], extent.cx);
            }
        }
        if (previousFont) SelectObject(dc, previousFont);
    }
    const LONG chrome = frameWidth + 2 * Scale(kTitlePadding, dpi);
    for (auto& width : widths) width += chrome;
    if (dc) {
        if (oldFont) SelectObject(dc, oldFont);
        ReleaseDC(caption.controller, dc);
    }

    std::vector<RECT> bounds(widths.size());
    const LONG height = frame.bottom - frame.top;
    tagsearch::LayoutCaptionWindows(work, widths, height, bounds, caption.random);
    // On first display, place and show each tag without requesting foreground activation.
    const UINT flags = SWP_NOACTIVATE | (firstShow ? SWP_SHOWWINDOW | SWP_NOOWNERZORDER : SWP_NOZORDER);
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        const auto& rect = bounds[i];
        if (!SetWindowPos(caption.tags[i]->window, firstShow ? HWND_TOP : nullptr, rect.left, rect.top,
                          rect.right - rect.left, rect.bottom - rect.top, flags)) {
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

void ScanInBackground(Caption& caption, std::promise<ScanNoticeData>& notice, std::stop_token stop) {
    auto& scan = caption.scan;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        try {
            const auto commandLine = GetCommandLineW();
            std::size_t defaultCount = 0;
            scan.folders = tagsearch::ResolveSearchFolders(commandLine, &defaultCount);
            if (!stop.stop_requested()) {
                const std::span<const std::filesystem::path> folders = scan.folders;
                auto displayFolders = ScanNoticeArguments(commandLine);
                const bool hasFolderArguments = !displayFolders.empty();
                const auto iconFolders = tagsearch::CaptionNoticeIconFolders(folders, defaultCount, hasFolderArguments);
                if (!hasFolderArguments) displayFolders.assign(iconFolders.begin(), iconFolders.end());
                notice.set_value({std::move(displayFolders), LoadNoticeIcons(iconFolders, stop)});
                PostMessageW(caption.controller, kScanStarted, 0, 0);
                scan.tags = tagsearch::ScanTagsWithDesktopIniCache(scan.folders, defaultCount, stop);
                scan.success = !stop.stop_requested();
            }
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
        std::promise<ScanNoticeData> notice;
        caption.scanNotice = notice.get_future();
        caption.scanner = std::jthread([&caption, notice = std::move(notice)](std::stop_token stop) mutable {
            ScanInBackground(caption, notice, stop);
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
    if (!caption.tags.empty() && !PositionTags(caption, true)) {
        PostQuitMessage(1);
        return;
    }
    UpdateOverlapNotice(caption, true);
    caption.exitAfterNotices = caption.tags.empty();
    caption.QuitIfNoNotices();
}

LRESULT CALLBACK ControllerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* caption = reinterpret_cast<Caption*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!caption) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case kScanStarted:
        if (caption->scanNotice.valid()) {
            auto notice = caption->scanNotice.get();
            caption->noticeIcons = std::move(notice.icons);
            ShowNotice(*caption, caption->scanIcon, L"正在读取标签…", ScanNoticeBody(window, notice.folders));
        }
        return 0;
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
        if (!caption->tags.empty()) PositionTags(*caption);
        return 0;
    case kNoticeEvent:
        if (LOWORD(lParam) == NIN_BALLOONHIDE || LOWORD(lParam) == NIN_BALLOONTIMEOUT ||
            LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
            caption->FinishNotice(HIWORD(lParam));
        }
        return 0;
    case WM_TIMER:
        if (caption->FinishNotice(static_cast<UINT>(wParam))) return 0;
        break;
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
        RegisterCaptionShortcut();
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
