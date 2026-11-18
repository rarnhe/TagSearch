#include "TagSearchCore.h"

#include <shlobj.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

void CreateSearchFolderAcceptsMultipleScopesAndLiteralTag() {
    TestDirectory test;
    const auto first = test.path / L"one & 本地";
    const auto second = test.path / L"two";
    fs::create_directories(first);
    fs::create_directories(second);
    const std::wstring tag = L"项目 & \"归档\" + %";
    PIDLIST_ABSOLUTE id = nullptr;
    const auto result = tagsearch::CreateTagSearchFolder(tag, {first, second}, &id);
    CoTaskMemFree(id);
    Expect(SUCCEEDED(result) && id != nullptr, "Shell should create a search folder with both scopes and a literal tag");
}

void CreateSearchFolderRejectsEmptyScope() {
    PIDLIST_ABSOLUTE id = nullptr;
    Expect(tagsearch::CreateTagSearchFolder(L"example", {}, &id) == E_INVALIDARG && id == nullptr,
           "a missing scope must not turn into an unrestricted search");
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
        {"create_search_folder_accepts_multiple_scopes_and_literal_tag", CreateSearchFolderAcceptsMultipleScopesAndLiteralTag},
        {"create_search_folder_rejects_empty_scope", CreateSearchFolderRejectsEmptyScope},
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
