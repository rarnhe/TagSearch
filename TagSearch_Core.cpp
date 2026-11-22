#include "TagSearch_Core.h"

#include <dwmapi.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <structuredquery.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <type_traits>
#include <set>
#include <utility>

namespace tagsearch {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

template<typename T>
using TaskMemory = std::unique_ptr<T, decltype(&CoTaskMemFree)>;

using Pidl = TaskMemory<std::remove_pointer_t<PIDLIST_ABSOLUTE>>;

void AddFolder(Folders& folders, const fs::path& candidate) {
    if (candidate.empty()) {
        return;
    }

    std::error_code error;
    auto folder = fs::canonical(candidate, error);
    if (!error && fs::is_directory(folder, error) &&
        std::find(folders.begin(), folders.end(), folder) == folders.end()) {
        folders.push_back(std::move(folder));
    }
}

void AddKnownFolder(Folders& folders, REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, 0, nullptr, &path);
    const TaskMemory<wchar_t> memory(path, &CoTaskMemFree);
    if (SUCCEEDED(hr)) {
        AddFolder(folders, path);
    }
}

void ReadItemTags(const fs::path& item, Tags& tags) {
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(
            item.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(store.GetAddressOf())))) {
        return;
    }

    PROPVARIANT value{};
    if (SUCCEEDED(store->GetValue(PKEY_Keywords, &value))) {
        if (value.vt == (VT_VECTOR | VT_LPWSTR)) {
            for (ULONG i = 0; i < value.calpwstr.cElems; ++i) {
                if (const auto keyword = value.calpwstr.pElems[i]) {
                    tags.emplace_back(keyword);
                }
            }
        } else if (value.vt == VT_LPWSTR && value.pwszVal) {
            const auto keywords = SplitKeywords(value.pwszVal);
            tags.insert(tags.end(), keywords.begin(), keywords.end());
        }
    }
    PropVariantClear(&value);
}

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

Tags SplitKeywords(std::wstring_view value) {
    Tags tags;
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

COLORREF WindowAccentColor() {
    DWORD argb = 0;
    BOOL opaque = FALSE;
    if (FAILED(DwmGetColorizationColor(&argb, &opaque))) return GetSysColor(COLOR_HIGHLIGHT);
    return RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
}

Tags NormalizeTags(Tags tags) {
    std::set<std::wstring> distinct;
    for (const auto& tag : tags) {
        std::wstring trimmed = TrimWhitespace(tag);
        if (!trimmed.empty()) {
            distinct.insert(std::move(trimmed));
        }
    }

    Tags normalized(distinct.begin(), distinct.end());

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

Folders ResolveSearchFolders(const wchar_t* commandLine, std::size_t* defaultCount) {
    Folders folders;
    AddKnownFolder(folders, FOLDERID_Desktop);
    AddKnownFolder(folders, FOLDERID_Downloads);
    if (defaultCount) *defaultCount = folders.size();

    int count = 0;
    const std::unique_ptr<LPWSTR, decltype(&LocalFree)> arguments(
        CommandLineToArgvW(commandLine, &count), &LocalFree);
    for (int i = 1; arguments && i < count; ++i) {
        AddFolder(folders, arguments.get()[i]);
    }
    return folders;
}

namespace {

struct FolderSnapshot {
    Tags tags;
    bool complete;
};

using FolderSnapshots = std::map<fs::path, FolderSnapshot>;

struct DesktopIni {
    bool exists;
    std::wstring text;
};

std::optional<DesktopIni> ReadDesktopIni(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return DesktopIni{false, {}};
        return std::nullopt;
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return std::nullopt;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size < 0 || size > INT_MAX) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(size)) ||
        file.peek() != std::char_traits<char>::eof() || file.bad()) return std::nullopt;

    std::wstring text;
    const unsigned int marker = bytes.size() >= 2
        ? (static_cast<unsigned char>(bytes[0]) << 8) | static_cast<unsigned char>(bytes[1])
        : 0;
    if (marker == 0xfffe || marker == 0xfeff) {
        if ((bytes.size() - 2) % sizeof(wchar_t)) return std::nullopt;
        text.resize((bytes.size() - 2) / sizeof(wchar_t));
        if (marker == 0xfffe) {
            std::memcpy(text.data(), bytes.data() + 2, bytes.size() - 2);
        } else {
            for (std::size_t i = 0; i < text.size(); ++i) {
                const auto high = static_cast<unsigned char>(bytes[2 + i * 2]);
                const auto low = static_cast<unsigned char>(bytes[3 + i * 2]);
                text[i] = static_cast<wchar_t>((high << 8) | low);
            }
        }
    } else if (!bytes.empty()) {
        const bool utf8 = bytes.starts_with("\xef\xbb\xbf");
        const int offset = utf8 ? 3 : 0;
        const int length = static_cast<int>(bytes.size()) - offset;
        if (length) {
            const UINT codePage = utf8 ? CP_UTF8 : CP_ACP;
            const DWORD flags = utf8 ? MB_ERR_INVALID_CHARS : 0;
            const int required = MultiByteToWideChar(codePage, flags, bytes.data() + offset, length, nullptr, 0);
            if (!required) return std::nullopt;
            text.resize(required);
            if (!MultiByteToWideChar(codePage, flags, bytes.data() + offset, length, text.data(), required)) {
                return std::nullopt;
            }
        }
    }
    if (text.find(L'\0') != std::wstring::npos) return std::nullopt;
    return DesktopIni{true, std::move(text)};
}

