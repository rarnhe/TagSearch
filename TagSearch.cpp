#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <shlobj.h>
#include <random>
#include <filesystem>
#include <cwctype>
#include <set>

#include <algorithm>
#include <locale>
#include <utility>

#include "TagSearch.h"

#pragma comment(lib, "comctl32.lib")

//为了使用getproperty
#include <propsys.h>
//#include <propvarutil.h>
//#include <propkey.h>
#pragma comment(lib, "propsys.lib")

//异步运行
#include <thread>
#include <future>

//获取缩放率
static double getScaleFactor() {
    HDC hdc = GetDC(nullptr);
    if (!hdc) return 1.0f; // 容错处理

    // 步骤1: 获取实际分辨率（物理分辨率）
    int actualWidth = GetDeviceCaps(hdc, DESKTOPHORZRES); // 物理宽度
    //int actualHeight = GetDeviceCaps(hdc, DESKTOPVERTRES); // 物理高度

    // 步骤2: 获取逻辑分辨率（系统缩放后的分辨率）
    int logicalWidth = GetSystemMetrics(SM_CXSCREEN);

    ReleaseDC(nullptr, hdc);

    // 容错：避免除以零
    if (logicalWidth <= 0) return 1.0f;

    // 步骤3: 计算缩放率（物理分辨率 / 逻辑分辨率）
    return static_cast<double>(actualWidth) / logicalWidth;
}

//获取taskbar的高度
static int GetTaskbarHeight() {
    // 将 "Shell_TrayWnd" 从 char* 转换为 LPCWSTR
    std::wstring wstr;
    int len = MultiByteToWideChar(CP_ACP, 0, "Shell_TrayWnd", -1, nullptr, 0);
    wstr.resize(len);
    MultiByteToWideChar(CP_ACP, 0, "Shell_TrayWnd", -1, &wstr[0], len);

    HWND hTaskbar = FindWindow(wstr.c_str(), NULL);
    if (hTaskbar == NULL) {
        return 0; // 未找到任务栏窗口
    }

    RECT taskbarRect;
    GetWindowRect(hTaskbar, &taskbarRect);
    return static_cast<int>(taskbarRect.bottom - taskbarRect.top);
}

#include <propkey.h>
#include <propvarutil.h>

// WinRT 通知相关
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

// 链接库
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "propsys.lib")

using namespace winrt;
using namespace Windows::UI::Notifications;
using namespace Windows::Data::Xml::Dom;

const std::wstring MY_APP_ID = L"MyCustom.HookTool.X";

