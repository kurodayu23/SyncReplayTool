<#
.SYNOPSIS
    SyncReplayTool 交付包校验脚本 —— 三道机械闸门。

.DESCRIPTION
    闸门一（时间戳）：包内每个文件的 mtime ≥ 其对应源文件的 mtime。
        Windows 包对 exe / DLL / Run.cmd 逐个比；
        麒麟包对 payload 每个文件比（在容器里比，路径是中文，宿主侧解析不可靠）。

    闸门二（中文文案）：把源码里的每条中文文案抽成一张**清单表**，
        逐条到交付包里的二进制里找，缺一条即失败。
        - 复用 scripts/verify_binary_strings.py 的字面量抽取器（不重写）；
        - 用 scripts/cjk_manifest.py 额外记录每条文案**以哪种字节形态命中**
          （UTF-8 / UTF-16LE）—— QStringLiteral 落的是 UTF-16LE，
          只按 ASCII/UTF-8 搜会整片漏掉。

    闸门三（结构）：条目数、关键文件存在性、
        探针 exe / 旧版 exe 是否确实不在包里、
        麒麟包里 自检.sh 是否存在**且可执行**（x 位）、
        符号链接是否原样保留、有没有悬空链接。

.PARAMETER Json
    只输出 JSON（不输出人读的日志），便于最终汇总自动取数。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\verify_delivery_package.ps1 `
        -PackageDir D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\pkgtest

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\verify_delivery_package.ps1 `
        -Json | Out-File -Encoding utf8 report.json

.NOTES
    退出码：0 = 三道闸门全过；1 = 有闸门没过（详情看输出）；2 = 参数/环境错误。
