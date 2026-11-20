#include "TagSearch_Core.h"

#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <structuredquery.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <locale>
#include <memory>
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

Tags ScanTags(const Folders& folders, std::stop_token stop) {
    Tags tags;
    Folders pending = folders;
    std::set<fs::path> visited;
    while (!pending.empty() && !stop.stop_requested()) {
        auto folder = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(folder).second) {
            continue;
        }

        ReadItemTags(folder, tags);
        std::error_code error;
        for (fs::directory_iterator entry(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && entry != end && !stop.stop_requested(); entry.increment(error)) {
            const auto& path = entry->path();
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) {
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
    }
    return stop.stop_requested() ? Tags{} : NormalizeTags(std::move(tags));
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