constexpr wchar_t kDesktopIniName[] = L"desktop.ini";
constexpr std::wstring_view kShellClassInfo = L"[.ShellClassInfo]";
constexpr std::wstring_view kSummarySection = L"[{F29F85E0-4FF9-1068-AB91-08002B27B3D9}]";

bool EqualsIniName(std::wstring_view value, std::wstring_view name) {
    return value.size() == name.size() &&
        CompareStringOrdinal(value.data(), static_cast<int>(value.size()),
                             name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
}

struct IniEntry {
    std::size_t position = std::wstring_view::npos;
    std::size_t length = 0;
    std::wstring_view value;
};

IniEntry FindIniEntry(std::wstring_view text, std::wstring_view section, std::wstring_view key) {
    bool inSection = false;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find_first_of(L"\r\n", start);
        if (end == std::wstring_view::npos) end = text.size();
        const auto line = TrimWhitespace(text.substr(start, end - start));

        if (!line.empty() && line.front() == L'[' && line.back() == L']') {
            if (inSection) return {start};
            inSection = EqualsIniName(line, section);
        } else if (inSection) {
            const auto rawLine = text.substr(start, end - start);
            const auto separator = rawLine.find(L'=');
            if (separator != std::wstring_view::npos &&
                EqualsIniName(TrimWhitespace(rawLine.substr(0, separator)), key)) {
                return {start, end - start, rawLine.substr(separator + 1)};
            }
        }

        start = end + 1;
        if (end < text.size() && text[end] == L'\r' && start < text.size() && text[start] == L'\n') ++start;
    }
    return {inSection ? text.size() : std::wstring_view::npos};
}

std::wstring_view UnquoteIniValue(std::wstring_view value) {
    if (value.size() >= 2 && value.front() == value.back() &&
        (value.front() == L'"' || value.front() == L'\'')) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::optional<int> ParseIconIndex(std::wstring_view text) {
    const auto trimmed = TrimWhitespace(text);
    const std::wstring value(UnquoteIniValue(trimmed));
    wchar_t* end = nullptr;
    const auto index = std::wcstoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end || index < INT_MIN || index > INT_MAX) return std::nullopt;
    return static_cast<int>(index);
}

std::optional<Tags> ReadCachedTags(const DesktopIni& ini, const IniEntry& entry) {
    const auto value = TrimWhitespace(entry.value);
    if (!ini.exists || !value.starts_with(L"31,")) return std::nullopt;
    return NormalizeTags(SplitKeywords(std::wstring_view(value).substr(3)));
}

