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

    & $Jom -f Makefile.Release
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

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
}
finally {
    Pop-Location
}