// --- 修正后的 Shell 注册函数 ---
HRESULT RegisterAppShortcut() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);

    // 转为路径对象并指向同目录下的 ini
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::wstring desktopIniPath = (exeDir / L"desktop.ini").wstring();

    //wchar_t programsPath[MAX_PATH];
    // 使用 SHGetKnownFolderPath 替代过时的 SHGetSpecialFolderPath
    PWSTR pszPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Programs, 0, NULL, &pszPath);
    if (FAILED(hr)) return hr;

    std::wstring exeName = std::filesystem::path(exePath).stem().wstring();
    std::wstring lnkPath = std::wstring(pszPath) + L"\\" + exeName + L".lnk";
    CoTaskMemFree(pszPath);

    if (PathFileExistsW(lnkPath.c_str())) return S_OK;

    IShellLinkW* psl;
    hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (SUCCEEDED(hr)) {
        psl->SetPath(exePath);

        // 动态读取图标逻辑（使用局部变量 finalIconPath 和 finalIconIndex）
        std::wstring finalIconPath = L"";
        int finalIconIndex = 0;

        std::ifstream iniFile(desktopIniPath, std::ios::in | std::ios::binary);
        if (iniFile.is_open()) {
            std::string line;
            while (std::getline(iniFile, line)) {
                // ANSI 转 wstring
                int size_needed = MultiByteToWideChar(CP_ACP, 0, line.c_str(), (int)line.size(), NULL, 0);
                std::wstring wline(size_needed, 0);
                MultiByteToWideChar(CP_ACP, 0, line.c_str(), (int)line.size(), &wline[0], size_needed);

                size_t pos = wline.find(L"IconResource=");
                if (pos != std::wstring::npos) {
                    std::wstring resValue = wline.substr(pos + 13);
                    size_t commaPos = resValue.find_last_of(L',');
                    if (commaPos != std::wstring::npos) {
                        finalIconPath = resValue.substr(0, commaPos);
                        try {
                            finalIconIndex = std::stoi(resValue.substr(commaPos + 1));
                        }
                        catch (...) { finalIconIndex = 0; }
                    }
                    break;
                }
            }
            iniFile.close();
        }

        //// --- 开始 MessageBox 验证 ---
        //std::wstring validationMsg = L"Desktop.ini 解析结果：\n\n";
        //validationMsg += L"文件路径: " + desktopIniPath + L"\n";
        //validationMsg += L"解析状态: " + std::wstring(finalIconPath.empty() ? L"未找到配置" : L"解析成功") + L"\n\n";

        //if (!finalIconPath.empty()) {
        //	validationMsg += L"图标路径 (Raw): " + finalIconPath + L"\n";
        //	validationMsg += L"图标索引: " + std::to_wstring(finalIconIndex) + L"\n";

        //	// 额外验证：展开环境变量（处理 %SystemRoot% 等情况）
        //	wchar_t expandedPath[MAX_PATH];
        //	ExpandEnvironmentStringsW(finalIconPath.c_str(), expandedPath, MAX_PATH);
        //	validationMsg += L"实际物理路径: " + std::wstring(expandedPath);
        //}

        //MessageBoxW(NULL, validationMsg.c_str(), L"Debug - 图标解析验证", MB_OK | MB_ICONINFORMATION);
        //// --- 验证结束 ---

        // 4. 应用图标设置
        if (!finalIconPath.empty()) {
            psl->SetIconLocation(finalIconPath.c_str(), finalIconIndex);
        }
        //else {
        //	// 回退方案：默认 shell32 图标
        //	psl->SetIconLocation(L"C:\\Windows\\System32\\shell32.dll", 293);
        //}

        IPropertyStore* pps;
        hr = psl->QueryInterface(IID_PPV_ARGS(&pps));
        if (SUCCEEDED(hr)) {
            PROPVARIANT var;
            // 确保使用 InitPropVariantFromString (Unicode 版本)
            hr = InitPropVariantFromString(MY_APP_ID.c_str(), &var);
            if (SUCCEEDED(hr)) {
                hr = pps->SetValue(PKEY_AppUserModel_ID, var);
                pps->Commit();
                PropVariantClear(&var);
            }
            pps->Release();
        }

        IPersistFile* ppf;
        hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
        if (SUCCEEDED(hr)) {
            hr = ppf->Save(lnkPath.c_str(), TRUE);
            ppf->Release();
            // 只有保存成功后才刷新系统缓存
            if (SUCCEEDED(hr)) {
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
            }
        }
        psl->Release();
    }
    return hr;
}

// --- 修正后的通知函数 ---
static void ShowWin11NativeNotification(const std::wstring& title, const std::wstring& message)
{
    try {
        std::wstring xmlPayload =
            L"<toast><visual><binding template='ToastGeneric'>"
            L"<text>" + title + L"</text>"
            L"<text>" + message + L"</text>"
            L"</binding></visual></toast>";

        XmlDocument toastXml;
        toastXml.LoadXml(xmlPayload);

        // 显式指定类型避免 auto 推导错误
        ToastNotifier notifier = ToastNotificationManager::CreateToastNotifier(MY_APP_ID);
        ToastNotification notification(toastXml);

        notifier.Show(notification);
    }
    catch (...) {
        // 静默处理或输出调试信息
    }
}

//清理不可见字符的函数
static std::wstring CleanInvisibleChars(const std::wstring& tag) {
    std::wstring cleaned;
    std::copy_if(tag.begin(), tag.end(), std::back_inserter(cleaned), [](wchar_t c) {
        return !std::iswcntrl(c);
        });
    return cleaned;
}

