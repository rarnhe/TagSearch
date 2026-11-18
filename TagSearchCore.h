#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tagsearch {

std::vector<std::wstring> SplitKeywords(std::wstring_view value);
std::vector<std::wstring> NormalizeTags(std::vector<std::wstring> tags);
std::optional<std::vector<std::wstring>> ParseDesktopIniTagsLine(std::wstring_view line);
std::wstring FormatDesktopIniTagsLine(const std::vector<std::wstring>& tags);
std::wstring UpdateDesktopIniTagsLine(
    std::wstring_view content,
    const std::vector<std::wstring>& tags);

} // namespace tagsearch
