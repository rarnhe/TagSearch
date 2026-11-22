#include "TagSearch_Core.h"
#include "TagSearch_Layout.h"

#include <propkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <structuredquery.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tagsearch {
std::optional<std::pair<std::filesystem::path, int>> ReadCaptionIconResource(const std::filesystem::path& folder);
std::span<const std::filesystem::path> CaptionNoticeIconFolders(
    std::span<const std::filesystem::path> folders, std::size_t defaultCount, bool hasFolderArguments);
HICON CreateCaptionNoticeTransportIcon();
void ConfigureCaptionNoticeIcon(NOTIFYICONDATAW& notice, HICON icon, HICON transportIcon);
HRESULT SaveCaptionShortcut(const std::filesystem::path& executable, const std::filesystem::path& shortcut,
                             const std::pair<std::filesystem::path, int>& icon);
}

namespace {

using TestFunction = std::function<void()>;

void Expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
void ExpectEqual(const T& actual, const T& expected, const char* message) {
    if (actual != expected) {
        throw std::runtime_error(message);
    }
}

void SplitKeywordsBasic() {
    ExpectEqual(
        tagsearch::SplitKeywords(L"alpha;beta;gamma"),
        std::vector<std::wstring>{L"alpha", L"beta", L"gamma"},
        "semicolon-separated keywords should retain their order");
}

void SplitKeywordsTrimsWhitespace() {
    ExpectEqual(
        tagsearch::SplitKeywords(L"  alpha ;\tbeta\t; gamma  "),
        std::vector<std::wstring>{L"alpha", L"beta", L"gamma"},
        "keywords should be trimmed using whitespace rules");
}

void SplitKeywordsIgnoresEmpty() {
    Expect(
        tagsearch::SplitKeywords(L"; ;\t;;").empty(),
        "empty and whitespace-only keywords should be ignored");
}

void NormalizeTagsDeduplicatesAndSorts() {
    ExpectEqual(
        tagsearch::NormalizeTags({L"beta", L"alpha", L"beta", L" alpha "}),
        std::vector<std::wstring>{L"alpha", L"beta"},
        "normalization should trim, deduplicate and sort tags");
}

void NormalizeTagsDiscardsEmpty() {
    Expect(
        tagsearch::NormalizeTags({L"", L" ", L"\t"}).empty(),
        "normalization should not retain empty tags");
}

namespace fs = std::filesystem;

struct TestDirectory {
    fs::path path = fs::temp_directory_path() /
        (L"TagSearch test " + std::to_wstring(GetCurrentProcessId()) + L" " +
         std::to_wstring(GetTickCount64()));

    TestDirectory() {
        fs::create_directory(path);
    }

