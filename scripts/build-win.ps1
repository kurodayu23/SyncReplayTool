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
    [string]$BuildRoot   = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool',
    [string]$QtRoot      = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64',
    [string]$Jom         = 'C:\Qt\Tools\QtCreator\bin\jom\jom.exe',
    [string]$VcVars      = 'C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat'
)

$ErrorActionPreference = 'Stop'

if (-not $ProjectRoot) {
    $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$Project = Join-Path $ProjectRoot 'SyncReplayTool.pro'

$SdkHeaders = '10.0.10240.0'
$SdkTools   = '10.0.19041.0'

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
