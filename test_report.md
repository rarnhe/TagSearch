# TagSearch C++ Windows 测试报告

- 状态：失败或未完整执行 (Failed)
- 时间（UTC）：2026-11-22 11:40:16
- 测试源码基准提交：3049d2b8cd2c1a8dde0316c6d779d43356e32e3c
- 执行记录：https://github.com/rarnhe/TagSearch/actions/runs/45018354278
- 平台：Windows / MSVC / C++20 / Windows SDK
- 阶段：Configure=success / Build=success / CTest=failure
- 统计：通过 33 / 失败 1 / 跳过 0 / 共 34

## GitHub Actions 阶段状态

| 阶段 | 结果 |
| --- | --- |
| Configure | success |
| Build | success |
| CTest | failure |

## 测试用例结果

| 测试 | 结果 |
| --- | --- |
| split_keywords_basic | 通过 |
| split_keywords_trims_whitespace | 通过 |
| split_keywords_ignores_empty | 通过 |
| normalize_tags_deduplicates_and_sorts | 通过 |
| normalize_tags_discards_empty | 通过 |
| resolve_folders_keeps_known_folders | 通过 |
| resolve_folders_accepts_quoted_paths_and_skips_invalid_inputs | 通过 |
| scan_tags_reads_nested_shell_properties_without_using_cache | 通过 |
| tag_cache_stores_requested_folders_independently | 通过 |
| tag_cache_scans_default_roots_live_and_repairs_invalid_cache | 通过 |
| tag_cache_combines_warm_and_cold_nested_folders | 通过 |
| create_search_folder_accepts_multiple_scopes_and_literal_tag | 通过 |
| create_search_folder_rejects_empty_scope | 通过 |
| overlap_finds_all_direct_intersections | 通过 |
| overlap_ignores_touching_edges_and_corners | 通过 |
| search_condition_keeps_one_tag_literal | 通过 |
| search_condition_combines_all_tags_with_and | 通过 |
| create_search_folder_accepts_combined_tags | 通过 |
| desktop_ini_reads_icon_resource | 通过 |
| desktop_ini_reads_legacy_icon | 通过 |
| caption_notice_matches_sender_icon_or_has_none | 通过 |
| caption_selects_notice_icon_folders_by_arguments | 通过 |
| caption_shortcut_stores_icon_and_app_id | 通过 |
| caption_bitmap_preserves_background_and_reveals_glyphs | 通过 |
| caption_layout_packs_variable_widths | 通过 |
| caption_layout_keeps_all_crowded_windows_and_counts_overlap | 通过 |
| caption_layout_randomizes_positions | 通过 |
| mixed_tag_query_matches_windows_search_order | 通过 |
| treeview_single_instance_startup | 通过 |
| treeview_fixed_window_and_folder_title | 通过 |
| treeview_blank_click_preserves_selection | 通过 |
| treeview_creates_and_reuses_desktop_ini | 通过 |
| caption_background_scan_and_native_tag_windows | 通过 |
| caption_crowded_tags_are_all_visible | 失败 |

## 失败详情

### caption_crowded_tags_are_all_visible
<pre>
the title should occupy the client area of the caption-only window

</pre>

### CTest 阶段输出
进程退出码：8
<pre>Test project D:/a/TagSearch/TagSearch/out/test
      Start  1: split_keywords_basic
 1/34 Test  #1: split_keywords_basic ............................................   Passed    0.03 sec
      Start  2: split_keywords_trims_whitespace
 2/34 Test  #2: split_keywords_trims_whitespace .................................   Passed    0.01 sec
      Start  3: split_keywords_ignores_empty
 3/34 Test  #3: split_keywords_ignores_empty ....................................   Passed    0.01 sec
      Start  4: normalize_tags_deduplicates_and_sorts
 4/34 Test  #4: normalize_tags_deduplicates_and_sorts ...........................   Passed    0.02 sec
      Start  5: normalize_tags_discards_empty
 5/34 Test  #5: normalize_tags_discards_empty ...................................   Passed    0.01 sec
      Start  6: resolve_folders_keeps_known_folders
 6/34 Test  #6: resolve_folders_keeps_known_folders .............................   Passed    0.02 sec
      Start  7: resolve_folders_accepts_quoted_paths_and_skips_invalid_inputs
 7/34 Test  #7: resolve_folders_accepts_quoted_paths_and_skips_invalid_inputs ...   Passed    0.02 sec
      Start  8: scan_tags_reads_nested_shell_properties_without_using_cache
 8/34 Test  #8: scan_tags_reads_nested_shell_properties_without_using_cache .....   Passed    0.17 sec
      Start  9: tag_cache_stores_requested_folders_independently
 9/34 Test  #9: tag_cache_stores_requested_folders_independently ................   Passed    0.14 sec
      Start 10: tag_cache_scans_default_roots_live_and_repairs_invalid_cache
10/34 Test #10: tag_cache_scans_default_roots_live_and_repairs_invalid_cache ....   Passed    0.11 sec
      Start 11: tag_cache_combines_warm_and_cold_nested_folders
11/34 Test #11: tag_cache_combines_warm_and_cold_nested_folders .................   Passed    0.07 sec
      Start 12: create_search_folder_accepts_multiple_scopes_and_literal_tag