// 读取指定 INI 文件中的标签
static void ReadTagsFromIniFile(const std::wstring& filePath, std::vector<std::wstring>& tags) {
    std::ifstream iniFile(filePath, std::ios::in | std::ios::binary);
    if (!iniFile.is_open()) {
        return; // 直接返回，不退出程序
    }

    std::string line;

    // 逐行读取文件
    while (std::getline(iniFile, line)) {
        if (line.empty()) {
            continue;
        }

        // 将 ANSI 字符串转换为 std::wstring，并检查转换失败的情况。
        const int sizeNeeded = MultiByteToWideChar(
            CP_ACP, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
        if (sizeNeeded <= 0) {
            continue;
        }

        std::wstring wline(static_cast<std::size_t>(sizeNeeded), L'\0');
        if (MultiByteToWideChar(
                CP_ACP, 0, line.c_str(), static_cast<int>(line.size()),
                wline.data(), sizeNeeded) <= 0) {
            continue;
        }

        auto parsedTags = tagsearch::ParseDesktopIniTagsLine(wline);
        if (parsedTags.has_value()) {
            tags.insert(tags.end(), parsedTags->begin(), parsedTags->end());
            break;
        }
    }

    iniFile.close();
}

//如果采用读取每个文件的方法
static HRESULT GetPropertyStore(PCWSTR pszFilename, GETPROPERTYSTOREFLAGS gpsFlags, IPropertyStore** ppps)
{
    WCHAR szExpanded[MAX_PATH];
    HRESULT hr = ExpandEnvironmentStrings(pszFilename, szExpanded, ARRAYSIZE(szExpanded)) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    if (SUCCEEDED(hr))
    {
        WCHAR szAbsPath[MAX_PATH];
        hr = _wfullpath(szAbsPath, szExpanded, ARRAYSIZE(szAbsPath)) ? S_OK : E_FAIL;
        if (SUCCEEDED(hr))
        {
            hr = SHGetPropertyStoreFromParsingName(szAbsPath, NULL, gpsFlags, IID_PPV_ARGS(ppps));
        }
    }
    return hr;
}

static std::wstring GetPropertyValueW(PCWSTR pszFilename, PCWSTR pszCanonicalName)
{
    PROPERTYKEY key;
    HRESULT hr = PSGetPropertyKeyFromName(pszCanonicalName, &key);
    if (SUCCEEDED(hr))
    {
        IPropertyStore* pps = nullptr;
        hr = GetPropertyStore(pszFilename, GPS_DEFAULT, &pps);
        if (SUCCEEDED(hr))
        {
            PROPVARIANT propvarValue = { 0 };
            hr = pps->GetValue(key, &propvarValue);
            if (SUCCEEDED(hr))
            {
                PWSTR pszDisplayValue = nullptr;
                hr = PSFormatForDisplayAlloc(key, propvarValue, PDFF_DEFAULT, &pszDisplayValue);
                if (SUCCEEDED(hr))
                {
                    std::wstring result(pszDisplayValue);
                    CoTaskMemFree(pszDisplayValue);
                    PropVariantClear(&propvarValue);
                    pps->Release();
                    return result;
                }
                PropVariantClear(&propvarValue);
            }
            pps->Release();
        }
    }
    return L"";
}

static std::vector<std::wstring> defineCheckFolders() {
    std::vector<std::wstring> paths;
    PWSTR pszDesktopPath = nullptr; // 声明一次，并初始化为 nullptr
    PWSTR pszDownloadPath = nullptr;

    // 1. 获取桌面路径
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &pszDesktopPath))) {
        paths.push_back(pszDesktopPath);
        CoTaskMemFree(pszDesktopPath);
    }
    // ⚠️ 提示：您可能需要添加错误处理，例如 if/else 打印失败信息

    // 2. 获取下载路径
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pszDownloadPath))) {
        paths.push_back(pszDownloadPath);
        CoTaskMemFree(pszDownloadPath);
        //pszPath = nullptr; // 重置 pszPath
    }

    // 检查是否成功获取了任何路径
    if (paths.empty()) {
        // 如果两个路径都获取失败，可以提前返回
        // 建议：输出一个错误信息
        return {};
    }

    return paths;
}

