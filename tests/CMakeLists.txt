if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "TagSearch tests require Windows, MSVC and the Windows SDK")
endif()

add_executable(tagsearch_tests test_tagsearch.cpp)
target_link_libraries(tagsearch_tests PRIVATE TagSearchCore)
target_compile_definitions(tagsearch_tests PRIVATE TAGSEARCH_FIXTURE=L"${CMAKE_CURRENT_SOURCE_DIR}/tagged.jpg")
target_compile_options(tagsearch_tests PRIVATE /utf-8)

set(cases
    split_keywords_basic
    split_keywords_trims_whitespace
    split_keywords_ignores_empty
    normalize_tags_deduplicates_and_sorts
    normalize_tags_discards_empty
    resolve_folders_keeps_known_folders
    resolve_folders_accepts_quoted_paths_and_skips_invalid_inputs
    scan_tags_reads_nested_shell_properties_without_using_cache
    create_search_folder_accepts_multiple_scopes_and_literal_tag
    create_search_folder_rejects_empty_scope
)

foreach(case IN LISTS cases)
    add_test(NAME ${case} COMMAND tagsearch_tests ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
endforeach()