12/34 Test #12: create_search_folder_accepts_multiple_scopes_and_literal_tag ....   Passed    0.06 sec
      Start 13: create_search_folder_rejects_empty_scope
13/34 Test #13: create_search_folder_rejects_empty_scope ........................   Passed    0.01 sec
      Start 14: overlap_finds_all_direct_intersections
14/34 Test #14: overlap_finds_all_direct_intersections ..........................   Passed    0.01 sec
      Start 15: overlap_ignores_touching_edges_and_corners
15/34 Test #15: overlap_ignores_touching_edges_and_corners ......................   Passed    0.01 sec
      Start 16: search_condition_keeps_one_tag_literal
16/34 Test #16: search_condition_keeps_one_tag_literal ..........................   Passed    0.02 sec
      Start 17: search_condition_combines_all_tags_with_and
17/34 Test #17: search_condition_combines_all_tags_with_and .....................   Passed    0.02 sec
      Start 18: create_search_folder_accepts_combined_tags
18/34 Test #18: create_search_folder_accepts_combined_tags ......................   Passed    0.05 sec
      Start 19: desktop_ini_reads_icon_resource
19/34 Test #19: desktop_ini_reads_icon_resource .................................   Passed    0.01 sec
      Start 20: desktop_ini_reads_legacy_icon
20/34 Test #20: desktop_ini_reads_legacy_icon ...................................   Passed    0.02 sec
      Start 21: caption_notice_matches_sender_icon_or_has_none
21/34 Test #21: caption_notice_matches_sender_icon_or_has_none ..................   Passed    0.02 sec
      Start 22: caption_selects_notice_icon_folders_by_arguments
22/34 Test #22: caption_selects_notice_icon_folders_by_arguments ................   Passed    0.02 sec
      Start 23: caption_shortcut_stores_icon_and_app_id
23/34 Test #23: caption_shortcut_stores_icon_and_app_id .........................   Passed    0.08 sec
      Start 24: caption_bitmap_preserves_background_and_reveals_glyphs
24/34 Test #24: caption_bitmap_preserves_background_and_reveals_glyphs ..........   Passed    0.07 sec
      Start 25: caption_layout_packs_variable_widths
25/34 Test #25: caption_layout_packs_variable_widths ............................   Passed    0.01 sec
      Start 26: caption_layout_keeps_all_crowded_windows_and_counts_overlap
26/34 Test #26: caption_layout_keeps_all_crowded_windows_and_counts_overlap .....   Passed    0.01 sec
      Start 27: caption_layout_randomizes_positions
27/34 Test #27: caption_layout_randomizes_positions .............................   Passed    0.01 sec
      Start 28: mixed_tag_query_matches_windows_search_order
28/34 Test #28: mixed_tag_query_matches_windows_search_order ....................   Passed    0.05 sec
      Start 29: treeview_single_instance_startup
29/34 Test #29: treeview_single_instance_startup ................................   Passed    0.20 sec
      Start 30: treeview_fixed_window_and_folder_title
30/34 Test #30: treeview_fixed_window_and_folder_title ..........................   Passed    0.17 sec
      Start 31: treeview_blank_click_preserves_selection
31/34 Test #31: treeview_blank_click_preserves_selection ........................   Passed    0.43 sec
      Start 32: treeview_creates_and_reuses_desktop_ini
32/34 Test #32: treeview_creates_and_reuses_desktop_ini .........................   Passed    0.27 sec
      Start 33: caption_background_scan_and_native_tag_windows
33/34 Test #33: caption_background_scan_and_native_tag_windows ..................   Passed    0.28 sec
      Start 34: caption_crowded_tags_are_all_visible
34/34 Test #34: caption_crowded_tags_are_all_visible ............................***Failed    0.53 sec
the title should occupy the client area of the caption-only window


97% tests passed, 1 tests failed out of 34

Total Test time (real) =   3.09 sec

The following tests FAILED:
	 34 - caption_crowded_tags_are_all_visible (Failed)
Errors while running CTest
Exit code: 8</pre>

## 自动测试覆盖范围
验证标签拆分、空白清理、去重与排序、默认目录和带引号的 Unicode 路径、从嵌套 JPEG 读取真实 System.Keywords 属性、重新扫描后的变化、TreeView 和 Caption 的默认目录实时扫描、参数目录缓存模板与再次读取、无效缓存重建及嵌套目录复用，主屏工作区边界、原生标题栏随机布局、拥挤时的重叠窗口计数、窗口重叠判断（含边缘接触和间接重叠），Windows Shell 多目录和 AND／OR／NOT 搜索条件的创建，以及 TreeView 的无参数启动、隐藏所有者、单实例恢复、窗口位置锁定和目录标题，以及 Caption 的后台扫描、随机布局、实际窗口边界、标题配色、圆角和强调色边框、透明笔画、字重与左右间距、拥挤时同时显示全部标签及重复启动。

## 人工验证边界
Windows CI 构建 TreeView 和 Caption 两个 EXE。Alt+Tab、Win+Tab、窗口拖动、Caption 按下和松开时的屏幕显示及光晕、DPI、主题、系统通知及 Explorer 实际返回的搜索结果需要在交互式 Windows 桌面验证。
