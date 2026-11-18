#pragma once

#include <windows.h>
#include <shtypes.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tagsearch {

using Folders = std::vector<std::filesystem::path>;
using Tags = std::vector<std::wstring>;

Tags SplitKeywords(std::wstring_view value);
Tags NormalizeTags(Tags tags);

// Pass a complete Unicode command line, including argv[0].
Folders ResolveSearchFolders(const wchar_t* commandLine);

// Call Shell operations from a thread with COM initialized.
Tags ScanTags(const Folders& folders);
HRESULT CreateTagSearchFolder(
    const std::wstring& tag, const Folders& folders, PIDLIST_ABSOLUTE* result);
bool OpenTagSearch(HWND owner, const std::wstring& tag, const Folders& folders);

} // namespace tagsearch
