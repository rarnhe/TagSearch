# TagSearch C++ Windows 测试报告

- 状态：通过 (Passed)
- 时间（UTC）：2026-11-21 15:42:40
- 测试源码基准提交：95db0868711bed3530772448b70c1b6e849135cf
- 执行记录：https://github.com/rarnhe/TagSearch/actions/runs/44920552460
- 平台：Windows / MSVC / C++20 / Windows SDK
- 阶段：Configure=success / Build=success / CTest=success
- 统计：通过 23 / 失败 0 / 跳过 0 / 共 23

## GitHub Actions 阶段状态

| 阶段 | 结果 |
| --- | --- |
| Configure | success |
| Build | success |
| CTest | success |

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
| create_search_folder_accepts_multiple_scopes_and_literal_tag | 通过 |
| create_search_folder_rejects_empty_scope | 通过 |
| overlap_finds_all_direct_intersections | 通过 |
| overlap_ignores_touching_edges_and_corners | 通过 |
| search_condition_keeps_one_tag_literal | 通过 |
| search_condition_combines_all_tags_with_and | 通过 |
| create_search_folder_accepts_combined_tags | 通过 |
| caption_layout_packs_variable_widths | 通过 |
| caption_layout_keeps_all_crowded_windows_and_counts_overlap | 通过 |
| caption_layout_randomizes_positions | 通过 |
| mixed_tag_query_matches_windows_search_order | 通过 |
| treeview_single_instance_startup | 通过 |
| treeview_fixed_window_and_folder_title | 通过 |
| caption_background_scan_and_native_tag_windows | 通过 |
| caption_crowded_tags_are_all_visible | 通过 |

## 失败详情

## 自动测试覆盖范围
验证标签拆分、空白清理、去重与排序、默认目录和带引号的 Unicode 路径、从嵌套 JPEG 读取真实 System.Keywords 属性、重新扫描后的变化，主屏工作区边界、原生标题栏随机布局、拥挤时的重叠窗口计数、窗口重叠判断（含边缘接触和间接重叠），Windows Shell 多目录和 AND／OR／NOT 搜索条件的创建，以及 TreeView 的无参数启动、隐藏所有者、单实例恢复、窗口位置锁定和目录标题，以及 Caption 的后台扫描、随机布局、实际窗口边界、拥挤时同时显示全部标签及重复启动。

## 人工验证边界
Windows CI 构建 TreeView 和 Caption 两个 EXE。Alt+Tab、Win+Tab、窗口拖动、DPI、主题、系统通知及 Explorer 实际返回的搜索结果需要在交互式 Windows 桌面验证。