    ~TestDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

const fs::path fixturePath = TAGSEARCH_FIXTURE;

void ResolveFoldersKeepsKnownFolders() {
    const auto defaultFolders = tagsearch::ResolveSearchFolders(L"TagSearch.exe");
    for (const auto* id : {&FOLDERID_Desktop, &FOLDERID_Downloads}) {
        PWSTR path = nullptr;
        const auto result = SHGetKnownFolderPath(*id, 0, nullptr, &path);
        const fs::path known = path ? fs::path(path) : fs::path();
        CoTaskMemFree(path);
        if (SUCCEEDED(result) && fs::is_directory(known)) {
            const auto folder = fs::canonical(known);
            Expect(std::find(defaultFolders.begin(), defaultFolders.end(), folder) != defaultFolders.end(),
                   "existing Windows known folders should be searched by default");
        }
    }

    TestDirectory test;
    const auto command = L"TagSearch.exe \"" + test.path.wstring() + L"\"";
    const auto combined = tagsearch::ResolveSearchFolders(command.c_str());
    Expect(combined.size() == defaultFolders.size() + 1 &&
               std::equal(defaultFolders.begin(), defaultFolders.end(), combined.begin()),
           "additional folders should extend the known folders");
}

void ResolveFoldersAcceptsQuotedPathsAndSkipsInvalidInputs() {
    TestDirectory test;
    const auto first = test.path / L"中文 & paths";
    const auto second = test.path / L"second";
    fs::create_directory(first);
    fs::create_directory(second);
    const auto file = test.path / L"not a directory.txt";
    std::ofstream(file).put('x');

    const auto command = L"\"C:\\Program Files\\TagSearch.exe\" \"" + first.wstring() +
        L"\" \"" + file.wstring() + L"\" \"" + (test.path / L"absent").wstring() +
        L"\" \"" + second.wstring() + L"\" \"" + (first / L".").wstring() + L"\" \"\"";
    const auto folders = tagsearch::ResolveSearchFolders(command.c_str());
    const auto defaults = tagsearch::ResolveSearchFolders(L"TagSearch.exe");
    ExpectEqual(folders.size(), defaults.size() + 2, "only valid distinct folders should be added");
    ExpectEqual(folders[folders.size() - 2], fs::canonical(first), "quoted Unicode path should survive");
    ExpectEqual(folders.back(), fs::canonical(second), "all command-line directories should be added");
}

void ScanTagsReadsNestedShellPropertiesWithoutUsingCache() {
    TestDirectory test;
    const auto nested = test.path / L"子目录" / L"nested";
    fs::create_directories(nested);
    const auto image = nested / L"label & image.jpg";
    fs::copy_file(fixturePath, image);
    fs::copy_file(fixturePath, test.path / L"duplicate.jpg");
    const auto fakeCache = test.path / L"desktop.ini";
    std::ofstream(fakeCache) << "Prop5=31,stale-cache-tag\r\n";

    ExpectEqual(
        tagsearch::ScanTags({test.path, nested}),
        tagsearch::NormalizeTags({L"alpha", L"项目 & 资料"}),
        "scan should read metadata from nested JPEGs, deduplicate tags and ignore the old cache");
    Expect(fs::exists(fakeCache), "reading live properties should leave existing files alone");

    fs::remove(test.path / L"duplicate.jpg");
    ExpectEqual(
        tagsearch::ScanTags({test.path}),
        tagsearch::NormalizeTags({L"alpha", L"项目 & 资料"}),
        "a root must reach the JPEG through nested folders");
    fs::remove(image);
    Expect(tagsearch::ScanTags({test.path}).empty(), "a new scan should reflect a removed file");
}

std::wstring IniValue(const fs::path& ini, const wchar_t* section, const wchar_t* key) {
    std::array<wchar_t, 512> text{};
    GetPrivateProfileStringW(section, key, L"", text.data(), static_cast<DWORD>(text.size()), ini.c_str());
    return text.data();
}

constexpr wchar_t summarySection[] = L"{F29F85E0-4FF9-1068-AB91-08002B27B3D9}";

void CaptionCachesRequestedFoldersIndependently() {
    TestDirectory test;
    const auto first = test.path / L"Folder A";
    const auto second = test.path / L"Folder B";
    fs::create_directories(first / L"nested");
    fs::create_directory(second);
    fs::copy_file(fixturePath, first / L"nested" / L"tags.jpg");
    fs::copy_file(fixturePath, first / L"duplicate.jpg");

    const auto firstIni = first / L"desktop.ini";
    const auto secondIni = second / L"desktop.ini";
    std::ofstream(firstIni) << "[.ShellClassInfo]\nIconResource=folder.ico,0\n"
                              "[{F29F85E0-4FF9-1068-AB91-08002B27B3D9}]\nProp2=31,Folder title\n"
                              "[Other]\nProp5=31,unrelated\n";

    const tagsearch::Folders requested{first, second};
    const tagsearch::Folders folders{test.path, first, second};
    const auto expected = tagsearch::NormalizeTags({L"alpha", L"项目 & 资料"});
    ExpectEqual(tagsearch::ScanCaptionTags(folders, requested), expected,
                "a cold scan should merge deduplicated tags from requested directories");
    const auto firstKeywords = IniValue(firstIni, summarySection, L"Prop5");
    Expect(firstKeywords.starts_with(L"31,"), "the first directory should store a summary value");
    ExpectEqual(tagsearch::NormalizeTags(tagsearch::SplitKeywords(std::wstring_view(firstKeywords).substr(3))),
                expected, "the first directory should store its own Unicode tag list");
    ExpectEqual(IniValue(secondIni, summarySection, L"Prop5"), std::wstring(L"31,"),
                "a directory without tags should store an empty cache");
    ExpectEqual(IniValue(firstIni, L".ShellClassInfo", L"IconResource"), std::wstring(L"folder.ico,0"),
                "writing the cache should preserve custom folder icons");
    ExpectEqual(IniValue(firstIni, summarySection, L"Prop2"), std::wstring(L"31,Folder title"),
                "writing the cache should preserve other summary values");
    ExpectEqual(IniValue(firstIni, L"Other", L"Prop5"), std::wstring(L"31,unrelated"),
                "only Prop5 in the summary section belongs to the cache");
    Expect(!fs::exists(test.path / L"desktop.ini"), "implicit roots should not get a cache");

    fs::remove_all(first / L"nested");
    fs::remove(first / L"duplicate.jpg");
    fs::copy_file(fixturePath, second / L"new.jpg");
    ExpectEqual(tagsearch::ScanCaptionTags(folders, requested), expected,
                "a warm run should reuse the stored snapshot after the original files are removed");

    fs::remove(firstIni);
    Expect(tagsearch::ScanCaptionTags(folders, requested).empty(),
           "an empty cache should skip newly added files even when an implicit root contains it");
}

void CaptionCombinesWarmAndColdNestedFolders() {
    TestDirectory test;
    const auto child = test.path / L"Child";
    fs::create_directory(child);
    const auto childIni = child / L"desktop.ini";
    std::ofstream(childIni) << "[{f29f85e0-4ff9-1068-ab91-08002b27b3d9}]\n"
                              "Prop5 = 31, cached ;cached; child\n";
    fs::copy_file(fixturePath, child / L"live-only.jpg");
    const auto expected = tagsearch::NormalizeTags({L"cached", L"child"});
    ExpectEqual(tagsearch::ScanCaptionTags({test.path, child}, {test.path, child}), expected,
                "a cold parent should reuse a requested child's valid cache without rescanning the child");
    const auto parentKeywords = IniValue(test.path / L"desktop.ini", summarySection, L"Prop5");
    Expect(parentKeywords.starts_with(L"31,"), "the parent should store its own summary value");
    ExpectEqual(tagsearch::NormalizeTags(tagsearch::SplitKeywords(std::wstring_view(parentKeywords).substr(3))),
                expected, "the parent snapshot should include the nested cached tags");
}

void CreateSearchFolderAcceptsMultipleScopesAndLiteralTag() {
    TestDirectory test;
    const auto first = test.path / L"one & 本地";
    const auto second = test.path / L"two";
    fs::create_directories(first);
    fs::create_directories(second);
    const std::wstring tag = L"项目 & \"归档\" + %";
    PIDLIST_ABSOLUTE id = nullptr;
    const auto result = tagsearch::CreateTagSearchFolder({tag}, {first, second}, &id);
    const bool created = SUCCEEDED(result) && id != nullptr;
    CoTaskMemFree(id);
    Expect(created, "Shell should create a search folder with both scopes and a literal tag");
}

void CreateSearchFolderRejectsEmptyScope() {
    PIDLIST_ABSOLUTE id = nullptr;
    Expect(tagsearch::CreateTagSearchFolder({L"example"}, {}, &id) == E_INVALIDARG && id == nullptr,
           "a missing scope must not turn into an unrestricted search");
}

void OverlapFindsAllDirectIntersections() {
    const tagsearch::TagWindowBounds selected{{-40, 0, 60, 40}, L"A"};
    const std::vector<tagsearch::TagWindowBounds> visible{
        selected,
        {{30, 0, 90, 40}, L"B", tagsearch::TagOperator::Or},
        {{70, 0, 110, 40}, L"chain-only"},
        {{-100, 10, -20, 30}, L"C", tagsearch::TagOperator::Not},
        {{30, 0, 90, 40}, L"B", tagsearch::TagOperator::Or},
    };
    ExpectEqual(tagsearch::TermsForOverlappingWindows(selected, visible),
                std::vector<tagsearch::TagSearchTerm>{{L"A"}, {L"B", tagsearch::TagOperator::Or}, {L"C", tagsearch::TagOperator::Not}},
                "each directly overlapping tag should occur once, including on negative coordinates");
}

void OverlapIgnoresTouchingEdgesAndCorners() {
    const tagsearch::TagWindowBounds selected{{0, 0, 100, 100}, L"A"};
    const std::vector<tagsearch::TagWindowBounds> visible{
        {{100, 0, 130, 50}, L"edge"},
        {{100, 100, 130, 130}, L"corner"},
        {{20, 0, 20, 80}, L"empty"},
        {{110, 0, 150, 50}, L"separate"},
    };
    ExpectEqual(tagsearch::TermsForOverlappingWindows(selected, visible), std::vector<tagsearch::TagSearchTerm>{{L"A"}},
                "the selected tag should remain a single-tag search when there is no common area");
}

std::wstring ExactKeyword(ICondition& condition) {
    LPWSTR property = nullptr;
    CONDITION_OPERATION operation{};
    PROPVARIANT value{};
    const auto hr = condition.GetComparisonInfo(&property, &operation, &value);
    LPWSTR keyword = nullptr;
    const auto converted = PropVariantToStringAlloc(value, &keyword);
    const bool exactKeyword = SUCCEEDED(hr) && SUCCEEDED(converted) &&
        property && std::wstring_view(property) == L"System.Keywords" &&
        operation == COP_EQUAL && keyword;
    const std::wstring result = keyword ? keyword : L"";
    CoTaskMemFree(keyword);
    CoTaskMemFree(property);
    PropVariantClear(&value);
    Expect(exactKeyword, "each leaf must be an exact System.Keywords comparison");
    return result;
}

void SearchConditionKeepsOneTagLiteral() {
    const std::wstring literal = L"A AND B";
    Microsoft::WRL::ComPtr<ICondition> condition;
    Expect(SUCCEEDED(tagsearch::CreateTagSearchCondition({literal}, condition.GetAddressOf())),
           "a single tag should create a Shell condition");
    CONDITION_TYPE type{};
    Expect(SUCCEEDED(condition->GetConditionType(&type)) && type == CT_LEAF_CONDITION,
           "a single tag must remain a single leaf");
    ExpectEqual(ExactKeyword(*condition.Get()), literal, "AND inside a label must remain part of the tag");
}

CONDITION_TYPE ConditionType(ICondition& condition) {
    CONDITION_TYPE type{};
    Expect(SUCCEEDED(condition.GetConditionType(&type)), "a condition should have a readable type");
    return type;
}

std::vector<Microsoft::WRL::ComPtr<ICondition>> Children(ICondition& condition) {
    Microsoft::WRL::ComPtr<IEnumUnknown> children;
    Expect(SUCCEEDED(condition.GetSubConditions(IID_PPV_ARGS(children.GetAddressOf()))),
           "a compound condition should expose its children");
    std::vector<Microsoft::WRL::ComPtr<ICondition>> result;
    for (;;) {
        Microsoft::WRL::ComPtr<IUnknown> child;
        ULONG fetched = 0;
        const auto hr = children->Next(1, child.GetAddressOf(), &fetched);
        if (hr == S_FALSE) break;
        Expect(hr == S_OK && fetched == 1, "the child condition should be readable");
        Expect(SUCCEEDED(child.As(&result.emplace_back())), "the child should implement ICondition");
    }
    return result;
}

void SearchConditionCombinesAllTagsWithAnd() {
    const tagsearch::TagWindowBounds selected{{0, 0, 4, 4}, L"项目 & \"资料\""};
    const std::array<tagsearch::TagWindowBounds, 3> candidates{{
        {{0, 0, 2, 2}, L"发布 AND 归档"},
        {{2, 2, 4, 4}, L"版本 100%"},
        {{5, 5, 6, 6}, L"outside"},
    }};
    const auto terms = tagsearch::TermsForOverlappingWindows(selected, candidates);
    Microsoft::WRL::ComPtr<ICondition> condition;
    Expect(SUCCEEDED(tagsearch::CreateTagQueryCondition(terms, condition.GetAddressOf())),
           "overlapping tags should create a Shell condition");
    Expect(ConditionType(*condition.Get()) == CT_AND_CONDITION, "all AND tags should use one AND node");

    tagsearch::Tags expected;
    for (const auto& term : terms) expected.push_back(term.tag);
    tagsearch::Tags actual;
    for (const auto& leaf : Children(*condition.Get())) actual.push_back(ExactKeyword(*leaf.Get()));
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    ExpectEqual(actual, expected, "the AND node should hold each tag as a literal keyword");
}

std::size_t CountCaptionIntersections(std::span<const RECT> windows, const RECT& work) {
    std::size_t count = 0;
    for (const auto& window : windows) {
        Expect(!IsRectEmpty(&window) && window.left >= work.left && window.top >= work.top &&
               window.right <= work.right && window.bottom <= work.bottom,
               "each tag must fit entirely within the work area");
        for (const auto& other : windows) {
            RECT overlap{};
            if (&other != &window && IntersectRect(&overlap, &window, &other)) {
                ++count;
                break;
            }
        }
    }
    return count;
}

void CaptionReadsDesktopIniIconResource() {
    TestDirectory test;
    {
        const std::wstring ini =
            L"\ufeff[.ShellClassInfo]\r\nIconResource=\"icons\\标记, light.ico\", -7\r\n"
            L"IconFile=ignored.ico\r\nIconIndex=1\r\n";
        std::ofstream file(test.path / L"desktop.ini", std::ios::binary);
        file.write(reinterpret_cast<const char*>(ini.data()), static_cast<std::streamsize>(ini.size() * sizeof(wchar_t)));
        Expect(file.good(), "the fixture should write a Unicode desktop.ini");
    }

    const auto icon = tagsearch::ReadCaptionIconResource(test.path);
    ExpectEqual(icon, std::optional{std::pair{test.path / L"icons" / L"标记, light.ico", -7}},
                "IconResource should retain a quoted relative path and signed index");
    Expect(!tagsearch::ReadCaptionIconResource(test.path / L"missing"),
           "a folder without desktop.ini should not supply an icon");
}

void CaptionReadsLegacyDesktopIniIcon() {
    TestDirectory test;
    {
        std::ofstream file(test.path / L"desktop.ini");
        file << "[.ShellClassInfo]\nIconFile=%SystemRoot%\\System32\\shell32.dll\nIconIndex=0\n";
        Expect(file.good(), "the fixture should write an ANSI desktop.ini");
    }

    const auto icon = tagsearch::ReadCaptionIconResource(test.path);
    if (!icon) throw std::runtime_error("IconFile and IconIndex should supply a resource");
    Expect(icon->first.is_absolute() && icon->first.filename() == L"shell32.dll" && icon->second == 0,
           "the legacy icon should expand Windows environment variables and retain its index");

    HICON handle = nullptr;
    ExtractIconExW(icon->first.c_str(), icon->second, nullptr, &handle, 1);
    const bool extracted = handle != nullptr;
    if (handle) DestroyIcon(handle);
    Expect(extracted, "the returned location should load as a Win32 notification icon");
}

void CaptionNoticeMatchesSenderIconOrHasNone() {
    const HICON icon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!icon) throw std::runtime_error("the test should load an icon");
    const HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) throw std::runtime_error("the test should create a hidden notification owner");