void SaveCachedTags(const fs::path& path, const DesktopIni& original, const IniEntry& entry,
                    const Tags& tags, std::stop_token stop) {
    // The Windows Prop5 list has no escape syntax for semicolons or line breaks.
    constexpr std::wstring_view unsupported(L";\r\n\0", 4);
    constexpr std::wstring_view prefix = L"Prop5=31,";
    std::wstring line(prefix);
    for (const auto& tag : tags) {
        if (tag.find_first_of(unsupported) != std::wstring::npos) return;
        if (line.size() > prefix.size()) line += L';';
        line += tag;
    }

    auto text = original.text;
    if (text.empty()) {
        text = kShellClassInfo;
        text += L"\r\nInfoTip=\r\nIconResource=\r\n";
    }
    auto position = entry.position;
    if (position == std::wstring_view::npos) {
        if (!text.empty() && text.back() != L'\n' && text.back() != L'\r') text += L"\r\n";
        text += kSummarySection;
        text += L"\r\n";
        position = text.size();
    }
    if (entry.length) {
        text.replace(position, entry.length, line);
    } else {
        if (position && text[position - 1] != L'\n' && text[position - 1] != L'\r') line.insert(0, L"\r\n");
        text.insert(position, line + L"\r\n");
    }
    text.insert(text.begin(), L'\ufeff');
    if (text.size() > MAXDWORD / sizeof(wchar_t) || stop.stop_requested()) return;

    GUID id{};
    std::array<wchar_t, 39> token{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, token.data(), static_cast<int>(token.size()))) return;
    const auto temp = path.parent_path() / (std::wstring(L".TagSearch-") + token.data() + L".tmp");
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const auto length = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    DWORD written = 0;
    const bool ready = WriteFile(file, text.data(), length, &written, nullptr) &&
        written == length && FlushFileBuffers(file);
    CloseHandle(file);

    // If another program edited desktop.ini during the scan, leave that version in place.
    if (ready && !stop.stop_requested()) {
        const auto current = ReadDesktopIni(path);
        if (current && current->exists == original.exists && current->text == original.text) {
            if (original.exists) {
                const DWORD attributes = GetFileAttributesW(path.c_str());
                constexpr DWORD preserved = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (!(attributes & preserved) || SetFileAttributesW(temp.c_str(), attributes & preserved))) {
                    auto backup = temp;
                    backup += L".bak";
                    if (ReplaceFileW(path.c_str(), temp.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
                        DeleteFileW(backup.c_str());
                    } else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
                               GetLastError() == ERROR_FILE_NOT_FOUND) {
                        MoveFileExW(backup.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
                    }
                }
            } else {
                MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
            }
        }
    }
    DeleteFileW(temp.c_str());
}

Tags ScanTagsWithSnapshots(const Folders& folders, const FolderSnapshots& snapshots,
                           std::stop_token stop, bool* complete = nullptr) {
    Tags tags;
    Folders pending = folders;
    std::set<fs::path> visited;
    while (!pending.empty() && !stop.stop_requested()) {
        auto folder = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(folder).second) {
            continue;
        }

        if (const auto cached = snapshots.find(folder); cached != snapshots.end()) {
            tags.insert(tags.end(), cached->second.tags.begin(), cached->second.tags.end());
            if (complete && !cached->second.complete) *complete = false;
            continue;
        }

        ReadItemTags(folder, tags);
        std::error_code error;
        const auto options = complete ? fs::directory_options::none : fs::directory_options::skip_permission_denied;
        for (fs::directory_iterator entry(folder, options, error), end;
             !error && entry != end && !stop.stop_requested(); entry.increment(error)) {
            const auto& path = entry->path();
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                if (complete) *complete = false;
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                ReadItemTags(path, tags);
                continue;
            }

            // Cloud placeholders may have a reparse tag. Skip name surrogates
            // (junctions and directory symlinks) so traversal stays in scope.
            WIN32_FIND_DATAW data{};
            const HANDLE find = (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                ? FindFirstFileW(path.c_str(), &data) : INVALID_HANDLE_VALUE;
            if (find != INVALID_HANDLE_VALUE) {
                FindClose(find);
                constexpr DWORD nameSurrogateBit = 0x20000000;
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    (data.dwReserved0 & nameSurrogateBit)) {
                    ReadItemTags(path, tags);
                    continue;
                }
            }
            pending.push_back(path);
        }
        if (complete && error) *complete = false;
    }
    return stop.stop_requested() ? Tags{} : NormalizeTags(std::move(tags));
}

} // namespace

std::optional<IconResource> ReadDesktopIniIcon(const fs::path& folder) {
    const auto ini = ReadDesktopIni(folder / kDesktopIniName);
    if (!ini || !ini->exists) return std::nullopt;

    auto value = TrimWhitespace(FindIniEntry(ini->text, kShellClassInfo, L"IconResource").value);
    int index = 0;
    if (UnquoteIniValue(value).empty()) {
        value = TrimWhitespace(FindIniEntry(ini->text, kShellClassInfo, L"IconFile").value);
        index = ParseIconIndex(FindIniEntry(ini->text, kShellClassInfo, L"IconIndex").value).value_or(0);
    } else if (const auto comma = value.rfind(L','); comma != std::wstring::npos) {
        if (const auto parsed = ParseIconIndex(std::wstring_view(value).substr(comma + 1))) {
            index = *parsed;
            value = TrimWhitespace(std::wstring_view(value).substr(0, comma));
        }
    }
    const std::wstring input(UnquoteIniValue(value));
    if (input.empty()) return std::nullopt;

    const DWORD capacity = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (!capacity || capacity > 32768) return std::nullopt;
    std::wstring expanded(capacity, L'\0');
    const DWORD length = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), capacity);
    if (!length || length > capacity) return std::nullopt;
    expanded.resize(length - 1);

    return IconResource{(folder / expanded).lexically_normal(), index};
}