static bool RenameExeIfNeeded(std::wstring newName) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::filesystem::path currentPath = std::filesystem::current_path();
    //std::wstring folderName = currentPath.filename().wstring();
    std::wstring exeName = std::filesystem::path(exePath).filename().wstring();

    if (exeName != newName) {
        std::wstring newExePath = (currentPath / newName).wstring() + L".exe";
        if (MoveFileW(exePath, newExePath.c_str())) {
            //将exe名字改为文件夹的名字
            return true;
        }
        else {
            ShowWin11NativeNotification((L".\\" + newName + L".exe").c_str(), L"找到exe，衰仔，不要再等了");
            return false;
        }
    }
    return true;
}

static std::vector<std::wstring> UpdateTags(std::vector<std::wstring> paths) {

    //int item = 0;
    std::vector<std::wstring> allTags; // 所有标签

    // 拼接路径
    std::wstring folderName;
    for (size_t i = 0; i < paths.size(); ++i) {
        // 从路径中提取文件夹名
        std::wstring folder = std::filesystem::path(paths[i]).filename().wstring();
        folderName += folder;
        if (i < paths.size() - 1) {
            folderName += L"&";  // 在文件夹名称之间添加 "&"
        }
    }

    PWSTR pszDesktopPath = nullptr; // 声明一次，并初始化为 nullptr
    std::filesystem::path folderPath = {};
    std::wstring folderDesktopini = {};

    // 1. 获取桌面路径
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &pszDesktopPath))) {
        folderDesktopini = std::wstring(pszDesktopPath) + L"\\" + folderName + L"\\desktop.ini";
        // 创建文件夹
        //folderPath = std::filesystem::path(folderDesktopini).parent_path();
        CoTaskMemFree(pszDesktopPath);
    }

    if (!std::filesystem::exists(folderDesktopini)) {
        // 迭代路径列表 (现在 paths 包含所有成功获取的路径)
        for (const auto& path : paths) {
            // ⚠️ 额外提醒：std::filesystem::directory_iterator 可能会抛出异常
            // 如果 path 无效或没有权限，您应该将其包装在 try-catch 块中。
            //ShowWin11NativeNotification(path.c_str(), L"is working");
            try {
                for (const auto& entry : std::filesystem::directory_iterator(path)) {
                    if (entry.is_regular_file()) { // 确保是常规文件
                        std::wstring tagStr = GetPropertyValueW(entry.path().c_str(), L"System.Keywords");
                        //if (!tagStr.empty()) {
                        //    allTags.push_back(tagStr);
                        //    //item = item + 1;
                        //}
                        if (!tagStr.empty()) {
                            auto fileTags = tagsearch::SplitKeywords(tagStr);
                            allTags.insert(allTags.end(), fileTags.begin(), fileTags.end());
                        }
                    }
                }
            }
            catch (...) {
                // 可以选择打印错误信息，然后继续下一个路径
                // 例如：std::wcerr << L"遍历路径失败: " << path << L", 错误: " << e.what() << std::endl;
            }
        }
    }
    else {
        ReadTagsFromIniFile(folderDesktopini, allTags);
    }

    return tagsearch::NormalizeTags(std::move(allTags));
}