    const HICON transportIcon = tagsearch::CreateCaptionNoticeTransportIcon();
    if (!transportIcon) {
        DestroyWindow(owner);
        throw std::runtime_error("the test should create a transparent tray registration icon");
    }
    NOTIFYICONDATAW notice{sizeof(notice)};
    notice.hWnd = owner;
    notice.uFlags = NIF_MESSAGE;
    tagsearch::ConfigureCaptionNoticeIcon(notice, icon, transportIcon);
    const auto iconHandle = reinterpret_cast<LRESULT>(icon);
    const bool matched = (notice.uFlags & NIF_ICON) && notice.dwInfoFlags == NIIF_USER &&
        notice.hIcon == icon && notice.hBalloonIcon == icon &&
        SendMessageW(owner, WM_GETICON, ICON_SMALL, 0) == iconHandle &&
        SendMessageW(owner, WM_GETICON, ICON_BIG, 0) == iconHandle;

    tagsearch::ConfigureCaptionNoticeIcon(notice, nullptr, transportIcon);
    const bool removed = notice.uFlags == (NIF_MESSAGE | NIF_ICON) && notice.dwInfoFlags == NIIF_NONE &&
        notice.hIcon == transportIcon && !notice.hBalloonIcon &&
        SendMessageW(owner, WM_GETICON, ICON_SMALL, 0) == 0 &&
        SendMessageW(owner, WM_GETICON, ICON_BIG, 0) == 0;
    DestroyWindow(owner);
    DestroyIcon(transportIcon);

