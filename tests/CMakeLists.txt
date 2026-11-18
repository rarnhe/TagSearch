if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "TagSearch tests require Windows, MSVC and the Windows SDK")
endif()

add_executable(tagsearch_tests
    test_tagsearch.cpp
    ../TagSearchCore.cpp
)
target_compile_features(tagsearch_tests PRIVATE cxx_std_20)
target_include_directories(tagsearch_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
if(MSVC)
    target_compile_options(tagsearch_tests PRIVATE /utf-8)
endif()

set(cases
    split_keywords_basic
    split_keywords_trims_whitespace
    split_keywords_ignores_empty
    normalize_tags_deduplicates_and_sorts
    normalize_tags_discards_empty
    parse_desktop_ini_tags
    parse_desktop_ini_missing
    format_desktop_ini_tags
    format_desktop_ini_skips_empty
    update_desktop_ini_replaces_tag_line
    update_desktop_ini_appends_missing_tag_line
    update_desktop_ini_preserves_windows_line_endings
)

foreach(case IN LISTS cases)
    add_test(NAME ${case} COMMAND tagsearch_tests ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 10 RUN_SERIAL TRUE)
endforeach()