static std::vector<std::wstring> SaveDesktopIni(std::vector<std::wstring> paths, HWND hTreeView) {
    std::wstring folderName;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::wstring folder = std::filesystem::path(paths[i]).filename().wstring();
        folderName += folder;
        if (i < paths.size() - 1) {
            folderName += L"&";
        }
    }

    PWSTR pszDesktopPath = nullptr; // 声明一次，并初始化为 nullptr
    std::wstring folderDesktopini = {};
    std::filesystem::path folderPath = {};

    // 1. 获取桌面路径
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &pszDesktopPath))) {
        folderDesktopini = std::wstring(pszDesktopPath) + L"\\" + folderName + L"\\desktop.ini";
        // 创建文件夹
        folderPath = std::filesystem::path(folderDesktopini).parent_path();
        CoTaskMemFree(pszDesktopPath);
    }

    if (!std::filesystem::exists(folderPath)) {
        try {
            std::filesystem::create_directories(folderPath);
            //SetFileAttributesW(folderPath.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY);
            SetFileAttributesW(folderPath.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        }
        catch (...) {
            //std::wcerr << L"创建目录时出错: " << e.what() << std::endl;
            return {};
        }
    }

    //从当前treeview获取item list更新
    std::wstring accumulatedText;
    HTREEITEM hItem = TreeView_GetRoot(hTreeView);
    while (hItem) {
        TVITEM item = { 0 };
        item.mask = TVIF_TEXT | TVIF_HANDLE;
        item.hItem = hItem;
        item.cchTextMax = 256; // Set a maximum size for the text
        wchar_t buffer[256] = { 0 };
        item.pszText = buffer;

        // Get the text of the current node
        if (TreeView_GetItem(hTreeView, &item)) {
            buffer[item.cchTextMax - 1] = L'\0'; // 确保字符串零终止
            if (!accumulatedText.empty()) {
                accumulatedText += L";"; // Add separator for multiple nodes
            }
            accumulatedText += buffer; // Accumulate the node text
        }

        // 移动到下一个项
        hItem = TreeView_GetNextItem(hTreeView, hItem, TVGN_NEXT); // 正确的调用方式
    }

    const std::vector<std::wstring> selectedTags =
        tagsearch::SplitKeywords(accumulatedText);
    const std::wstring tagsLine =
        tagsearch::FormatDesktopIniTagsLine(selectedTags);

    if (!std::filesystem::exists(folderDesktopini)) { //如果文件不存在
        std::wofstream iniFile(folderDesktopini, std::ios::app);
        if (iniFile.is_open()) {
            iniFile.imbue(std::locale("zh-CN")); // 或者可以使用 file.imbue(std::locale("zh-CN.cp936"));
            iniFile << L"[.ShellClassInfo]" << std::endl;
            iniFile << L"{F29F85E0-4FF9-1068-AB91-08002B27B3D9}" << std::endl;

            // 写入格式化后的标签属性行
            iniFile << tagsLine << std::endl;
            iniFile.close();
        }
        //ShowWin11NativeNotification(folderDesktopini.c_str(), L"is saving");
    }
    else {
        // 更改文件属性为可编辑
        DWORD attributes = GetFileAttributesW(folderDesktopini.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            // 移除 FILE_ATTRIBUTE_HIDDEN 和 FILE_ATTRIBUTE_SYSTEM
            attributes &= ~FILE_ATTRIBUTE_HIDDEN;
            attributes &= ~FILE_ATTRIBUTE_SYSTEM;
            SetFileAttributesW(folderDesktopini.c_str(), attributes);
        }

        // 读取已有 desktop.ini，再由可单元测试的核心函数更新标签行。
        std::wifstream iniFile(folderDesktopini);
        std::wstring line;
        std::wstring existingContent;

        while (std::getline(iniFile, line)) {
            existingContent += line + L"\n";
        }
        iniFile.close();

        const std::wstring updatedContent =
            tagsearch::UpdateDesktopIniTagsLine(existingContent, selectedTags);

        // 覆盖写回更新后的 desktop.ini，保留其他 Shell 配置项。
        std::wofstream outFile(folderDesktopini, std::ios::out | std::ios::trunc);
        outFile.imbue(std::locale("zh-CN")); // 设置区域
        if (outFile.is_open()) {
            outFile << updatedContent;
            outFile.close();
        }
        else {
            ShowWin11NativeNotification(folderDesktopini.c_str(), L"can not write");
        }
    }
    // 给个提示
    ShowWin11NativeNotification(folderDesktopini.c_str(), L"is updating");
    SetFileAttributesW(folderDesktopini.c_str(), FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN); //desktop.ini的属性
    //// 在文件创建或修改后添加
    //SetFileAttributesW(folderDesktopini.c_str(),FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    return paths; // 确保返回值
}