    Expect(matched, "the tray, notification and owner window should use the same icon");
    Expect(removed, "the transparent tray icon should keep text-only notices registered");
}

void CaptionSelectsNoticeIconFoldersByArguments() {
    const std::array<fs::path, 3> folders{L"Desktop", L"Downloads", L"Explicit"};
    const auto defaults = tagsearch::CaptionNoticeIconFolders(folders, 2, false);
    const auto explicitFolders = tagsearch::CaptionNoticeIconFolders(folders, 2, true);
    Expect(defaults.size() == 2 && defaults.front() == L"Desktop" && defaults.back() == L"Downloads",
           "launch without folder arguments should try the default folders");
    Expect(explicitFolders.size() == 1 && explicitFolders.front() == L"Explicit",
           "launch with folder arguments should try only non-default folders");
    Expect(tagsearch::CaptionNoticeIconFolders(std::span(folders).first(2), 2, true).empty(),
           "an explicit argument that adds no valid directory should still skip default icons");
}

void CaptionShortcutStoresIconAndAppId() {
    TestDirectory test;
    const auto executable = test.path / L"Caption.exe";
    const auto shortcut = test.path / L"Caption.lnk";
    const auto icon = std::pair{test.path / L"folder.ico", -9};
    std::ofstream(executable).put('\0');
    Expect(SUCCEEDED(tagsearch::SaveCaptionShortcut(executable, shortcut, icon)),
           "the program should write a shortcut for its own executable");
    const auto refreshed = tagsearch::SaveCaptionShortcut(executable, shortcut, icon);
    if (FAILED(refreshed)) {
        throw std::runtime_error("refreshing the program's own shortcut failed, HRESULT " +
                                 std::to_string(static_cast<std::uint32_t>(refreshed)));
    }

    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.GetAddressOf())))) {
        throw std::runtime_error("the test should open a shell link");
    }
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(shortcut.c_str(), STGM_READ))) {
        throw std::runtime_error("the saved shortcut should load");
    }

    std::array<wchar_t, MAX_PATH> savedIcon{};
    int index = 0;
    Expect(SUCCEEDED(link->GetIconLocation(savedIcon.data(), static_cast<int>(savedIcon.size()), &index)) &&
           fs::path(savedIcon.data()) == icon.first && index == icon.second,
           "the shortcut should retain the desktop.ini icon location and index");

    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(link.As(&properties))) throw std::runtime_error("the link should support properties");
    PROPVARIANT value{};
    const bool matches = SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &value)) &&
                         value.vt == VT_LPWSTR && value.pwszVal &&
                         std::wstring_view(value.pwszVal) == L"TagSearch.Caption";
    PropVariantClear(&value);
    Expect(matches, "the shortcut should retain Caption's AppUserModelID");
    Expect(FAILED(tagsearch::SaveCaptionShortcut(test.path / L"Different.exe", shortcut, icon)),
           "an existing link to a different executable should be left untouched");
}