Tags ScanTags(const Folders& folders, std::stop_token stop) {
    return ScanTagsWithSnapshots(folders, {}, stop);
}

Tags ScanTagsWithDesktopIniCache(const Folders& folders, std::size_t defaultCount, std::stop_token stop) {
    const auto requestedFolders = std::span(folders).subspan(defaultCount);
    Folders ordered(requestedFolders.begin(), requestedFolders.end());
    // Resolve children first so a parent scan can reuse any explicitly requested child snapshot.
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.native().size() > right.native().size();
    });

    FolderSnapshots snapshots;
    for (const auto& folder : ordered) {
        if (stop.stop_requested()) return {};
        const auto path = folder / kDesktopIniName;
        const auto original = ReadDesktopIni(path);
        IniEntry entry;
        if (original) {
            entry = FindIniEntry(original->text, kSummarySection, L"Prop5");
            if (auto cached = ReadCachedTags(*original, entry)) {
                snapshots.emplace(folder, FolderSnapshot{std::move(*cached), true});
                continue;
            }
        }
        bool complete = true;
        auto tags = ScanTagsWithSnapshots({folder}, snapshots, stop, &complete);
        if (original && complete && !stop.stop_requested()) {
            SaveCachedTags(path, *original, entry, tags, stop);
        }
        snapshots.emplace(folder, FolderSnapshot{std::move(tags), complete});
    }
    return ScanTagsWithSnapshots(folders, snapshots, stop);
}

std::vector<TagSearchTerm> TermsForOverlappingWindows(
    const TagWindowBounds& selected, std::span<const TagWindowBounds> visibleWindows) {
    std::vector<TagSearchTerm> terms{{std::wstring(selected.tag), selected.operation}};
    for (const auto& candidate : visibleWindows) {
        RECT intersection{};
        if (IntersectRect(&intersection, &selected.bounds, &candidate.bounds) &&
            std::none_of(terms.begin(), terms.end(), [&](const auto& term) { return term.tag == candidate.tag; })) {
            terms.push_back({std::wstring(candidate.tag), candidate.operation});
        }
    }
    // A tag's mode should have the same effect whichever window is clicked
    // when the overlap selects the same group.
    std::stable_sort(terms.begin(), terms.end(), [](const auto& left, const auto& right) {
        return left.operation < right.operation;
    });
    return terms;
}

std::wstring FormatTagQuery(std::span<const TagSearchTerm> terms) {
    std::wstring text;
    for (const auto& term : terms) {
        if (!text.empty()) text += L' ';
        switch (term.operation) {
        case TagOperator::And: text += L"AND "; break;
        case TagOperator::Or: text += L"OR "; break;
        case TagOperator::Not: text += L"NOT "; break;
        }
        text += term.tag;
    }
    return text;
}

namespace {

std::vector<TagSearchTerm> AndTerms(const Tags& tags) {
    std::vector<TagSearchTerm> terms;
    terms.reserve(tags.size());
    for (const auto& tag : tags) terms.push_back({tag});
    return terms;
}

HRESULT CombineTerms(IConditionFactory2& factory, CONDITION_TYPE type,
                     const std::vector<ComPtr<ICondition>>& terms, ICondition** result) {
    if (terms.size() == 1) return terms.front().CopyTo(result);

    std::vector<ICondition*> children;
    children.reserve(terms.size());
    for (const auto& term : terms) children.push_back(term.Get());
    return factory.CreateCompoundFromArray(type, children.data(), static_cast<ULONG>(children.size()),
                                           CONDITION_CREATION_NONE, IID_PPV_ARGS(result));
}

} // namespace

