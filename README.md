# TagSearch

TagSearch 是 Windows 桌面的标签搜索工具。它读取文件和文件夹在 Windows Shell 中公开的 `System.Keywords` 属性，再让文件资源管理器搜索所选标签。

## 使用

在 CMD 中运行任一版本。桌面和下载目录始终作为默认范围，可以在 EXE 后追加任意数量的目录。含空格的路径请加双引号。

```cmd
TagSearch_TreeView.exe C:\Work\repos "D:\Project Files"
TagSearch_Caption.exe C:\Work\repos "D:\Project Files"
```

- **TreeView** 在屏幕右侧显示标签列表。单击标签，或选中后按 Enter，即可在文件资源管理器中搜索。
- **Caption** 使用主面板显示标签数量和翻页按钮，每个标签有独立的半透明工具窗口。单击标题栏或文字区域执行搜索，按住拖动可调整位置。单击 `↺` 重新扫描并重置布局。标签窗口随所在屏幕调整尺寸和字体，前景颜色随窗口背景变化，优先使用隶书字体。
- 启动时递归扫描有效目录，读取根目录、子目录和文件的 Shell 属性。未公开 `System.Keywords` 的项目不会提供标签。扫描跳过无法访问的目录，也不会进入扫描过程中遇到的目录联接或目录符号链接。
- 标签保存在当前进程内存中。Explorer 使用同一组已解析目录作为搜索范围，结果取决于 Windows Search 和对应文件类型的属性处理程序。

## 架构

| 文件 | 职责 |
| --- | --- |
| `TagSearch_TreeView.cpp/.h` | TreeView 界面、标签交互和开始菜单快捷方式 |
| `TagSearch_Caption.cpp` | 主面板、浮动标签窗口、拖动、布局、DPI 和刷新 |
| `TagSearchCore.cpp/.h` | Unicode 命令行、默认目录、递归扫描、标签规范化和 Explorer 搜索 |
| `tests/` | CTest 测试和 Markdown 报告生成 |
| `.github/workflows/test_cpp.yml` | Windows 双轨验证，构建两个 EXE，运行 CTest 并生成报告 |

## 构建与测试

需要 Windows 10 或 Windows 11、Visual Studio 2022 或更新版本的 MSVC 工具、Windows SDK 和 CMake。

```powershell
cmake -S . -B out/test -A x64 -DBUILD_TESTING=ON
cmake --build out/test --config Release --parallel
ctest --test-dir out/test -C Release --output-on-failure --no-tests=error
```

两个 EXE 位于 `out/test/Release/`。测试覆盖标签规范化、默认目录、带引号的 Unicode 路径、嵌套 JPEG 的真实标签属性、重新扫描及多目录搜索对象创建。PR 的 `test_report.md` 与 JUnit 结果可从 Actions 下载，主分支运行后会更新仓库根目录的报告。窗口拖动、跨屏 DPI、主题以及 Explorer 实际返回的结果适合在交互式 Windows 桌面验证。
