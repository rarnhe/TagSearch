# TagSearch

TagSearch 是 Windows 桌面的标签搜索工具。它读取文件和文件夹在 Windows Shell 中公开的 `System.Keywords` 属性，再让文件资源管理器搜索所选标签。

## 使用

在 CMD 中运行任一版本。桌面和下载目录始终作为默认范围，可以在 EXE 后追加任意数量的目录。含空格的路径请加双引号。

```cmd
TagSearch_TreeView.exe C:\Work\repos "D:\Project Files"
TagSearch_Caption.exe C:\Work\repos "D:\Project Files"
```

- **TreeView** 在屏幕右侧显示标签列表。单击标签，或选中后按 Enter，即可在文件资源管理器中搜索。首次双击可直接打开，窗口显示后在后台扫描桌面和下载目录。再次双击会恢复并激活已有窗口。TreeView 固定在主屏工作区右上角，标题栏无法拖动，并通过隐藏所有者和工具窗口样式从任务切换视图中隐藏。标题如 `文件夹1·文件夹2`，只列出额外传入的有效目录。只扫描默认目录时标题为「默认目录」。
- **Caption** 启动后在后台扫描，扫描完成后在主屏工作区随机分布全部标签。每个浮动窗口只有 Windows 原生标题栏，单击标签时标题文字短暂加粗，窗口光晕随之消失并恢复，标题栏保持当前配色，拖动限制在主屏工作区。排布优先使用互不重叠的位置，空间不足时仍显示全部窗口并通过 Windows 系统通知提示「可能发生重叠的标签个数」。同一窗口只计一次。扫描未找到标签时自动退出。
- 标签窗口默认浅绿色。右键选择 AND、OR 或 NOT，分别显示浅绿色、粉色或灰色。拖动标签使窗口重叠，再点击其中一个执行搜索。同一组标签按 AND、OR、NOT 的顺序组成查询，操作符从左到右绑定。查询 `AND tagA OR tagB NOT tagC` 表示 `(tagA OR tagB) AND NOT tagC`。Esc 可退出。
- 递归扫描读取根目录、子目录和文件的 Shell 属性。未公开 `System.Keywords` 的项目不会提供标签。扫描跳过无法访问的目录，也不会进入扫描过程中遇到的目录联接或目录符号链接。
- **TreeView 和 Caption 的目录缓存**。桌面和下载始终实时扫描，标签缓存只用于命令行新增的目录。参数目录缺少 `desktop.ini` 时，递归扫描后创建文件，写入 `[.ShellClassInfo]` 的 `InfoTip=`、`IconResource=`，以及摘要信息节的 `Prop5=31,标签1;标签2`。已有 `desktop.ini` 但摘要信息节缺少有效的 `Prop5=31,` 时，同样扫描并写入该项，保留图标等原有配置。已有有效标签缓存时直接读取，空标签列表也是有效缓存。桌面、下载和各参数目录的标签合并去重后显示。
- Explorer 使用同一组已解析目录作为搜索范围，结果取决于 Windows Search 和对应文件类型的属性处理程序。

## 架构

| 文件 | 职责 |
| --- | --- |
| `TagSearch_TreeView.cpp` | TreeView 界面、标签交互和开始菜单快捷方式 |
| `TagSearch_Caption.cpp` | 后台扫描、浮动标签窗口、随机排布、重叠提示、拖动、DPI 和操作符菜单 |
| `TagSearch_Core.cpp/.h` | Unicode 命令行、默认目录、递归扫描、共享目录缓存、desktop.ini 图标解析、标签规范化和 Explorer 搜索 |
| `tests/` | CTest 测试和 Markdown 报告生成 |
| `.github/workflows/test_cpp.yml` | Windows 双轨验证，构建两个 EXE，运行 CTest 并生成报告 |

## 构建与测试

需要 Windows 10 或 Windows 11、Visual Studio 2022 或更新版本的 MSVC 工具、Windows SDK 和 CMake。预设使用 Ninja，Visual Studio 可通过「使用 C++ 的桌面开发」安装 CMake 工具。

用 Visual Studio 的「打开文件夹」打开仓库，然后在顶部配置下拉菜单选择架构和构建模式。

| 目标架构 | 模式 |
| --- | --- |
| x86 | Debug、Release |
| x64 | Debug、Release |

在命令行使用预设时，先打开与目标架构对应的 Visual Studio Developer Command Prompt。Ninja 使用终端中选定的工具链。

```cmd
cmake --preset x64-release
cmake --build --preset x64-release --parallel
ctest --preset x64-release
```

此例的两个 EXE 位于 `out/build/x64-release/`。也可直接生成 Visual Studio 工程并构建。

```powershell
cmake -S . -B out/test -A x64 -DBUILD_TESTING=ON
cmake --build out/test --config Release --parallel
ctest --test-dir out/test -C Release --output-on-failure --no-tests=error
```

直接生成工程时，两个 EXE 位于 `out/test/Release/`。Windows CI 验证上述 4 组预设的 EXE 架构并运行 CTest。测试覆盖标签规范化、默认目录、带引号的 Unicode 路径、嵌套 JPEG 的真实标签属性、首次构建缓存、再次读取及嵌套目录缓存、工作区内的随机布局、拥挤时的重叠计数、窗口重叠判断及 AND／OR／NOT 搜索对象创建。PR 的 `test_report.md` 与 JUnit 结果可从 Actions 下载，主分支运行后会更新仓库根目录的报告。集成测试会实际启动两个程序，检查 TreeView 的固定位置和目录标题、Caption 的后台扫描、原生标题栏和拥挤时同时显示全部标签，以及各自的单实例启动。Alt+Tab、Win+Tab、窗口拖动、跨屏 DPI、主题、系统通知及 Explorer 实际返回的结果适合在交互式 Windows 桌面验证。
