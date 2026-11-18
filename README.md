# TagSearch

TagSearch 是一个面向 Windows 桌面文件标签的轻量级搜索入口。它不自行实现文件内容索引，而是读取 Windows Shell 文件属性中的 `System.Keywords`，汇总标签，再将用户选择的标签交给 Windows Search / Explorer 执行搜索。

## 工作流程

1. 使用 `SHGetKnownFolderPath` 获取当前用户的桌面和下载目录。
2. 枚举这两个目录的第一层普通文件，通过 Windows Shell Property System 读取每个文件的 `System.Keywords` 属性。
3. 将属性值按分号拆分，清理空白、去重并排序，在 Win32 TreeView 中展示标签。
4. 点击标签时构造 `search-ms:` 查询并启动 Explorer，让 Windows Search 查找带有该标签的文件。
5. 将当前标签列表保存到桌面上的组合目录 `desktop.ini`，使用 `Prop5=31,` 行作为轻量缓存；下次启动时优先读取缓存。
6. 启动时还会注册开始菜单快捷方式，并尝试从程序目录的 `desktop.ini` 读取 `IconResource`，为快捷方式设置图标和 AppUserModelID。

## 当前实现边界

- 标签扫描只遍历桌面和下载目录的**第一层文件**，不递归子目录。
- 当前搜索 URI 的位置条件写为 `%userprofile%\\Downloads` 和 `%userprofile%\\Desktop`；如果系统将桌面或下载目录重定向到其他位置，扫描范围与搜索范围可能不一致。
- 当桌面缓存目录中的 `desktop.ini` 已存在时，启动阶段会读取缓存，而不是重新扫描文件属性；文件标签变化不会自动触发缓存失效或刷新。
- TreeView 右键菜单中的部分操作尚未完成，Windows Shell 搜索和 Toast 的实际表现仍依赖运行环境。

## 架构

- `TagSearch.cpp`：Win32 窗口、TreeView、Shell 属性读取、Explorer 搜索、快捷方式和通知集成。
- `TagSearchCore.h/.cpp`：可独立测试的标签拆分、清理、去重、排序，以及 `desktop.ini` 标签行解析、序列化和更新。
- `tests/`：原生 C++ 测试，通过 CTest 调用生产代码中的核心函数。
- `.github/workflows/test_cpp.yml`：Windows / MSVC 双轨验证，运行构建与 CTest，并生成 `test_report.md`。

## 构建与测试

需要 Windows、Visual Studio 2022 或更新版本的 MSVC C++ 工具，以及包含 C++/WinRT 头文件的 Windows SDK。

```powershell
cmake -S . -B out/test -A x64 -DBUILD_TESTING=ON
cmake --build out/test --config Release --parallel
ctest --test-dir out/test -C Release --output-on-failure --no-tests=error
```

自动测试覆盖标签属性值拆分、空白清理、空标签过滤、去重与排序，以及 `desktop.ini` 标签行的解析、序列化、替换、缺失时追加和 CRLF 保留。它不模拟 Explorer、Toast、快捷方式或 TreeView 的真实桌面交互；这些部分仍需在 Windows 桌面环境中人工验证。
