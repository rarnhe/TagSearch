param(
    [string]$BuildDirectory = "out/test",
    [string]$Configure = "success",
    [string]$Build = "success",
    [string]$Test = "success",
    [string]$OutputPath = "test_report.md",
    [string]$LogDirectory = ""
)

$ErrorActionPreference = "Stop"
$cases = @()
$junitError = ""
$junit = Join-Path $BuildDirectory "test-results.xml"

if (Test-Path -LiteralPath $junit) {
    try {
        [xml]$document = Get-Content -LiteralPath $junit -Raw
        $cases = @($document.SelectNodes("//testcase"))
    }
    catch {
        $junitError = $_.Exception.Message
    }
}

$failed = @($cases | Where-Object { $_.SelectSingleNode("failure") -or $_.SelectSingleNode("error") })
$skipped = @($cases | Where-Object { $_.SelectSingleNode("skipped") })
$passed = [Math]::Max(0, $cases.Count - $failed.Count - $skipped.Count)
$ok = $Configure -eq "success" -and $Build -eq "success" -and $Test -eq "success" -and $cases.Count -gt 0 -and $failed.Count -eq 0 -and $skipped.Count -eq 0
$status = if ($ok) { "通过 (Passed)" } else { "失败或未完整执行 (Failed)" }

$revision = "unknown"
$revisionError = ""
try {
    $revisionOutput = & git rev-parse HEAD 2>&1
    if ($LASTEXITCODE -eq 0) {
        $revision = ($revisionOutput | Out-String).Trim()
    }
    else {
        $revisionError = ($revisionOutput | Out-String).Trim()
    }
}
catch {
    $revisionError = $_.Exception.Message
}

$run = if ($env:GITHUB_RUN_ID) {
    "$env:GITHUB_SERVER_URL/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID"
}
else {
    "本地 Windows 执行（含当前工作区测试改动）"
}

$lines = [System.Collections.Generic.List[string]]::new()
@(
    "# TagSearch C++ Windows 测试报告", "",
    "- 状态：$status",
    "- 时间（UTC）：$([DateTime]::UtcNow.ToString('yyyy-MM-dd HH:mm:ss'))",
    "- 测试源码基准提交：$revision",
    "- 执行记录：$run",
    "- 平台：Windows / MSVC / C++20 / Windows SDK",
    "- 阶段：Configure=$Configure / Build=$Build / CTest=$Test",
    "- 统计：通过 $passed / 失败 $($failed.Count) / 跳过 $($skipped.Count) / 共 $($cases.Count)", ""
) | ForEach-Object { $lines.Add($_) }

if ($revisionError) {
    $lines.Add("- Git revision 读取错误：$([System.Net.WebUtility]::HtmlEncode($revisionError))")
}

$lines.Add("## GitHub Actions 阶段状态")
$lines.Add("")
$lines.Add("| 阶段 | 结果 |")
$lines.Add("| --- | --- |")
$phaseStates = [ordered]@{ Configure = $Configure; Build = $Build; CTest = $Test }
foreach ($phase in $phaseStates.Keys) {
    $lines.Add("| $phase | $($phaseStates[$phase]) |")
}

$lines.Add("")
$lines.Add("## 测试用例结果")
$lines.Add("")
$lines.Add("| 测试 | 结果 |")
$lines.Add("| --- | --- |")
foreach ($case in $cases) {
    $outcome = if ($case.SelectSingleNode("failure") -or $case.SelectSingleNode("error")) {
        "失败"
    }
    elseif ($case.SelectSingleNode("skipped")) {
        "跳过"
    }
    else {
        "通过"
    }
    $lines.Add("| $($case.name) | $outcome |")
}
if ($cases.Count -eq 0) {
    $lines.Add("| 未获得 CTest JUnit 结果 | 未执行 |")
}

$lines.Add("")
$lines.Add("## 失败详情")
if ($failed.Count -eq 0 -and !$ok) {
    $lines.Add("没有可解析的 CTest 用例级错误；整体失败原因请查看下面的阶段输出。")
}
foreach ($case in $failed) {
    $lines.Add("")
    $lines.Add("### $($case.name)")
    $lines.Add("<pre>")
    $lines.Add([System.Net.WebUtility]::HtmlEncode($case.InnerText))
    $lines.Add("</pre>")
}
if ($junitError) {
    $lines.Add("")
    $lines.Add("CTest JUnit 文件读取失败：")
    $lines.Add("<pre>$([System.Net.WebUtility]::HtmlEncode($junitError))</pre>")
}

if ($LogDirectory) {
    foreach ($phase in $phaseStates.Keys) {
        $outcome = $phaseStates[$phase]
        if ($outcome -eq "success") { continue }

        $log = Join-Path $LogDirectory "$($phase.ToLowerInvariant()).log"
        $output = ""
        $readError = ""
        if (Test-Path -LiteralPath $log) {
            try {
                $output = (Get-Content -LiteralPath $log -Tail 80) -join [Environment]::NewLine
            }
            catch {
                $readError = $_.Exception.Message
            }
        }

        $lines.Add("")
        $lines.Add("### $phase 阶段输出")
        if ($output) {
            $exitMatch = [regex]::Match($output, '(?im)^\s*Exit code:\s*(-?\d+)\s*$')
            if ($exitMatch.Success) {
                $lines.Add("进程退出码：$($exitMatch.Groups[1].Value)")
            }
            $lines.Add("<pre>$([System.Net.WebUtility]::HtmlEncode($output))</pre>")
        }
        elseif ($readError) {
            $lines.Add("<pre>读取日志失败：$([System.Net.WebUtility]::HtmlEncode($readError))</pre>")
        }
        else {
            $lines.Add("日志文件不存在：$log")
        }
    }
}

$lines.Add("")
$lines.Add("## 自动测试覆盖范围")
$lines.Add("验证标签拆分、空白清理、去重与排序、默认目录和带引号的 Unicode 路径、从嵌套 JPEG 读取真实 System.Keywords 属性、重新扫描后的变化，窗口重叠判断（含边缘接触和间接重叠），Windows Shell 多目录和多标签 AND 搜索条件的创建，以及 TreeView 的无参数启动、隐藏所有者和单实例恢复。")
$lines.Add("")
$lines.Add("## 人工验证边界")
$lines.Add("Windows CI 构建 TreeView 和 Caption 两个 EXE。Alt+Tab、Win+Tab、窗口拖动、DPI、主题及 Explorer 实际返回的搜索结果需要在交互式 Windows 桌面验证。")

$content = $lines -join [Environment]::NewLine
$outputDirectory = Split-Path -Parent $OutputPath
if ($outputDirectory -and !(Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
Set-Content -LiteralPath $OutputPath -Value $content -Encoding utf8

if ($env:GITHUB_STEP_SUMMARY) {
    try {
        Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY -Value $content -Encoding utf8
    }
    catch {
        Write-Warning "The Markdown report was saved, but Actions Summary could not be updated: $($_.Exception.Message)"
    }
}