void CaptionLayoutPacksVariableWidths() {
    const RECT work{-200, 30, 0, 80};
    const std::array<LONG, 6> widths{120, 120, 50, 50, 30, 30};
    std::array<RECT, widths.size()> bounds{};
    std::mt19937 random{1729};
    const auto count = tagsearch::LayoutCaptionWindows(work, widths, 25, bounds, random);
    ExpectEqual(count, std::size_t{0}, "two rows should exactly fit the six native caption widths");
    ExpectEqual(CountCaptionIntersections(bounds, work), count, "touching edges must not register as overlap");
    for (std::size_t i = 0; i < widths.size(); ++i) {
        ExpectEqual(bounds[i].right - bounds[i].left, widths[i], "each tag keeps its measured width");
    }
}

void CaptionLayoutKeepsAllCrowdedWindowsAndCountsOverlap() {
    const RECT work{-100, 30, 0, 50};
    const std::array<LONG, 6> widths{280, 230, 160, 60, 30, 120};
    std::array<RECT, widths.size()> bounds{};
    for (unsigned seed = 0; seed < 32; ++seed) {
        std::mt19937 random{seed};
        const auto count = tagsearch::LayoutCaptionWindows(work, widths, 40, bounds, random);
        ExpectEqual(count, widths.size(), "all tag windows intersect at least one other and should count once");
        ExpectEqual(CountCaptionIntersections(bounds, work), count, "oversize and overflow tags must stay in bounds");
    }

    const RECT twoRows{0, 0, 100, 40};
    const std::array<LONG, 3> wide{100, 100, 90};
    std::array<RECT, wide.size()> mixed{};
    std::mt19937 random{12};
    const auto count = tagsearch::LayoutCaptionWindows(twoRows, wide, 20, mixed, random);
    ExpectEqual(count, std::size_t{2}, "only the two windows in the crowded row should count");
    ExpectEqual(CountCaptionIntersections(mixed, twoRows), count, "the other row must stay disjoint");
}

