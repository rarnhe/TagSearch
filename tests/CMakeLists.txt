if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "TagSearch tests require Windows, MSVC and the Windows SDK")
endif()

# Compile the Caption layout and notification icon helpers without the app entry point.
add_executable(tagsearch_tests test_tagsearch.cpp ../TagSearch_Caption.cpp TagSearch_Layout.h)
target_link_libraries(tagsearch_tests PRIVATE TagSearchCore)
target_compile_definitions(tagsearch_tests PRIVATE
    TAGSEARCH_CAPTION_LAYOUT_TEST_ONLY
    TAGSEARCH_FIXTURE=L"${CMAKE_CURRENT_SOURCE_DIR}/tagged.jpg"
)
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
    caption_reads_desktop_ini_icon_resource
    caption_reads_legacy_desktop_ini_icon
    caption_notice_matches_sender_icon_or_has_none
    caption_selects_notice_icon_folders_by_arguments
    caption_shortcut_stores_icon_and_app_id
    caption_layout_packs_variable_widths
    caption_layout_keeps_all_crowded_windows_and_counts_overlap
    caption_layout_randomizes_positions
    mixed_tag_query_matches_windows_search_order
)

foreach(case IN LISTS cases)
    add_test(NAME ${case} COMMAND tagsearch_tests ${case})
    set_tests_properties(${case} PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
endforeach()

add_executable(tagsearch_window_startup_test test_window_startup.cpp)
target_link_libraries(tagsearch_window_startup_test PRIVATE TagSearchCore)
target_compile_options(tagsearch_window_startup_test PRIVATE /utf-8)
add_dependencies(tagsearch_window_startup_test TagSearch_TreeView TagSearch_Caption)

add_test(NAME treeview_single_instance_startup
         COMMAND tagsearch_window_startup_test startup $<TARGET_FILE:TagSearch_TreeView>)
add_test(NAME treeview_fixed_window_and_folder_title
         COMMAND tagsearch_window_startup_test fixed-window $<TARGET_FILE:TagSearch_TreeView>)
add_test(NAME treeview_blank_click_preserves_selection
         COMMAND tagsearch_window_startup_test treeview-blank-click $<TARGET_FILE:TagSearch_TreeView>
                 "${CMAKE_CURRENT_SOURCE_DIR}/tagged.jpg")
add_test(NAME caption_background_scan_and_native_tag_windows
         COMMAND tagsearch_window_startup_test caption $<TARGET_FILE:TagSearch_Caption>
                 "${CMAKE_CURRENT_SOURCE_DIR}/tagged.jpg")
add_test(NAME caption_crowded_tags_are_all_visible
         COMMAND tagsearch_window_startup_test caption-crowded $<TARGET_FILE:TagSearch_Caption>
                 "${CMAKE_CURRENT_SOURCE_DIR}/tagged.jpg")
set_tests_properties(treeview_single_instance_startup treeview_fixed_window_and_folder_title
                     treeview_blank_click_preserves_selection
                     caption_background_scan_and_native_tag_windows caption_crowded_tags_are_all_visible
                     PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
