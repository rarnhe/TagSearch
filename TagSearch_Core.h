#pragma once

#include <windows.h>
#include <shtypes.h>

#include <cstddef>
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

Tags SplitKeywords(std::wstring_view value);
Tags NormalizeTags(Tags tags);

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
// The first defaultCount roots are scanned live; remaining roots may use desktop.ini snapshots.
Tags ScanTagsWithDesktopIniCache(const Folders& folders, std::size_t defaultCount, std::stop_token stop = {});
std::optional<IconResource> ReadDesktopIniIcon(const std::filesystem::path& folder);
HRESULT CreateTagSearchCondition(const Tags& tags, ICondition** result);
HRESULT CreateTagSearchFolder(const Tags& tags, const Folders& folders, PIDLIST_ABSOLUTE* result);
HRESULT CreateTagQueryCondition(std::span<const TagSearchTerm> terms, ICondition** result);
HRESULT CreateTagQueryFolder(std::span<const TagSearchTerm> terms, const Folders& folders, PIDLIST_ABSOLUTE* result);
bool OpenTagQuery(HWND owner, std::span<const TagSearchTerm> terms, const Folders& folders);
bool OpenTagSearch(HWND owner, const Tags& tags, const Folders& folders);

} // namespace tagsearch
