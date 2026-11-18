#include "TagSearchCore.h"

#include <exception>
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

void ParseDesktopIniTags() {
    const auto parsed = tagsearch::ParseDesktopIniTagsLine(
        L"Prop5=31,alpha; beta ;gamma;");

    Expect(parsed.has_value(), "Prop5=31 line should be recognized");
    ExpectEqual(
        *parsed,
        std::vector<std::wstring>{L"alpha", L"beta", L"gamma"},
        "desktop.ini tags should be parsed and trimmed");
}

void ParseDesktopIniMissing() {
    Expect(
        !tagsearch::ParseDesktopIniTagsLine(L"[.ShellClassInfo]").has_value(),
        "unrelated desktop.ini lines should not be parsed as tags");
}

void FormatDesktopIniTags() {
    ExpectEqual(
        tagsearch::FormatDesktopIniTagsLine({L"alpha", L"beta"}),
        std::wstring(L"Prop5=31,alpha;beta"),
        "desktop.ini tag line should use the expected property prefix");
}

void FormatDesktopIniSkipsEmpty() {
    ExpectEqual(
        tagsearch::FormatDesktopIniTagsLine({L" alpha ", L"", L"beta ", L"  "}),
        std::wstring(L"Prop5=31,alpha;beta"),
        "desktop.ini serialization should trim and skip empty tags");
}

void UpdateDesktopIniReplacesTagLine() {
    const std::wstring original =
        L"[.ShellClassInfo]\nProp5=31,old;older\nOther=keep\n";
    const std::wstring expected =
        L"[.ShellClassInfo]\nProp5=31,new\nOther=keep\n";

    ExpectEqual(
        tagsearch::UpdateDesktopIniTagsLine(original, {L"new"}),
        expected,
        "existing Prop5 line should be replaced without losing other settings");
}

void UpdateDesktopIniAppendsMissingTagLine() {
    const std::wstring original = L"[.ShellClassInfo]\nIconResource=icon.dll,0\n";
    const std::wstring expected =
        L"[.ShellClassInfo]\nIconResource=icon.dll,0\nProp5=31,new\n";

    ExpectEqual(
        tagsearch::UpdateDesktopIniTagsLine(original, {L"new"}),
        expected,
        "missing Prop5 line should be appended");
}

void UpdateDesktopIniPreservesWindowsLineEndings() {
    const std::wstring original = L"Header=1\r\nProp5=31,old\r\n";
    const std::wstring expected = L"Header=1\r\nProp5=31,new\r\n";

    ExpectEqual(
        tagsearch::UpdateDesktopIniTagsLine(original, {L"new"}),
        expected,
        "existing CRLF line endings should be preserved");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<std::pair<std::string, TestFunction>> tests{
        {"split_keywords_basic", SplitKeywordsBasic},
        {"split_keywords_trims_whitespace", SplitKeywordsTrimsWhitespace},
        {"split_keywords_ignores_empty", SplitKeywordsIgnoresEmpty},
        {"normalize_tags_deduplicates_and_sorts", NormalizeTagsDeduplicatesAndSorts},
        {"normalize_tags_discards_empty", NormalizeTagsDiscardsEmpty},
        {"parse_desktop_ini_tags", ParseDesktopIniTags},
        {"parse_desktop_ini_missing", ParseDesktopIniMissing},
        {"format_desktop_ini_tags", FormatDesktopIniTags},
        {"format_desktop_ini_skips_empty", FormatDesktopIniSkipsEmpty},
        {"update_desktop_ini_replaces_tag_line", UpdateDesktopIniReplacesTagLine},
        {"update_desktop_ini_appends_missing_tag_line", UpdateDesktopIniAppendsMissingTagLine},
        {"update_desktop_ini_preserves_windows_line_endings", UpdateDesktopIniPreservesWindowsLineEndings},
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