class CCoInitialize {
public:
    CCoInitialize() : m_hr(CoInitialize(NULL)) {}
    ~CCoInitialize() { if (SUCCEEDED(m_hr)) CoUninitialize(); }
    operator HRESULT() const { return m_hr; }
    HRESULT m_hr;
};

CCoInitialize comInit; // 初始化 COM

std::vector<std::wstring> folders = defineCheckFolders();

std::vector<std::wstring> tags = UpdateTags(folders);

const int hItemCount = static_cast<int>(tags.size());

// 声明控件句柄和字体句柄
HWND hTreeView;
HFONT hFont;

struct WindowMetrics {
    int TreeViewWidth;
    int TreeViewHeight;
    int WindowWidth;
    int WindowHeight;
    int Bolder;
    HFONT hFont;
};

int primaryScreenWidth = GetSystemMetrics(SM_CXSCREEN);
int primaryScreenHeight = GetSystemMetrics(SM_CYSCREEN); // 获取主屏幕高度

double scaleFactor = getScaleFactor();
int notificationCenterWidth = 350; //win11下选择350，win10下选择390,恰为通知中心宽度
int taskbarHeight = GetTaskbarHeight();

static WindowMetrics CalculateWindowMetrics() {
    int fontSize = 23;

    WindowMetrics metrics = { 0 };
    metrics.Bolder = 10; //框的边界宽

    metrics.WindowWidth = static_cast<int>(notificationCenterWidth * scaleFactor);
    metrics.WindowHeight = static_cast<int>((primaryScreenHeight - taskbarHeight) * scaleFactor);
    metrics.TreeViewWidth = static_cast<int>((notificationCenterWidth - 13) * scaleFactor) - metrics.Bolder * 2; // 13 magic number
    metrics.TreeViewHeight = static_cast<int>((primaryScreenHeight - taskbarHeight - 35) * scaleFactor) - metrics.Bolder * 2; //35 magic number

    //std::string strScale = std::to_string(scaleFactor);
    //ShowWin11NativeNotification(std::wstring(strScale.begin(), strScale.end()).c_str(), L"衰仔，不要再等了");

    // 创建字体
    metrics.hFont = CreateFont(
        (int)(fontSize * scaleFactor), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_QUALITY, L"Segoe UI");

    return metrics;
}

WindowMetrics metrics = CalculateWindowMetrics();

