#pragma once

#include <windows.h>
#include <shtypes.h>

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

struct ICondition;

namespace tagsearch {

using Folders = std::vector<std::filesystem::path>;
using Tags = std::vector<std::wstring>;

Tags SplitKeywords(std::wstring_view value);
Tags NormalizeTags(Tags tags);

struct TagWindowBounds {
    RECT bounds;
    std::wstring_view tag;
};

Tags TagsForOverlappingWindows(
    const TagWindowBounds& selected, const std::vector<TagWindowBounds>& visibleWindows);

// Pass a complete Unicode command line, including argv[0].
Folders ResolveSearchFolders(const wchar_t* commandLine, std::size_t* defaultCount = nullptr);

// Call Shell operations from a thread with COM initialized.
Tags ScanTags(const Folders& folders, std::stop_token stop = {});
HRESULT CreateTagSearchCondition(const Tags& tags, ICondition** result);
HRESULT CreateTagSearchFolder(const Tags& tags, const Folders& folders, PIDLIST_ABSOLUTE* result);
bool OpenTagSearch(HWND owner, const Tags& tags, const Folders& folders);

} // namespace tagsearch