HRESULT CreateTagQueryCondition(std::span<const TagSearchTerm> terms, ICondition** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (terms.empty() || std::any_of(terms.begin(), terms.end(), [](const auto& term) { return term.tag.empty(); })) {
        return E_INVALIDARG;
    }

    ComPtr<IConditionFactory2> factory;
    HRESULT hr = CoCreateInstance(CLSID_ConditionFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return hr;

    // AQS binds binary operators from left to right. Fold runs of the same
    // operator together, using AND NOT to append a negated term.
    std::vector<ComPtr<ICondition>> group;
    CONDITION_TYPE groupType = CT_AND_CONDITION;
    for (const auto& term : terms) {
        ComPtr<ICondition> leaf;
        hr = factory->CreateStringLeaf(PKEY_Keywords, COP_EQUAL, term.tag.c_str(), nullptr,
                                       CONDITION_CREATION_NONE, IID_PPV_ARGS(leaf.GetAddressOf()));
        if (FAILED(hr)) return hr;
        if (term.operation == TagOperator::Not) {
            ComPtr<ICondition> negated;
            hr = factory->MakeNot(leaf.Get(), FALSE, negated.GetAddressOf());
            if (FAILED(hr)) return hr;
            leaf = std::move(negated);
        }

        if (!group.empty()) {
            const CONDITION_TYPE nextType = term.operation == TagOperator::Or ? CT_OR_CONDITION : CT_AND_CONDITION;
            if (group.size() > 1 && nextType != groupType) {
                ComPtr<ICondition> combined;
                hr = CombineTerms(*factory.Get(), groupType, group, combined.GetAddressOf());
                if (FAILED(hr)) return hr;
                group.clear();
                group.push_back(std::move(combined));
            }
            groupType = nextType;
        }
        group.push_back(std::move(leaf));
    }

    return CombineTerms(*factory.Get(), groupType, group, result);
}

HRESULT CreateTagSearchCondition(const Tags& tags, ICondition** result) {
    return CreateTagQueryCondition(AndTerms(tags), result);
}

namespace {

HRESULT CreateSearchFolder(std::span<const TagSearchTerm> terms, const std::wstring& name,
                           const Folders& folders, PIDLIST_ABSOLUTE* result) {
    if (!result) return E_POINTER;
    *result = nullptr;

    std::vector<Pidl> items;
    std::vector<PCIDLIST_ABSOLUTE> ids;
    for (const auto& folder : folders) {
        PIDLIST_ABSOLUTE id = nullptr;
        if (SUCCEEDED(SHParseDisplayName(folder.c_str(), nullptr, &id, 0, nullptr))) {
            items.emplace_back(id, &CoTaskMemFree);
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        return E_INVALIDARG;
    }

    ComPtr<IShellItemArray> scope;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(
        static_cast<UINT>(ids.size()), ids.data(), scope.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<ICondition> condition;
    hr = CreateTagQueryCondition(terms, condition.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    ComPtr<ISearchFolderItemFactory> search;
    hr = CoCreateInstance(CLSID_SearchFolderItemFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(search.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = search->SetDisplayName(name.c_str());
    if (SUCCEEDED(hr)) hr = search->SetScope(scope.Get());
    if (SUCCEEDED(hr)) hr = search->SetCondition(condition.Get());
    return SUCCEEDED(hr) ? search->GetIDList(result) : hr;
}

bool OpenSearchFolder(HWND owner, PIDLIST_ABSOLUTE id, HRESULT hr) {
    const Pidl search(id, &CoTaskMemFree);
    if (FAILED(hr)) {
        return false;
    }

    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpIDList = search.get();
    execute.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&execute) != FALSE;
}

} // namespace

HRESULT CreateTagSearchFolder(const Tags& tags, const Folders& folders, PIDLIST_ABSOLUTE* result) {
    std::wstring name;
    for (const auto& tag : tags) {
        if (!name.empty()) name += L" AND ";
        name += tag;
    }
    return CreateSearchFolder(AndTerms(tags), name, folders, result);
}

HRESULT CreateTagQueryFolder(std::span<const TagSearchTerm> terms, const Folders& folders, PIDLIST_ABSOLUTE* result) {
    return CreateSearchFolder(terms, FormatTagQuery(terms), folders, result);
}

bool OpenTagSearch(HWND owner, const Tags& tags, const Folders& folders) {
    PIDLIST_ABSOLUTE id = nullptr;
    const HRESULT hr = CreateTagSearchFolder(tags, folders, &id);
    return OpenSearchFolder(owner, id, hr);
}

bool OpenTagQuery(HWND owner, std::span<const TagSearchTerm> terms, const Folders& folders) {
    PIDLIST_ABSOLUTE id = nullptr;
    const HRESULT hr = CreateTagQueryFolder(terms, folders, &id);
    return OpenSearchFolder(owner, id, hr);
}

} // namespace tagsearch
