# build-probe.ps1 - build the headless probe (scripts\tests\headless-probe\probe.pro).
# Mirrors scripts\build-win.ps1 (VS2015 x64 + pinned Windows SDK).
# The probe links the product sources, so it must be rebuilt after every
# source change - a stale probe silently tests the previous build.
# ASCII only.
param(
  [string]$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\work\probe\build',
  [string]$QtRoot    = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64',
  [string]$Jom       = 'C:\Qt\Tools\QtCreator\bin\jom\jom.exe',
  [string]$VcVars    = 'C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat',
  [string]$Pro       = (Join-Path $PSScriptRoot 'probe.pro')
)
$ErrorActionPreference = 'Stop'
$SdkHeaders = '10.0.10240.0'
$SdkTools   = '10.0.19041.0'

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

$dump = Join-Path $env:TEMP 'srt_probe_vcenv.txt'
cmd /c "`"$VcVars`" x64 >nul 2>&1 && set" | Out-File -FilePath $dump -Encoding OEM
foreach ($line in (Get-Content $dump)) {
  $i = $line.IndexOf('=')
  if ($i -lt 1) { continue }
  $name = $line.Substring(0, $i)
  $val  = $line.Substring($i + 1)
  if ($val -eq '') { Set-Item "Env:$name" '' } else { Set-Item "Env:$name" $val }
}
Remove-Item $dump -Force -ErrorAction SilentlyContinue

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

Push-Location $BuildRoot
try {
  & "$QtRoot\bin\qmake.exe" $Pro CONFIG+=release
  if ($LASTEXITCODE -ne 0) { throw "qmake failed ($LASTEXITCODE)" }
  & $Jom -f Makefile.Release
  if ($LASTEXITCODE -ne 0) {
    Start-Sleep -Milliseconds 800
    & $Jom -f Makefile.Release
  }
  if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
  $exe = Join-Path $BuildRoot 'release\srtprobe.exe'
  if (-not (Test-Path $exe)) { throw "artifact missing: $exe" }
  Write-Output ("OK: {0}" -f $exe)
} finally {
  Pop-Location
}
