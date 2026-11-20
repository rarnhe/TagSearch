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
    overlap_finds_all_direct_intersections
    overlap_ignores_touching_edges_and_corners
    search_condition_keeps_one_tag_literal
    search_condition_combines_all_tags_with_and
    create_search_folder_accepts_combined_tags
    caption_slots_keep_windows_in_bounds
    caption_slots_handle_tight_fit
    mixed_tag_query_matches_windows_search_order
)

foreach(case IN LISTS cases)
    add_test(NAME ${case} COMMAND tagsearch_tests ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
endforeach()

add_executable(tagsearch_treeview_startup_test test_treeview_startup.cpp)
target_link_libraries(tagsearch_treeview_startup_test PRIVATE TagSearchCore)
target_compile_options(tagsearch_treeview_startup_test PRIVATE /utf-8)
add_dependencies(tagsearch_treeview_startup_test TagSearch_TreeView)

add_test(NAME treeview_single_instance_startup
         COMMAND tagsearch_treeview_startup_test startup $<TARGET_FILE:TagSearch_TreeView>)
add_test(NAME treeview_fixed_window_and_folder_title
         COMMAND tagsearch_treeview_startup_test fixed-window $<TARGET_FILE:TagSearch_TreeView>)
set_tests_properties(treeview_single_instance_startup treeview_fixed_window_and_folder_title
                     PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