HWND hWndMain; // 全局变量，用于存储主窗口句柄
//HTREEITEM hSelectedItem = NULL;

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        // 创建 TreeView 控件
        hTreeView = CreateWindowEx(
            0,
            WC_TREEVIEW,
            L"",
            WS_VISIBLE | WS_CHILD | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
            metrics.Bolder, metrics.Bolder, metrics.TreeViewWidth, metrics.TreeViewHeight,
            hwnd,
            (HMENU)1,
            NULL,
            NULL
        );

        // 设置 TreeView 字体
        SendMessage(hTreeView, WM_SETFONT, (WPARAM)metrics.hFont, TRUE);

        // 初始化 TreeView
        TVINSERTSTRUCT tvis = { 0 };
        tvis.hParent = TVI_ROOT;
        tvis.hInsertAfter = TVI_LAST;
        tvis.item.mask = TVIF_TEXT;

        for (int i = 0; i < hItemCount; i++) {
            // 分配内存给每个根节点文本
            wchar_t* itemText = new wchar_t[50];
            wcscpy_s(itemText, 50, tags[i].c_str()); // 使用 .c_str() 获取 const wchar_t*

            tvis.item.pszText = itemText;

            // 插入为根节点
            HTREEITEM hItem = TreeView_InsertItem(hTreeView, &tvis);
            delete[] itemText; // 释放内存
        }

        break;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        // 绘制树视图控件的背景
        FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
        EndPaint(hwnd, &ps);
    }
    break;

    case WM_NOTIFY:
        if (wParam == 1) { // 确保这是我们的TreeView控件
            NMHDR* pnmh = (NMHDR*)lParam;
            // 处理节点选择改变
            if (pnmh->code == TVN_SELCHANGED) {
                NMTREEVIEW* pnmv = (NMTREEVIEW*)lParam;
                if (pnmv->action == TVC_BYMOUSE) { // 确保是鼠标操作
                    // 获取选中的节点文本
                    HTREEITEM hSelectedItem = pnmv->itemNew.hItem;
                    if (hSelectedItem == NULL) {
                        return 0;
                    }

                    // 获取节点文本
                    TVITEM tvi = {};
                    tvi.hItem = hSelectedItem;
                    tvi.mask = TVIF_TEXT;
                    tvi.pszText = new wchar_t[50];
                    tvi.cchTextMax = 50;
                    TreeView_GetItem(hTreeView, &tvi);

                    std::wstring searchCmd = L"search-ms:query=标记：" + std::wstring(tvi.pszText) +
                        L"&crumb=location:" + L"%userprofile%\\Downloads" +
                        L"&crumb=location:" + L"%userprofile%\\Desktop";

                    std::wstring command = L"/c start explorer.exe \"" + searchCmd + L"\"";
                    ShellExecute(hwnd, L"open", L"cmd.exe", command.c_str(), NULL, SW_HIDE);

                    //// 获取Documents路径并执行 ShellExecute
                    //PWSTR pszPath;
                    //if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &pszPath))) {
                    //    std::wstring documentsPath(pszPath);
                    //    CoTaskMemFree(pszPath);

                    //    std::wstring searchCmd = L"search-ms:query=标记：" + searchText +
                    //        L"&crumb=location:" + documentsPath;

                    //    std::wstring command = L"/c start explorer.exe \"" + searchCmd + L"\"";
                    //    ShellExecute(hwnd, L"open", L"cmd.exe", command.c_str(), NULL, SW_HIDE);
                    //}

                    delete[] tvi.pszText;
                }
            }
            // 处理左键单击事件
            else if (pnmh->code == NM_CLICK) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hTreeView, &pt);

                HTREEITEM hItem = TreeView_HitTest(hTreeView, &pt);
                if (hItem) {
                    // 更新当前选择项
                    TreeView_SelectItem(hTreeView, hItem);

                    // 获取选中的节点文本
                    TVITEM tvi = {};
                    tvi.hItem = hItem;
                    tvi.mask = TVIF_TEXT;
                    tvi.pszText = new wchar_t[50];
                    tvi.cchTextMax = 50;
                    TreeView_GetItem(hTreeView, &tvi);

                    std::wstring searchCmd = L"search-ms:query=标记：" + std::wstring(tvi.pszText) +
                        L"&crumb=location:" + L"%userprofile%\\Downloads" +
                        L"&crumb=location:" + L"%userprofile%\\Desktop";

                    std::wstring command = L"/c start explorer.exe \"" + searchCmd + L"\"";
                    ShellExecute(hwnd, L"open", L"cmd.exe", command.c_str(), NULL, SW_HIDE);

                    //// 获取Documents路径并执行 ShellExecute
                    //PWSTR pszPath;
                    //if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &pszPath))) {
                    //    std::wstring documentsPath(pszPath);
                    //    CoTaskMemFree(pszPath);

                    //    std::wstring searchCmd = L"search-ms:query=标记：" + std::wstring(tvi.pszText) +
                    //        L"&crumb=location:" + documentsPath;

                    //    std::wstring command = L"/c start explorer.exe \"" + searchCmd + L"\"";
                    //    ShellExecute(hwnd, L"open", L"cmd.exe", command.c_str(), NULL, SW_HIDE);
                    //}

                    delete[] tvi.pszText;
                }
            }
        }
        break;

    case WM_CONTEXTMENU: {
        if ((HWND)wParam == hTreeView) {
            POINT pt;
            GetCursorPos(&pt);
            int xPop = pt.x;
            int yPop = pt.y;
            ScreenToClient(hTreeView, &pt);

            HTREEITEM hItem = TreeView_HitTest(hTreeView, &pt);
            if (hItem) {
                // 更新当前选择项目
                TreeView_SelectItem(hTreeView, hItem);

                // 获取选中的节点文本
                TVITEM tvi = {};
                tvi.hItem = hItem;
                tvi.mask = TVIF_TEXT;
                tvi.pszText = new wchar_t[50];
                tvi.cchTextMax = 50;
                TreeView_GetItem(hTreeView, &tvi);

                // 创建菜单
                HMENU menu = CreatePopupMenu();
                AppendMenu(menu, MF_STRING, 1, L"分别打开"); // 添加菜单项
                AppendMenu(menu, MF_STRING, 2, L"注入颜色"); // 添加菜单项

                // 显示弹出菜单
                int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, xPop, yPop, 0, hwnd, NULL);

                // 处理菜单命令
                if (command == 1) {
                    // 添加新项的逻辑
                    // 例如：TreeView_InsertItem(...)
                }
                else if (command == 2) {
                    // 删除选中项的逻辑
                    TreeView_DeleteItem(hTreeView, hItem);
                    // 设置颜色逻辑
                    //TreeView_SetTextColor(hTreeView, RGB(0, 0, 255));
                }

                // 释放节点文本内存
                delete[] tvi.pszText;
                DestroyMenu(menu); // 清理菜单资源
            }
        }
        break;
    }

    case WM_DESTROY:
        DeleteObject(hFont); // 删除字体对象
        SaveDesktopIni(folders, hTreeView);
        PostQuitMessage(0);
        break;

        //case WM_NCMBUTTONDOWN: {
        //    if (wParam == HTCAPTION) {
        //        return 0;
        //    } break;
        //}
    case WM_SYSCOMMAND: {
        // 检查是否是移动命令
        if ((wParam & 0xFFF0) == SC_MOVE) {
            // 忽略移动命令
            return 0; // 处理完成
        }
        else if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            // 允许最小化
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        else if ((wParam & 0xFFF0) == SC_CLOSE) {
            // 允许关闭
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        else if ((wParam & 0xFFF0) == SC_RESTORE) {
            // 允许恢复窗口
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
        break; // 继续处理其他命令
    }
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow
) {
    // 1. 初始化 WinRT 环境 (用于原生通知)
    winrt::init_apartment();

    // 2. 注册应用快捷方式
    RegisterAppShortcut();

    //为了加一个标题
    std::vector<std::wstring> paths = defineCheckFolders();
    std::wstring folderName;
    for (size_t i = 0; i < paths.size(); ++i) {
        // 从路径中提取文件夹名
        std::wstring folder = std::filesystem::path(paths[i]).filename().wstring();
        folderName += folder;
        if (i < paths.size() - 1) {
            folderName += L"&";  // 在文件夹名称之间添加 "&"
        }
    }

    if (RenameExeIfNeeded(folderName)) {
        SetProcessDPIAware();
        INITCOMMONCONTROLSEX icex = {};
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icex);

        const wchar_t CLASS_NAME[] = L"Sample Window Class";

        WNDCLASS wc = {};
        wc.lpfnWndProc = WindowProc; // 设置窗口过程
        wc.hInstance = hInstance;
        wc.lpszClassName = CLASS_NAME;

        RegisterClass(&wc);

        hWndMain = CreateWindowEx(
            0, CLASS_NAME,
            folderName.c_str(), //这里可以添加程序标题
            //WS_OVERLAPPEDWINDOW & ~(WS_SIZEBOX | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU),
            WS_POPUP | WS_CAPTION | WS_MINIMIZEBOX,
            static_cast<int>(primaryScreenWidth * scaleFactor) - metrics.WindowWidth,
            0,
            metrics.WindowWidth,
            metrics.WindowHeight,
            NULL, NULL, hInstance, NULL);

        //std::string strScale = std::to_string(scaleFactor);
        //ShowWin11NativeNotification(std::wstring(strScale.begin(), strScale.end()).c_str(), L"衰仔，不要再等了");

        ShowWindow(hWndMain, nCmdShow);

        //// 安装鼠标钩子
        //hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, MouseHookProc, NULL, 0);

        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        //// 卸载钩子
        //UnhookWindowsHookEx(hMouseHook);
    }

    return 0;
}