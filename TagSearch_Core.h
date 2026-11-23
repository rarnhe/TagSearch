#pragma once

#include <windows.h>
#include <shtypes.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ICondition;

namespace tagsearch {

using Folders = std::vector<std::filesystem::path>;
using Tags = std::vector<std::wstring>;
using IconResource = std::pair<std::filesystem::path, int>;

struct ScanStatistics {
    std::uint64_t enumeratedFiles = 0;
    std::uint64_t propertyStoreCalls = 0;
    std::uint64_t skippedDirectories = 0;
    std::uint64_t elapsedMilliseconds = 0;
    std::uint64_t cachedRoots = 0;
    std::uint64_t scannedRoots = 0;
};

// Applies to first-time desktop.ini scans; extension checks are case-insensitive.
bool ShouldScanTagFile(const std::filesystem::path& path);
bool ShouldSkipTagDirectory(const std::filesystem::path& path);

Tags SplitKeywords(std::wstring_view value);
// Stable, first-seen deduplication; never reorder a folder's labels.
Tags NormalizeTags(Tags tags);
COLORREF WindowAccentColor();

enum class TagOperator { And, Or, Not };

struct TagSearchTerm {
    std::wstring tag;
    TagOperator operation = TagOperator::And;

    bool operator==(const TagSearchTerm&) const = default;
};

struct TagWindowBounds {
    RECT bounds;
    std::wstring_view tag;
    TagOperator operation = TagOperator::And;
};

std::vector<TagSearchTerm> TermsForOverlappingWindows(
    const TagWindowBounds& selected, std::span<const TagWindowBounds> visibleWindows);
std::wstring FormatTagQuery(std::span<const TagSearchTerm> terms);

// Pass a complete Unicode command line, including argv[0].
Folders ResolveSearchFolders(const wchar_t* commandLine, std::size_t* defaultCount = nullptr);

// Call Shell operations from a thread with COM initialized.
Tags ScanTags(const Folders& folders, std::stop_token stop = {});
// All roots, including default folders, use their own desktop.ini snapshots when valid.
// New default-folder files contain only the summary section; explicit folders use the full template.
// Callers supply Desktop and Downloads before their additional command-line roots.
bool EnableFolderDesktopIniCustomization(const std::filesystem::path& folder);
Tags ScanTagsWithDesktopIniCache(const Folders& folders, std::size_t defaultCount,
                                 std::stop_token stop = {}, ScanStatistics* statistics = nullptr);
std::optional<IconResource> ReadDesktopIniIcon(const std::filesystem::path& folder);
HRESULT CreateTagSearchCondition(const Tags& tags, ICondition** result);
HRESULT CreateTagSearchFolder(const Tags& tags, const Folders& folders, PIDLIST_ABSOLUTE* result);
HRESULT CreateTagQueryCondition(std::span<const TagSearchTerm> terms, ICondition** result);
HRESULT CreateTagQueryFolder(std::span<const TagSearchTerm> terms, const Folders& folders, PIDLIST_ABSOLUTE* result);
bool OpenTagQuery(HWND owner, std::span<const TagSearchTerm> terms, const Folders& folders);
bool OpenTagSearch(HWND owner, const Tags& tags, const Folders& folders);

} // namespace tagsearch
