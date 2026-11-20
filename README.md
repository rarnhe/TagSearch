# TagSearch

TagSearch 是 Windows 桌面的标签搜索工具。它读取文件和文件夹在 Windows Shell 中公开的 `System.Keywords` 属性，再让文件资源管理器搜索所选标签。

## 使用

在 CMD 中运行任一版本。桌面和下载目录始终作为默认范围，可以在 EXE 后追加任意数量的目录。含空格的路径请加双引号。

```cmd
TagSearch_TreeView.exe C:\Work\repos "D:\Project Files"
TagSearch_Caption.exe C:\Work\repos "D:\Project Files"
```

- **TreeView** 在屏幕右侧显示标签列表。单击标签，或选中后按 Enter，即可在文件资源管理器中搜索。首次双击可直接打开，窗口显示后在后台扫描桌面和下载目录。再次双击会恢复并激活已有窗口。TreeView 固定在主屏工作区右上角，标题栏无法拖动，并通过隐藏所有者和工具窗口样式从任务切换视图中隐藏。标题如 `文件夹1·文件夹2`，只列出额外传入的有效目录。只扫描默认目录时标题为「默认目录」。
- **Caption** 启动后先在主屏工作区的随机位置显示采用系统强调色的 `↺`，并在后台扫描。标签窗口在 `↺` 周围由近及远排列，初始位置互不重叠，拖动也限制在主屏工作区。空间用尽后可用鼠标滚轮或 Page Up／Page Down 翻页。
- 标签窗口默认浅绿色。右键选择 AND、OR 或 NOT，分别显示浅绿色、粉色或灰色。拖动标签使窗口重叠，再点击其中一个执行搜索。被点击的标签排在首位，后续标签按当前扫描列表顺序加入，操作符从左到右绑定。查询 `AND tagA OR tagB NOT tagC` 表示 `(tagA OR tagB) AND NOT tagC`。`↺` 只用于重新扫描，没有右键菜单，也不参与重叠查询。重新扫描保留已有标签的操作符，Esc 可退出。
- 启动时递归扫描有效目录，读取根目录、子目录和文件的 Shell 属性。未公开 `System.Keywords` 的项目不会提供标签。扫描跳过无法访问的目录，也不会进入扫描过程中遇到的目录联接或目录符号链接。
- 标签保存在当前进程内存中。Explorer 使用同一组已解析目录作为搜索范围，结果取决于 Windows Search 和对应文件类型的属性处理程序。

## 架构

| 文件 | 职责 |
| --- | --- |
| `TagSearch_TreeView.cpp` | TreeView 界面、标签交互和开始菜单快捷方式 |
| `TagSearch_Caption.cpp` | 刷新窗口、浮动标签窗口、拖动、DPI 和操作符菜单 |
| `TagSearch_Layout.cpp/.h` | 围绕刷新窗口计算可见、互不相交的标签位置 |
| `TagSearch_Core.cpp/.h` | Unicode 命令行、默认目录、递归扫描、标签规范化和 Explorer 搜索 |
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

直接生成工程时，两个 EXE 位于 `out/test/Release/`。Windows CI 验证上述 4 组预设的 EXE 架构并运行 CTest。测试覆盖标签规范化、默认目录、带引号的 Unicode 路径、嵌套 JPEG 的真实标签属性、重新扫描、围绕锚点的非重叠布局、窗口重叠判断及 AND／OR／NOT 搜索对象创建。PR 的 `test_report.md` 与 JUnit 结果可从 Actions 下载，主分支运行后会更新仓库根目录的报告。集成测试会实际启动两个程序，检查 TreeView 的固定位置和目录标题、Caption 的刷新窗口与真实标签布局，以及各自的单实例启动。Alt+Tab、Win+Tab、窗口拖动、跨屏 DPI、主题及 Explorer 实际返回的结果适合在交互式 Windows 桌面验证。