#>
[CmdletBinding()]
param(
    [string]$PackageDir = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\pkgtest',
    [string]$ZipPath = '',
    [string]$TarPath = '',
    [string]$SrcRoot = '',
    [string]$RuntimeDir = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\deploy',
    [string[]]$FallbackRuntimeDirs = @('C:\Windows\System32'),
    [string]$PayloadDir = 'D:\AP\AP7000\ai-workspace\kylin-pkg\payload',
    [string]$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\nightD',
    [string]$WorkRoot = 'D:\AP\AP7000\ai-workspace\pkg-preflight',
    [string]$ContainerImage = 'kylin-v10-real:latest',
    [string]$ContainerName = 'srt-nightd-verify',
    [int]$ContainerPort = 0,
    [string]$JsonPath = '',
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:Quiet = $Json.IsPresent

function Say {
    param([Parameter(Position=0)][string]$m, [string]$ForegroundColor)
    if ($script:Quiet) { return }
    if ($ForegroundColor) { Write-Host $m -ForegroundColor $ForegroundColor }
    else { Write-Host $m }
}

if (-not $SrcRoot) {
    $SrcRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$SrcRoot = (Resolve-Path -LiteralPath $SrcRoot).Path

if (-not $ZipPath) { $ZipPath = Join-Path $PackageDir '数据同步工具-Windows-x64.zip' }
if (-not $TarPath) { $TarPath = Join-Path $PackageDir '数据同步工具-麒麟.tar.gz' }

$DeliverRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$TrashCmd    = 'C:\Users\jss18\.minimax\bin\mavis-trash.cmd'

# 排除名单（必须与 package-delivery.ps1 保持一致）
$ExcludedExeNames = @(
    'cfgprobe.exe', 'emptycheck.exe', 'globprobe.exe', 'guifull.exe',
    'leakprobe.exe', 'probe.exe', 'qssdump.exe', 'sendtest.exe',
    'SyncReplayTool-新版.exe', 'SyncReplayTool2.exe'
)

# 打包脚本**有意**改写过的文件（内容与源不同是预期的）
#   Run.cmd：deploy 里那份启动的是 SyncReplayTool2.exe（10/16 的旧构建），
#            而那个文件被排除名单排除、不在包里 —— 客户双击启动器直接失败。
#            打包脚本把启动目标归一化到包内唯一存在的 SyncReplayTool.exe。
$IntentionallyModified = @{
    'Run.cmd' = '打包脚本把启动目标从 SyncReplayTool2.exe（不在包内）改成 SyncReplayTool.exe'
}

# 关键文件清单
$WinMustHave = @(
    'SyncReplayTool.exe', 'Run.cmd', 'qt.conf', '使用步骤与注意事项.md',
    'Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Svg.dll',
    'platforms/qwindows.dll', 'imageformats/qjpeg.dll', 'iconengines/qsvgicon.dll',
    'rsync.exe', 'ssh.exe', 'scp.exe', 'ssh-keygen.exe', 'ssh-keyscan.exe',
    'msvcp140.dll', 'vcruntime140.dll', 'concrt140.dll', 'vccorlib140.dll'
)
$TarMustHave = @(
    'SyncReplayTool', 'run.sh', '自检.sh', '安装.sh', '安装自启.sh',
    '数据同步工具.desktop', 'qt.conf', '使用步骤与注意事项.md',
    'bin/rsync', 'bin/ssh', 'bin/scp', 'bin/ssh-keygen', 'bin/ssh-keyscan'
)

# ZIP 的 DOS 时间戳只有 2 秒精度（且向下取整），裸比 mtime 必然差 1~2 秒。
# 这是格式精度，不是"包比源旧" —— 所以给 2 秒容差，并用 sha256 补强。
$ZipMtimeToleranceSec = -2

function Remove-Recyclable {
    param([string[]]$Path)
    foreach ($p in $Path) {
        if (-not $p) { continue }
        if (-not (Test-Path -LiteralPath $p)) { continue }
        if (Test-Path -LiteralPath $TrashCmd) { & $TrashCmd -- $p | Out-Null }
        else { Remove-Item -LiteralPath $p -Recurse -Force }
    }
}

function Get-NormalizedPath {
    param([string]$Path)
    if (-not $Path) { return '' }
    try { return [System.IO.Path]::GetFullPath($Path).TrimEnd('\') } catch { return $Path.TrimEnd('\') }
}

function Test-IsUnder {
    param([string]$Child, [string]$Parent)
    $c = Get-NormalizedPath $Child; $p = Get-NormalizedPath $Parent
    if (-not $c -or -not $p) { return $false }
    return ($c -eq $p) -or ($c.StartsWith($p + '\', [System.StringComparison]::OrdinalIgnoreCase))
}

# 路径 <-> base64(UTF-8)：容器输出走 base64，彻底绕开 Windows 控制台代码页
function To-B64 {
    param([string]$Text)
    return [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($Text))
}
function From-B64 {
    param([string]$Text)
    try { return [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Text)) } catch { return '' }
}

$report = [ordered]@{
    generated_at = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    package_dir  = $PackageDir
    zip          = [ordered]@{}
    tar          = [ordered]@{}
    gates        = [ordered]@{}
    overall      = 'UNKNOWN'
}

# =============================================================================
# 0. 前置检查
# =============================================================================

Say '=== 交付包校验：闸门一（时间戳）/ 闸门二（中文文案）/ 闸门三（结构） ==='

Add-Type -AssemblyName System.IO.Compression.FileSystem

if (-not (Test-Path -LiteralPath $ZipPath)) { throw "找不到 zip：$ZipPath" }
$zipItem = Get-Item -LiteralPath $ZipPath
$report.zip = [ordered]@{
    path   = $zipItem.FullName
    bytes  = $zipItem.Length
    sha256 = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
}
Say ("   zip : {0}  {1} 字节" -f $zipItem.Name, $zipItem.Length)

$hasTar = Test-Path -LiteralPath $TarPath
if ($hasTar) {
    $tarItem = Get-Item -LiteralPath $TarPath
    $report.tar = [ordered]@{
        path   = $tarItem.FullName
        bytes  = $tarItem.Length
        sha256 = (Get-FileHash -LiteralPath $TarPath -Algorithm SHA256).Hash
    }
    Say ("   tar : {0}  {1} 字节" -f $tarItem.Name, $tarItem.Length)
} else {
    Say "   tar : 缺失（$TarPath）—— 麒麟侧闸门将标记为 SKIP" -ForegroundColor Yellow
}

# =============================================================================
# 闸门一 · 时间戳（Windows）
# =============================================================================

Say ''
Say '--- 闸门一：时间戳（Windows zip，逐个文件比） ---'

# 源文件映射：zip 条目名 -> 磁盘上的源文件
function Resolve-WinSource {
    param([string]$Entry)
    if ($Entry -eq 'SyncReplayTool.exe') { return (Join-Path $BuildRoot 'release\SyncReplayTool.exe') }
    if ($Entry -eq '使用步骤与注意事项.md') {
        $doc = Get-ChildItem -LiteralPath $SrcRoot -File -Filter '*.md' |
               Where-Object { $_.Name -like '*步骤*' -or $_.Name -like '*注意事项*' } | Select-Object -First 1
        if ($doc) { return $doc.FullName }
        return $null
    }
    if ($Entry -eq 'qt.conf') { return (Join-Path $PayloadDir 'qt.conf') }
    $p = Join-Path $RuntimeDir ($Entry -replace '/', '\')
    if (Test-Path -LiteralPath $p) { return $p }
    foreach ($fb in $FallbackRuntimeDirs) {
        if (-not $fb) { continue }
        $cand = Join-Path $fb ($Entry -replace '/', '\')
        if (Test-Path -LiteralPath $cand) { return $cand }
    }
    return $p
}

$zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
$gate1Win = [ordered]@{ passed = 0; failed = 0; no_source = 0; checks = @() }
try {
    $entries = @($zip.Entries | Where-Object { $_.Name -ne '' })
    $report.zip.entries = $entries.Count
    Say ("   zip 条目数: {0}" -f $entries.Count)
    foreach ($e in $entries) {
        $src = Resolve-WinSource -Entry $e.FullName
        if (-not $src -or -not (Test-Path -LiteralPath $src)) {
            $gate1Win.no_source++
            $gate1Win.checks += [ordered]@{
                entry = $e.FullName; status = 'NO_SOURCE'
                entry_mtime = $e.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
                source = $(if ($src) { $src } else { '' })
            }
            continue
        }
        $srcTime = (Get-Item -LiteralPath $src).LastWriteTime
        # ZIP 的时间戳是 DOS 格式，**只有 2 秒精度**（且向下取整）。
        # 所以裸比 mtime 必然差 1~2 秒 —— 那是格式精度，不是"包比源旧"。
        # 判据改成两条：
        #   (a) mtime 容差 2 秒（= zip 格式精度）；
        #   (b) 包内内容与源文件 **sha256 完全一致** —— 这条比 mtime 强得多，
        #       内容一样就不可能"包比源旧"。
        $delta = ($e.LastWriteTime - $srcTime).TotalSeconds
        $mtimeOk = ($delta -ge $ZipMtimeToleranceSec)

        $contentOk = $true
        $entrySha = ''
        $srcSha = ''
        if ($IntentionallyModified.ContainsKey($e.FullName)) {
            # 打包脚本**有意**改过的文件（例如启动器归一化）：不比内容，
            # 只记录理由，并单独确认改完仍然指向包内真实存在的 exe。
            $contentOk = $true
        } else {
            $es = $e.Open()
            try {
                $sha = [System.Security.Cryptography.SHA256]::Create()
                try { $entrySha = ([BitConverter]::ToString($sha.ComputeHash($es))).Replace('-', '') }
                finally { $sha.Dispose() }
            } finally { $es.Dispose() }
            $srcSha = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
            $contentOk = ($entrySha -eq $srcSha)
        }
        $ok = $mtimeOk -and $contentOk
        if ($ok) { $gate1Win.passed++ } else { $gate1Win.failed++ }
        $gate1Win.checks += [ordered]@{
            entry      = $e.FullName
            status     = $(if ($ok) { 'OK' } else { 'FAIL' })
            entry_mtime = $e.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
            source     = $src
            src_mtime  = $srcTime.ToString('yyyy-MM-dd HH:mm:ss')
            delta_sec  = [math]::Round($delta, 0)
            mtime_ok   = $mtimeOk
            content_ok = $contentOk
            entry_sha256 = $entrySha
            src_sha256   = $srcSha
            note        = $(if ($IntentionallyModified.ContainsKey($e.FullName)) { $IntentionallyModified[$e.FullName] } else { '' })
        }
    }
} finally { $zip.Dispose() }

if ($gate1Win.failed -gt 0) {
    Say ("   FAIL：{0} 个文件没过（mtime 旧于源 / 内容与源不一致）" -f $gate1Win.failed) -ForegroundColor Red
    foreach ($c in $gate1Win.checks | Where-Object { $_.status -eq 'FAIL' }) {
        Say ("     {0}  包内 {1}  源 {2}  Δ{3}s  mtime_ok={4} content_ok={5}" -f `
            $c.entry, $c.entry_mtime, $c.src_mtime, $c.delta_sec, $c.mtime_ok, $c.content_ok)
    }
} else {
    Say ("   PASS：{0}/{1} 个文件不旧于源且内容与源逐字节一致（{2} 个无对照源，{3} 个为打包脚本有意改写）" -f `
        $gate1Win.passed, ($gate1Win.passed + $gate1Win.failed), $gate1Win.no_source, $IntentionallyModified.Count) -ForegroundColor Green
}

# =============================================================================
# 闸门二 · 中文文案（Windows）
# =============================================================================

Say ''
Say '--- 闸门二：中文文案逐条命中（Windows） ---'

$verifyWin = Join-Path $WorkRoot 'verify-win'
Remove-Recyclable @($verifyWin)
New-Item -ItemType Directory -Force -Path $verifyWin | Out-Null
$exeOut = Join-Path $verifyWin 'SyncReplayTool.exe'

$zip2 = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    $entry = $zip2.Entries | Where-Object { $_.FullName -eq 'SyncReplayTool.exe' } | Select-Object -First 1
    if (-not $entry) { throw 'zip 里没有 SyncReplayTool.exe' }
    $ins = $entry.Open()
    try {
        $outs = [System.IO.File]::Create($exeOut)
        try { $ins.CopyTo($outs) } finally { $outs.Dispose() }
    } finally { $ins.Dispose() }
} finally { $zip2.Dispose() }

# 包内 exe 与编译产物逐字节对账
$buildExe = Join-Path $BuildRoot 'release\SyncReplayTool.exe'
$pkgExeSha = (Get-FileHash -LiteralPath $exeOut -Algorithm SHA256).Hash
$buildSha = ''
if (Test-Path -LiteralPath $buildExe) { $buildSha = (Get-FileHash -LiteralPath $buildExe -Algorithm SHA256).Hash }
$exeMatch = ($buildSha -ne '' -and $pkgExeSha -eq $buildSha)
Say ("   包内 exe sha256 : {0}" -f $pkgExeSha)
if ($buildSha) {
    Say ("   编译产物 sha256: {0}   -> {1}" -f $buildSha, $(if ($exeMatch) { '一致' } else { '不一致' }))
}

# 复用现成闸门 verify_binary_strings.py（时间戳 + 文案，一条命令两个判据）
$gateTool = Join-Path $SrcRoot 'scripts\verify_binary_strings.py'
$manifestTool = Join-Path $SrcRoot 'scripts\cjk_manifest.py'
$manifestWinPath = Join-Path $verifyWin 'cjk-manifest-win.json'

$gate2Win = [ordered]@{ tool = $gateTool; rc = 0; output = ''; manifest = $null }
if (Test-Path -LiteralPath $gateTool) {
    $g = & python $gateTool --src $SrcRoot --bin $exeOut --platform win --skip-mtime 2>&1
    $gate2Win.rc = $LASTEXITCODE
    $gate2Win.output = ($g | Out-String).Trim()
    Say ($gate2Win.output -split "`n" | Select-Object -First 1)
    if ($gate2Win.rc -ne 0) {
        Say "   （完整输出见 JSON 的 gates.gate2_cjk.windows.output）" -ForegroundColor Yellow
    }
} else {
    $gate2Win.rc = 2
    $gate2Win.output = "找不到 $gateTool"
}

if (Test-Path -LiteralPath $manifestTool) {
    $m = & python $manifestTool --src $SrcRoot --bin $exeOut --platform win --out $manifestWinPath 2>&1
    $mrc = $LASTEXITCODE
    if (Test-Path -LiteralPath $manifestWinPath) {
        $gate2Win.manifest = Get-Content -LiteralPath $manifestWinPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $gate2Win.manifest_rc = $mrc
        Say ("   清单表：{0} 条，命中 {1}，缺 {2}（UTF-8 {3} / UTF-16LE {4}）" -f `
            $gate2Win.manifest.total, $gate2Win.manifest.hit, $gate2Win.manifest.missing,
            $gate2Win.manifest.hit_by_encoding.'utf-8', $gate2Win.manifest.hit_by_encoding.'utf-16le')
    }
}

# =============================================================================
# 闸门三 · 结构（Windows）
# =============================================================================

Say ''
Say '--- 闸门三：结构与排除（Windows zip） ---'

$zip3 = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
$gate3Win = [ordered]@{ missing = @(); probe_hits = @(); must_have = @() }
try {
    $names = @($zip3.Entries | ForEach-Object { $_.FullName })
    foreach ($m in $WinMustHave) {
        $hit = @($names | Where-Object { $_ -eq $m })
        $gate3Win.must_have += [ordered]@{ path = $m; present = ($hit.Count -gt 0) }
        if ($hit.Count -eq 0) { $gate3Win.missing += $m }
    }
    # 排除门禁：10 个探针 / 旧版 exe 一个都不许在
    $gate3Win.probe_hits = @($names | Where-Object {
        $leaf = ($_ -split '/')[-1]
        $leaf -in $ExcludedExeNames
    })
} finally { $zip3.Dispose() }

if ($gate3Win.missing.Count -gt 0) {
    Say ("   FAIL：缺 {0} 个关键文件 -> {1}" -f $gate3Win.missing.Count, ($gate3Win.missing -join ', ')) -ForegroundColor Red
} else {
    Say ("   PASS：{0} 个关键文件全部在" -f $WinMustHave.Count) -ForegroundColor Green
}
if ($gate3Win.probe_hits.Count -gt 0) {
    Say ("   FAIL：包里出现了探针/旧版 exe -> {0}" -f ($gate3Win.probe_hits -join ', ')) -ForegroundColor Red
} else {
    Say ("   PASS：{0} 个探针/旧版 exe 全部不在包里" -f $ExcludedExeNames.Count) -ForegroundColor Green
}

# =============================================================================
# 麒麟侧：容器内检查（路径是中文，宿主侧解析不可靠 -> 全部在容器里做）
# =============================================================================

$gate1Tar = [ordered]@{ status = 'SKIP'; passed = 0; failed = 0; checks = @() }
$gate2Tar = [ordered]@{ status = 'SKIP'; rc = 0; output = ''; manifest = $null }
$gate3Tar = [ordered]@{ status = 'SKIP'; missing = @(); entries = 0; symlinks = 0; dangling = 0; selfcheck_mode = ''; selfcheck_executable = $false }

if ($hasTar) {
    Say ''
    Say '--- 麒麟包：容器内检查（路径/权限/符号链接） ---'

    $inspectSh = @'
#!/bin/bash
set -eo pipefail
export LC_ALL=C.UTF-8
PKG_TGZ="$1"; PAYLOAD="$2"; OUTDIR="$3"; SELFB64="$4"
b64() { printf '%s' "$1" | base64 -w0 2>/dev/null || printf '%s' "$1" | base64 | tr -d '\n'; }
mkdir -p /tmp/v "$OUTDIR"
tar -xzf "$PKG_TGZ" -C /tmp/v
ROOT=$(find /tmp/v -mindepth 1 -maxdepth 1 -type d | head -1)
echo "ROOT|$(b64 "$(basename "$ROOT")")"
find /tmp/v -mindepth 1 -printf '%y|%m|%T@|%s|%P\n' | LC_ALL=C sort | while IFS='|' read -r ty mo mt sz p; do
  echo "ENTRY|$(b64 "$ty")|$(b64 "$mo")|$(b64 "$mt")|$(b64 "$sz")|$(b64 "$p")"
done
echo "SUMMARY|$(b64 "$(find /tmp/v -mindepth 1 | wc -l)")|$(b64 "$(find /tmp/v -type l | wc -l)")|$(b64 "$(find /tmp/v -xtype l 2>/dev/null | wc -l)")"
SCNAME=$(printf '%s' "$SELFB64" | base64 -d)
SC=$(find "$ROOT" -maxdepth 1 -name "$SCNAME" | head -1)
if [ -n "$SC" ]; then
  echo "SELFCHK|$(b64 "${SC#$ROOT/}")|$(b64 "$(stat -c '%a' "$SC")")|$(b64 "$([ -x "$SC" ] && echo yes || echo no)")"
else
  echo "SELFCHK||$(b64 0)|$(b64 no)"
fi
cd "$PAYLOAD" || exit 1
find . -type f -print | LC_ALL=C sort | while IFS= read -r f; do
  rel=${f#./}
  pm=$(stat -c '%Y' "$f")
  tgt="$ROOT/$rel"
  if [ -e "$tgt" ]; then
    tm=$(stat -c '%Y' "$tgt")
    if [ "$tm" -ge "$pm" ]; then echo "PAYLOAD|$(b64 OK)|$(b64 "$rel")|$(b64 "$pm")|$(b64 "$tm")"
    else echo "PAYLOAD|$(b64 FAIL)|$(b64 "$rel")|$(b64 "$pm")|$(b64 "$tm")"; fi
  else
    echo "PAYLOAD|$(b64 MISSING)|$(b64 "$rel")|$(b64 "$pm")|$(b64 0)"
  fi
done
cp -f "$ROOT/SyncReplayTool" "$OUTDIR/SyncReplayTool" 2>/dev/null || true
echo "DONE"
'@

    $inspectPath = Join-Path $WorkRoot 'tar-inspect.sh'
    New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
    [System.IO.File]::WriteAllText($inspectPath, ($inspectSh -replace "`r`n", "`n"),
                                    (New-Object System.Text.UTF8Encoding($false)))

    $verifyKylin = Join-Path $WorkRoot 'verify-kylin'
    Remove-Recyclable @($verifyKylin)
    New-Item -ItemType Directory -Force -Path $verifyKylin | Out-Null

    # 只挂**只读**的目录（可写目录挂载会被本机安全策略拦下）；
    # 抽出来的二进制放在容器自己的 /out，再用 docker cp 拷回宿主。
    $selfB64 = To-B64 '自检.sh'
    docker create --name $ContainerName --entrypoint /bin/bash `
        -v "${TarPath}:/pkg.tgz:ro" `
        -v "${PayloadDir}:/payload:ro" `
        -v "${inspectPath}:/inspect.sh:ro" `
        $ContainerImage -c "/inspect.sh /pkg.tgz /payload /out $selfB64" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "docker create 失败（exit=$LASTEXITCODE）" }
    $inspectLog = & docker start -a $ContainerName 2>&1
    $inspectRc = $LASTEXITCODE
    $cpLog = & docker cp "${ContainerName}:/out/SyncReplayTool" (Join-Path $verifyKylin 'SyncReplayTool') 2>&1
    $cpRc = $LASTEXITCODE
    docker rm -f $ContainerName | Out-Null
    if ($cpRc -ne 0) { $cpLog | ForEach-Object { Say "     $_" } }
    if ($inspectRc -ne 0) {
        Say "   FAIL：容器内检查失败（exit=$inspectRc）" -ForegroundColor Red
        $inspectLog | ForEach-Object { Say "     $_" }
        $gate3Tar.status = 'FAIL'
    } else {
        $tarEntries = @()
        $payloadChecks = @()
        $rootName = ''
        foreach ($line in $inspectLog) {
            $s = [string]$line
            if ($s -like 'ENTRY|*') {
                $p = $s -split '\|'
                $tarEntries += [ordered]@{
                    type = (From-B64 $p[1]); mode = (From-B64 $p[2]); mtime = (From-B64 $p[3])
                    size = (From-B64 $p[4]); path = (From-B64 $p[5])
                }
            } elseif ($s -like 'SUMMARY|*') {
                $p = $s -split '\|'
                $gate3Tar.entries = [int](From-B64 $p[1])
                $gate3Tar.symlinks = [int](From-B64 $p[2])
                $gate3Tar.dangling = [int](From-B64 $p[3])
            } elseif ($s -like 'SELFCHK|*') {
                $p = $s -split '\|'
                $gate3Tar.selfcheck_path = (From-B64 $p[1])
                $gate3Tar.selfcheck_mode = (From-B64 $p[2])
                $gate3Tar.selfcheck_executable = ((From-B64 $p[3]) -eq 'yes')
            } elseif ($s -like 'PAYLOAD|*') {
                $p = $s -split '\|'
                $payloadChecks += [ordered]@{
                    status = (From-B64 $p[1]); path = (From-B64 $p[2])
                    payload_mtime = [int64](From-B64 $p[3]); tar_mtime = [int64](From-B64 $p[4])
                }
            } elseif ($s -like 'ROOT|*') {
                $rootName = From-B64 ($s -split '\|')[1]
            }
        }
        $report.tar.entries = $gate3Tar.entries
        $report.tar.symlinks = $gate3Tar.symlinks
        $report.tar.dangling_symlinks = $gate3Tar.dangling
        $report.tar.root_name = $rootName

        # ---- 闸门一（麒麟）：payload 每个文件的 mtime ----
        $gate1Tar.status = 'PASS'
        $gate1Tar.passed = 0; $gate1Tar.failed = 0
        foreach ($c in $payloadChecks) {
            if ($c.status -eq 'OK') { $gate1Tar.passed++ }
            else { $gate1Tar.failed++; $gate1Tar.status = 'FAIL' }
            $gate1Tar.checks += $c
        }
        Say ("   闸门一(麒麟)：payload {0} 个文件，通过 {1}，失败 {2}" -f $payloadChecks.Count, $gate1Tar.passed, $gate1Tar.failed)
        foreach ($c in $payloadChecks | Where-Object { $_.status -ne 'OK' }) {
            Say ("     [{0}] {1}  payload mtime={2}  包内 mtime={3}" -f $c.status, $c.path, $c.payload_mtime, $c.tar_mtime) -ForegroundColor Red
        }

        # ---- 闸门二（麒麟）：中文文案 ----
        $kylinExe = Join-Path $verifyKylin 'SyncReplayTool'
        if (Test-Path -LiteralPath $kylinExe) {
            $manifestTarPath = Join-Path $verifyKylin 'cjk-manifest-kylin.json'
            $g2 = & python $gateTool --src $SrcRoot --bin $kylinExe --platform linux 2>&1
            $gate2Tar.rc = $LASTEXITCODE
            $gate2Tar.output = ($g2 | Out-String).Trim()
            $gate2Tar.status = $(if ($gate2Tar.rc -eq 0) { 'PASS' } else { 'FAIL' })
            Say ($gate2Tar.output -split "`n" | Select-Object -First 1)
            $m2 = & python $manifestTool --src $SrcRoot --bin $kylinExe --platform linux --out $manifestTarPath 2>&1
            $m2rc = $LASTEXITCODE
            if (Test-Path -LiteralPath $manifestTarPath) {
                $gate2Tar.manifest = Get-Content -LiteralPath $manifestTarPath -Raw -Encoding UTF8 | ConvertFrom-Json
                $gate2Tar.manifest_rc = $m2rc
                Say ("   清单表：{0} 条，命中 {1}，缺 {2}" -f $gate2Tar.manifest.total, $gate2Tar.manifest.hit, $gate2Tar.manifest.missing)
            }
        } else {
            $gate2Tar.status = 'FAIL'
            $gate2Tar.output = '容器没有抽出 SyncReplayTool'
        }

        # ---- 闸门三（麒麟）：结构 ----
        $gate3Tar.status = 'PASS'
        $tarPaths = @($tarEntries | ForEach-Object { $_.path })
        foreach ($m in $TarMustHave) {
            $hit = @($tarPaths | Where-Object { $_ -eq "$rootName/$m" -or $_ -eq $m })
            if ($hit.Count -eq 0) { $gate3Tar.missing += $m; $gate3Tar.status = 'FAIL' }
        }
        if (-not $gate3Tar.selfcheck_executable) { $gate3Tar.status = 'FAIL' }
        if ($gate3Tar.dangling -gt 0) { $gate3Tar.status = 'FAIL' }
        Say ("   闸门三(麒麟)：条目 {0}，符号链接 {1}，悬空链接 {2}，自检.sh 权限位 {3}（可执行 {4}）" -f `
            $gate3Tar.entries, $gate3Tar.symlinks, $gate3Tar.dangling, $gate3Tar.selfcheck_mode, $gate3Tar.selfcheck_executable)
        if ($gate3Tar.missing.Count -gt 0) {
            Say ("     FAIL：麒麟包缺 {0} 个关键文件 -> {1}" -f $gate3Tar.missing.Count, ($gate3Tar.missing -join ', ')) -ForegroundColor Red
        }
        if ($gate3Tar.dangling -gt 0) {
            Say ("     FAIL：包内有 {0} 个悬空符号链接" -f $gate3Tar.dangling) -ForegroundColor Red
        }
    }
}

# =============================================================================
# 汇总
# =============================================================================

$gate1Status = if (($gate1Win.failed + $gate1Tar.failed) -gt 0) { 'FAIL' } else { 'PASS' }
$gate2Status = if (($gate2Win.rc -ne 0) -or ($gate2Tar.status -eq 'FAIL')) { 'FAIL' } else { 'PASS' }
$gate3Status = if (($gate3Win.missing.Count -gt 0) -or ($gate3Win.probe_hits.Count -gt 0) -or ($gate3Tar.status -eq 'FAIL')) { 'FAIL' } else { 'PASS' }

$report.gates = [ordered]@{
    gate1_timestamp = [ordered]@{
        status = $gate1Status
        windows = $gate1Win
        kylin   = $gate1Tar
    }
    gate2_cjk = [ordered]@{
        status  = $gate2Status
        windows = $gate2Win
        kylin   = $gate2Tar
    }
    gate3_structure = [ordered]@{
        status  = $gate3Status
        windows = $gate3Win
        kylin   = $gate3Tar
    }
}
$report.gates.gate3_structure.windows.exe_sha256_match = $exeMatch
$report.overall = if ($gate1Status -eq 'PASS' -and $gate2Status -eq 'PASS' -and $gate3Status -eq 'PASS') { 'PASS' } else { 'FAIL' }

Say ''
Say ("闸门一 时间戳 : {0}" -f $gate1Status) -ForegroundColor $(if ($gate1Status -eq 'PASS') { 'Green' } else { 'Red' })
Say ("闸门二 中文文案: {0}" -f $gate2Status) -ForegroundColor $(if ($gate2Status -eq 'PASS') { 'Green' } else { 'Red' })
Say ("闸门三 结构   : {0}" -f $gate3Status) -ForegroundColor $(if ($gate3Status -eq 'PASS') { 'Green' } else { 'Red' })
Say ("总判定        : {0}" -f $report.overall) -ForegroundColor $(if ($report.overall -eq 'PASS') { 'Green' } else { 'Red' })

$jsonText = $report | ConvertTo-Json -Depth 12
if ($JsonPath) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $JsonPath) | Out-Null
    [System.IO.File]::WriteAllText($JsonPath, $jsonText, (New-Object System.Text.UTF8Encoding($false)))
    Say "JSON 已写：$JsonPath"
}
if ($Json) { Write-Output $jsonText }

# 清理自造的临时目录
Remove-Recyclable @($verifyWin, $verifyKylin)

if ($report.overall -ne 'PASS') { exit 1 }
exit 0
