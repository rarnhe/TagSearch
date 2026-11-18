#include "TagSearchCore.h"

#include <algorithm>
#include <cwctype>
#include <locale>
#include <set>
#include <utility>

namespace tagsearch {
namespace {

constexpr std::wstring_view kDesktopIniTagsPrefix = L"Prop5=31,";

std::wstring TrimWhitespace(std::wstring_view value) {
    std::size_t first = 0;
    while (first < value.size() &&
           std::iswspace(static_cast<std::wint_t>(value[first])) != 0) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first &&
           std::iswspace(static_cast<std::wint_t>(value[last - 1])) != 0) {
        --last;
    }

    return std::wstring(value.substr(first, last - first));
}

} // namespace

std::vector<std::wstring> SplitKeywords(std::wstring_view value) {
    std::vector<std::wstring> tags;
    std::size_t start = 0;

    while (start <= value.size()) {
        const std::size_t separator = value.find(L';', start);
        const std::size_t end =
            separator == std::wstring_view::npos ? value.size() : separator;

        std::wstring tag = TrimWhitespace(value.substr(start, end - start));
        if (!tag.empty()) {
            tags.push_back(std::move(tag));
        }

        if (separator == std::wstring_view::npos) {
            break;
        }
        start = separator + 1;
    }

    return tags;
}

std::vector<std::wstring> NormalizeTags(std::vector<std::wstring> tags) {
    std::set<std::wstring> distinct;
    for (const auto& tag : tags) {
        std::wstring trimmed = TrimWhitespace(tag);
        if (!trimmed.empty()) {
            distinct.insert(std::move(trimmed));
        }
    }

    std::vector<std::wstring> normalized(distinct.begin(), distinct.end());

    try {
        const std::locale userLocale("");
        const auto& collate = std::use_facet<std::collate<wchar_t>>(userLocale);
        std::sort(
            normalized.begin(),
            normalized.end(),
            [&collate](const std::wstring& left, const std::wstring& right) {
                return collate.compare(
                    left.data(), left.data() + left.size(),
                    right.data(), right.data() + right.size()) < 0;
            });
    }
    catch (...) {
        std::sort(normalized.begin(), normalized.end());
    }

    return normalized;
}

std::optional<std::vector<std::wstring>> ParseDesktopIniTagsLine(std::wstring_view line) {
    const std::size_t prefix = line.find(kDesktopIniTagsPrefix);
    if (prefix == std::wstring_view::npos) {
        return std::nullopt;
    }

    return SplitKeywords(line.substr(prefix + kDesktopIniTagsPrefix.size()));
}

std::wstring FormatDesktopIniTagsLine(const std::vector<std::wstring>& tags) {
    std::wstring line(kDesktopIniTagsPrefix);
    bool first = true;

    for (const auto& rawTag : tags) {
        const std::wstring tag = TrimWhitespace(rawTag);
        if (tag.empty()) {
            continue;
        }

        if (!first) {
            line.push_back(L';');
        }
        line += tag;
        first = false;
    }

    return line;
}

std::wstring UpdateDesktopIniTagsLine(
    std::wstring_view content,
    const std::vector<std::wstring>& tags) {
    const std::wstring replacement = FormatDesktopIniTagsLine(tags);
    const std::wstring newline =
        content.find(L"\r\n") != std::wstring_view::npos ? L"\r\n" : L"\n";

    std::wstring updated;
    bool replaced = false;
    std::size_t cursor = 0;

    while (cursor < content.size()) {
        const std::size_t lineEnd = content.find_first_of(L"\r\n", cursor);
        const std::size_t textEnd =
            lineEnd == std::wstring_view::npos ? content.size() : lineEnd;
        const std::wstring_view line = content.substr(cursor, textEnd - cursor);

        if (!replaced && line.find(kDesktopIniTagsPrefix) != std::wstring_view::npos) {
            updated += replacement;
            replaced = true;
        }
        else {
            updated.append(line);
        }

        if (lineEnd == std::wstring_view::npos) {
            cursor = content.size();
            break;
        }

        if (content[lineEnd] == L'\r' &&
            lineEnd + 1 < content.size() &&
            content[lineEnd + 1] == L'\n') {
            updated += L"\r\n";
            cursor = lineEnd + 2;
        }
        else {
            updated.push_back(content[lineEnd]);
            cursor = lineEnd + 1;
        }
    }

    if (!replaced) {
        if (!updated.empty() && updated.back() != L'\n' && updated.back() != L'\r') {
            updated += newline;
        }
        updated += replacement;
        updated += newline;
    }

    return updated;
}

} // namespace tagsearch