void CaptionLayoutRandomizesPositions() {
    const RECT work{0, 0, 900, 560};
    const std::array<LONG, 5> widths{60, 100, 160, 100, 260};
    std::array<RECT, widths.size()> first{}, same{}, other{};
    std::mt19937 firstSeed{42}, sameSeed{42}, otherSeed{43};
    tagsearch::LayoutCaptionWindows(work, widths, 23, first, firstSeed);
    tagsearch::LayoutCaptionWindows(work, widths, 23, same, sameSeed);
    tagsearch::LayoutCaptionWindows(work, widths, 23, other, otherSeed);
    const auto equal = [](const RECT& a, const RECT& b) { return EqualRect(&a, &b) != FALSE; };
    Expect(std::equal(first.begin(), first.end(), same.begin(), same.end(), equal),
           "a fixed seed should reproduce the caption positions");
    Expect(!std::equal(first.begin(), first.end(), other.begin(), other.end(), equal),
           "different seeds should change the caption positions");
    ExpectEqual(CountCaptionIntersections(first, work), std::size_t{0}, "sparse titles should not overlap");
    ExpectEqual(CountCaptionIntersections(other, work), std::size_t{0}, "different placement should still fit");
}

void MixedTagQueryMatchesWindowsSearchOrder() {
    using tagsearch::TagOperator;
    const std::array<tagsearch::TagSearchTerm, 3> terms{{
        {L"tagA"}, {L"tagB", TagOperator::Or}, {L"tagC", TagOperator::Not},
    }};
    const std::array<tagsearch::TagWindowBounds, 3> overlapping{{
        {{0, 0, 12, 12}, L"tagC", TagOperator::Not},
        {{2, 2, 14, 14}, L"tagB", TagOperator::Or},
        {{4, 4, 16, 16}, L"tagA"},
    }};
    const std::vector<tagsearch::TagSearchTerm> expected(terms.begin(), terms.end());
    for (const auto& clicked : overlapping) {
        ExpectEqual(tagsearch::TermsForOverlappingWindows(clicked, overlapping), expected,
                    "clicking any window in the same group should retain AND, OR, and NOT precedence");
    }
    ExpectEqual(tagsearch::FormatTagQuery(terms), std::wstring(L"AND tagA OR tagB NOT tagC"),
                "the query string should show the selected operator for each tag");
    Microsoft::WRL::ComPtr<ICondition> condition;
    Expect(SUCCEEDED(tagsearch::CreateTagQueryCondition(terms, condition.GetAddressOf())),
           "a mixture of tag modes should create a Shell condition");
    Expect(ConditionType(*condition.Get()) == CT_AND_CONDITION, "NOT should join the preceding OR group with AND");
    const auto children = Children(*condition.Get());
    ExpectEqual(children.size(), std::size_t{2}, "the AND root should hold an OR and a NOT");
    const auto firstIsOr = ConditionType(*children[0].Get()) == CT_OR_CONDITION;
    auto& either = *children[firstIsOr ? 0 : 1].Get();
    auto& excluded = *children[firstIsOr ? 1 : 0].Get();
    Expect(ConditionType(either) == CT_OR_CONDITION && ConditionType(excluded) == CT_NOT_CONDITION,
           "the Shell tree should implement (tagA OR tagB) AND NOT tagC");

    tagsearch::Tags allowed;
    for (const auto& child : Children(either)) allowed.push_back(ExactKeyword(*child.Get()));
    std::sort(allowed.begin(), allowed.end());
    ExpectEqual(allowed, tagsearch::Tags{L"tagA", L"tagB"}, "the OR should contain the two positive keywords");

    Microsoft::WRL::ComPtr<ICondition> negative;
    Expect(SUCCEEDED(excluded.GetSubConditions(IID_PPV_ARGS(negative.GetAddressOf()))),
           "NOT should expose the exact keyword it excludes");
    ExpectEqual(ExactKeyword(*negative.Get()), std::wstring(L"tagC"), "the NOT should exclude tagC");

    TestDirectory test;
    PIDLIST_ABSOLUTE id = nullptr;
    const auto hr = tagsearch::CreateTagQueryFolder(terms, {test.path}, &id);
    const bool created = SUCCEEDED(hr) && id != nullptr;
    CoTaskMemFree(id);
    Expect(created, "Explorer should accept a scoped search folder for the mixed condition");
}

