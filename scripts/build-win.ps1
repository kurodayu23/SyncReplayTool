<#
.SYNOPSIS
    Build SyncReplayTool (x64, Qt 5.9.6 msvc2015_64).

.DESCRIPTION
    VS2015 cannot compile against modern Windows SDK headers (ucrt/wchar.h uses
    _mm_loadu_si64 -> C3861). So headers/libs are pinned to SDK 10.0.10240.0,
    while rc.exe is taken from 10.0.19041.0 because 10240 ships no x64 rc.exe.

    Every machine-specific path below is a parameter whose default is this
    machine's value. Adjust them for your environment; nothing else needs to
    change.

    The build output directory is **not** hard-coded to any machine: it comes
    from the -BuildRoot parameter, or from the SRTPROJ_BUILDROOT environment
    variable. If neither is given, the script falls back to a 'build'
    directory **next to** the repository (a sibling of the repo folder), so
    that no build artifacts are ever produced inside the source tree.

    The project root defaults to the repository this script lives in, so the
    script keeps working wherever the repository is checked out.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-win.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build-win.ps1 `
        -BuildRoot D:\somewhere\build
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$BuildRoot   = $env:SRTPROJ_BUILDROOT,
    [string]$QtRoot      = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64',
    [string]$Jom         = 'C:\Qt\Tools\QtCreator\bin\jom\jom.exe',
    [string]$VcVars      = 'C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat'
)

$ErrorActionPreference = 'Stop'

if (-not $ProjectRoot) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
# 既没传 -BuildRoot、也没设环境变量 SRTPROJ_BUILDROOT 时：
# 用仓库**旁边**（与仓库同级）的 build 目录，绝不在源码树里生成构建产物。
if (-not $BuildRoot) {
    $BuildRoot = Join-Path (Split-Path -Parent $ProjectRoot) 'build'
}
$Project = Join-Path $ProjectRoot 'SyncReplayTool.pro'

$SdkHeaders = '10.0.10240.0'
$SdkTools   = '10.0.19041.0'

# ---- 构建前门禁：源码里不许有「QStringLiteral + 裸中文」----
#
# 原因：Qt5 的 QStringLiteral 展开成 u"..."（char16_t 字面量），
# MSVC 编译 u"" "中文" 时按**当前代码页**解码窄串再转 UTF-16，
# /utf-8 在 VS2015 上救不了 —— 实测产物里 "data 目录" 存成了
# UTF-16 的 29 94 BC E1 8D 7D（U+9429 U+E1BC U+7D8D），编译期就错了。
# 本工程约定中文一律用 QString::fromUtf8(u8"…")（见 FileTableModel.cpp 顶部注释）。
#
# 扫描器不存在或 python 不可用时**只警告不拦截**，别让别的机器编不了。
$scanTool = Join-Path $ProjectRoot 'scripts\scan_no_cjk_qstringliteral.py'
if (Test-Path $scanTool) {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) {
        & $py.Source $scanTool (Join-Path $ProjectRoot 'src')
        if ($LASTEXITCODE -ne 0) {
            throw '构建前门禁未通过：源码里存在 QStringLiteral + 裸中文（会被 MSVC 按 GBK 误解码）'
        }
    } else {
        Write-Warning '没找到 python，跳过「QStringLiteral + 裸中文」门禁扫描。'
    }
} else {
    Write-Warning '没找到 scripts\scan_no_cjk_qstringliteral.py，跳过该门禁。'
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

# ---- load VS2015 x64 environment ----
$dump = Join-Path $env:TEMP 'srt_vcenv.txt'
cmd /c "`"$VcVars`" x64 >nul 2>&1 && set" | Out-File -FilePath $dump -Encoding OEM
foreach ($line in (Get-Content $dump)) {
    $i = $line.IndexOf('=')
    if ($i -lt 1) { continue }
    $name = $line.Substring(0, $i)
    $val  = $line.Substring($i + 1)
    if ($val -eq '') { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
    else { Set-Item "Env:$name" $val }
}
Remove-Item $dump -Force

# ---- pin SDK version per variable class ----
function Set-SdkVersion($varName, $version) {
    $cur = [Environment]::GetEnvironmentVariable($varName)
    if (-not $cur) { return }
    $fixed = ($cur -split ';' | ForEach-Object {
        if ($_ -match 'Windows Kits\\10') { $_ -replace '10\.0\.\d+\.\d+', $version } else { $_ }
    }) -join ';'
    Set-Item "Env:$varName" $fixed
}

Set-SdkVersion 'INCLUDE' $SdkHeaders
Set-SdkVersion 'LIB'     $SdkHeaders
Set-SdkVersion 'LIBPATH' $SdkHeaders
Set-SdkVersion 'PATH'    $SdkTools

# ---- build ----
Push-Location $BuildRoot
try {
    & "$QtRoot\bin\qmake.exe" $Project CONFIG+=release
    if ($LASTEXITCODE -ne 0) { throw "qmake failed ($LASTEXITCODE)" }

    # ---- 占用的产品进程先清掉 ----
    #
    # exe 被占用时 MSVC 只会报 LNK1104「无法打开文件 release\SyncReplayTool.exe」，
    # 看着像编译错误，实际只是上一轮的产品进程（或从别处启动的实例）还活着。
    # 2026-11-04 实测撞过两次：一次是测试脚本起的实例没退，一次是构建**过程中**
    # 又有人把 exe 跑起来了（所以光在开头清一次不够，失败后还得再清再重试）。
    function Stop-ProductInstance {
        $procs = @(Get-Process SyncReplayTool -ErrorAction SilentlyContinue)
        if ($procs.Count -eq 0) { return $false }
        foreach ($p in $procs) {
            Write-Host ("  停掉占用 exe 的产品进程 pid=" + $p.Id) -ForegroundColor Yellow
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Milliseconds 800
        return $true
    }
    Stop-ProductInstance | Out-Null

    & $Jom -f Makefile.Release
    if ($LASTEXITCODE -ne 0) {
        # 多半是 LNK1104：链接那一刻又有人把 exe 跑起来了。清干净再重试一轮。
        if (Stop-ProductInstance) {
            Write-Host '  构建失败后清掉占用进程，重试一轮…' -ForegroundColor Yellow
            & $Jom -f Makefile.Release
        }
        if ($LASTEXITCODE -ne 0) {
            # 第二种 LNK1104：**obj 还没编译完，链接就去打开它**（jom 的并行调度竞态）。
            #   实测（2026-11-05 14:41）：main.obj 的写入时间比链接失败还晚 30 秒 ——
            #   也就是 jom 依据**旧的依赖信息**决定"可以直接链接"，
            #   而 main.cpp 恰好刚被改过、main.obj 正在重编。
            #   症状是 LNK1104「无法打开文件 release\main.obj」，
            #   而**不是**「exe 被占用」—— 所以上面那轮"清占用进程"的重试救不了它。
            #   怎么办：再跑一遍（这一遍依赖信息已经是新的，obj 也齐了）。
            #   只重试一次：连续两次都失败就是真问题，不该被重试掩盖。
            Write-Host '  再试一轮（多半是 obj 还没编完就去链接）…' -ForegroundColor Yellow
            Start-Sleep -Milliseconds 500
            & $Jom -f Makefile.Release
        }
        if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    }

    $targetExe = "$BuildRoot\release\SyncReplayTool.exe"
    if (-not (Test-Path $targetExe)) { throw "artifact missing: $targetExe" }
    Write-Output "OK: $targetExe"

    # 同步更新 QtCreator 默认影子构建目录与 deploy 目录。
    # 这两个目录里还带着运行时 DLL，只有 exe 是新编的那一份 ——
    # 客户在 QtCreator 里点「运行」看到的就是这个 exe。
    $qtCreatorDir = "$BuildRoot\build-Release\release"
    if (Test-Path $qtCreatorDir) {
        Copy-Item $targetExe (Join-Path $qtCreatorDir 'SyncReplayTool.exe') -Force
    }
    $deployDir = "$BuildRoot\deploy"
    if (Test-Path $deployDir) {
        Copy-Item $targetExe (Join-Path $deployDir 'SyncReplayTool.exe') -Force
    }

    # ---- Qt 插件目录：让**每一个**构建输出目录都能直接运行 ----
    #
    # 2026-10-28 用户现场弹窗实测：
    #   「This application failed to start because it could not find or load
    #     the Qt platform plugin "windows" in ""」
    # 影子构建目录里只有 exe，没有 platforms\qwindows.dll ——
    # 插件只在 QtCreator 的活动套件目录和 deploy 目录里。
    # 从影子构建目录直接双击 exe 必然弹这个框，
    # 而且它是**模态**错误框：命令行调用会直接挂死（实测进程挂着不动）。
    #
    # 所以：把插件目录一起同步到构建输出目录，插件源优先取
    # QtCreator 活动套件目录（那里是 windeployqt 铺好的），取不到就退到
    # Qt 安装目录下的 plugins。
    $pluginDirs = @('platforms', 'imageformats', 'iconengines')
    $pluginSrc = $null
    foreach ($cand in @("$BuildRoot\build-Release\release", $deployDir)) {
        if (Test-Path (Join-Path $cand 'platforms\qwindows.dll')) { $pluginSrc = $cand; break }
    }
    if (-not $pluginSrc) {
        $qtPlugins = Join-Path $QtRoot 'plugins'
        if (Test-Path (Join-Path $qtPlugins 'platforms\qwindows.dll')) { $pluginSrc = $qtPlugins }
    }
    if ($pluginSrc) {
        $outDir = Join-Path $BuildRoot 'release'
        foreach ($pd in $pluginDirs) {
            $s = Join-Path $pluginSrc $pd
            if (Test-Path $s) {
                Copy-Item $s (Join-Path $outDir $pd) -Recurse -Force
            }
        }
        Write-Output "OK: plugins synced from $pluginSrc"
    } else {
        Write-Warning '没找到 qwindows.dll —— 这个构建目录不能直接运行（缺 Qt 平台插件）。'
    }
}
finally {
    Pop-Location
}