void CreateSearchFolderAcceptsCombinedTags() {
    TestDirectory test;
    PIDLIST_ABSOLUTE id = nullptr;
    const auto result = tagsearch::CreateTagSearchFolder({L"A", L"B"}, {test.path}, &id);
    const bool created = SUCCEEDED(result) && id != nullptr;
    CoTaskMemFree(id);
    Expect(created, "the Windows Shell should create a search folder with multiple tag conditions");
}

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComApartment() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, TestFunction>> tests{
        {"split_keywords_basic", SplitKeywordsBasic},
        {"split_keywords_trims_whitespace", SplitKeywordsTrimsWhitespace},
        {"split_keywords_ignores_empty", SplitKeywordsIgnoresEmpty},
        {"normalize_tags_deduplicates_and_sorts", NormalizeTagsDeduplicatesAndSorts},
        {"normalize_tags_discards_empty", NormalizeTagsDiscardsEmpty},
        {"resolve_folders_keeps_known_folders", ResolveFoldersKeepsKnownFolders},
        {"resolve_folders_accepts_quoted_paths_and_skips_invalid_inputs", ResolveFoldersAcceptsQuotedPathsAndSkipsInvalidInputs},
        {"scan_tags_reads_nested_shell_properties_without_using_cache", ScanTagsReadsNestedShellPropertiesWithoutUsingCache},
        {"caption_caches_requested_folders_independently", CaptionCachesRequestedFoldersIndependently},
        {"caption_combines_warm_and_cold_nested_folders", CaptionCombinesWarmAndColdNestedFolders},
        {"create_search_folder_accepts_multiple_scopes_and_literal_tag", CreateSearchFolderAcceptsMultipleScopesAndLiteralTag},
        {"create_search_folder_rejects_empty_scope", CreateSearchFolderRejectsEmptyScope},
        {"overlap_finds_all_direct_intersections", OverlapFindsAllDirectIntersections},
        {"overlap_ignores_touching_edges_and_corners", OverlapIgnoresTouchingEdgesAndCorners},
        {"search_condition_keeps_one_tag_literal", SearchConditionKeepsOneTagLiteral},
        {"search_condition_combines_all_tags_with_and", SearchConditionCombinesAllTagsWithAnd},
        {"create_search_folder_accepts_combined_tags", CreateSearchFolderAcceptsCombinedTags},
        {"caption_reads_desktop_ini_icon_resource", CaptionReadsDesktopIniIconResource},
        {"caption_reads_legacy_desktop_ini_icon", CaptionReadsLegacyDesktopIniIcon},
        {"caption_notice_matches_sender_icon_or_has_none", CaptionNoticeMatchesSenderIconOrHasNone},
        {"caption_selects_notice_icon_folders_by_arguments", CaptionSelectsNoticeIconFoldersByArguments},
        {"caption_shortcut_stores_icon_and_app_id", CaptionShortcutStoresIconAndAppId},
        {"caption_layout_packs_variable_widths", CaptionLayoutPacksVariableWidths},
        {"caption_layout_keeps_all_crowded_windows_and_counts_overlap", CaptionLayoutKeepsAllCrowdedWindowsAndCountsOverlap},
        {"caption_layout_randomizes_positions", CaptionLayoutRandomizesPositions},
        {"mixed_tag_query_matches_windows_search_order", MixedTagQueryMatchesWindowsSearchOrder},
    };

    if (argc != 2) {
        std::cerr << "Usage: tagsearch_tests <test-case>\n";
        return 2;
    }

    for (const auto& [name, test] : tests) {
        if (name != argv[1]) {
            continue;
        }

        try {
            const ComApartment com;
            Expect(SUCCEEDED(com.result), "COM initialization should succeed");
            test();
            std::cout << "[PASS] " << name << '\n';
            return 0;
        }
        catch (const std::exception& error) {
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
            return 1;
        }
        catch (...) {
            std::cerr << "[FAIL] " << name << ": unknown exception\n";
            return 1;
        }
    }

    std::cerr << "Unknown test case: " << argv[1] << '\n';
    return 2;
}
