<#
.SYNOPSIS
    SyncReplayTool 交付包打包脚本（Windows x64 zip + 麒麟 x86_64 tar.gz）。

.DESCRIPTION
    把"打最终交付包"从手工操作变成**可重复执行**的一行命令。

    为什么必须有这个脚本（上一版是手工打的，风险点）：
      * deploy\ 里混着 8 个内部测试探针 exe 和 2 个旧版 exe 副本。
        手工 `Compress-Archive deploy\*` 会把它们一起打进客户机。
      * 上一版 Run.cmd 启动的是 `SyncReplayTool2.exe`（10/16 的旧构建），
        而那个文件**并不在包里** —— 客户双击启动器直接失败。
      * 上一版麒麟包里**漏了** `安装自启.sh`，而 `自检.sh` / `安装.sh`
        都在提示用户执行 `sh 安装自启.sh` —— 照着做必然 No such file。
      * 上一版交付包比源码老了近一小时，没有任何一道门禁发现这件事。

    本脚本做四件事：
      1. **编译到独立 BuildRoot**（默认 nightD）。绝不碰 release\ /
         build-Release\release\ / uibuild\ / nightA..nightUI\ ——
         那些目录被占用会 LNK1104，用户会看到"Qt 打不开软件"。
      2. **Windows zip 走白名单**（不是黑名单）：只收清单里的东西，
         探针 exe / 旧版 exe 一律不收；清单写死在脚本里，可审计。
      3. **麒麟 tar.gz 在 Linux 容器里打**（Windows 上打会丢符号链接
         和权限位，自检.sh 的 x 位会没了）。
      4. **幂等**：每次跑都重建 staging 目录，旧产物不会混进新包。
         最后打印每个产物的字节数 + sha256。

    脚本本身不做"包内容对不对"的判定 —— 那是 verify_delivery_package.ps1
    的活。这里只负责"打出一个结构正确的包"。

.PARAMETER BuildRoot
    编译用的独立目录。**绝不能**指向 release\ / build-Release\release\ /
    uibuild\ / nightA / nightB / nightC / nightUI —— 脚本会硬拦。

.PARAMETER OutDir
    包输出目录。默认是预演目录 pkgtest\，**不是**正式包目录 交付包-新\。

.EXAMPLE
    # 预演：编译到 nightD，打包到 pkgtest\
    powershell -ExecutionPolicy Bypass -File scripts\package-delivery.ps1

.EXAMPLE
    # 只重打 Windows zip，不重新编译
    powershell -ExecutionPolicy Bypass -File scripts\package-delivery.ps1 `
        -SkipBuild -SkipKylinTar

.NOTES
    退出码：0 = 打包成功；1 = 打包失败（门禁拦下）。
#>
[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$BuildRoot   = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\nightD',
    [string]$OutDir      = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\pkgtest',
    [string]$RuntimeDir  = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\deploy',
    [string[]]$FallbackRuntimeDirs = @('C:\Windows\System32'),
    [string]$PayloadDir  = 'D:\AP\AP7000\ai-workspace\kylin-pkg\payload',
    [string]$KylinBaseTar = 'D:\AP\AP7000\ai-workspace\kylin-pkg\.stage\base.tar.gz',
    [string]$KylinBinary  = '',
    [string]$WorkRoot     = 'D:\AP\AP7000\ai-workspace\pkg-preflight',
    [string]$ContainerImage = 'kylin-v10-real:latest',
    [string]$ContainerName  = 'srt-nightd-pkg',
    [int]$ContainerPort     = 0,
    [switch]$SkipBuild,
    [switch]$SkipWinZip,
    [switch]$SkipKylinTar,
    [switch]$KeepStaging,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# .NET Core 下 ZipArchive / ZipArchiveMode 在 System.IO.Compression 里，
# 不一定被自动加载 —— 显式装一次，免得 New-DeterministicZip 报
# "Unable to find type [System.IO.Compression.ZipArchiveMode]"。
try { Add-Type -AssemblyName System.IO.Compression } catch { }
try { Add-Type -AssemblyName System.IO.Compression.FileSystem } catch { }

# -Json 模式：只输出 JSON，不输出人读的日志（便于最终汇总自动取数）
$script:Quiet = $Json.IsPresent
$script:ArtifactList = @()
$script:ZipEntries = 0
$script:StagedFiles = 0
$script:RunCmdNormalized = $false
$script:RunCmdTarget = ''
$script:KylinMarkers = [ordered]@{}

# ---- -Json 模式：把所有人读的日志静音，只输出 JSON ----
# 用同名函数**遮蔽** Write-Host cmdlet（PowerShell 里函数优先于 cmdlet），
# 免得满脚本都是 Say(...) 的噪音，也免得漏掉某一处。
function Write-Host {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, ValueFromPipeline = $true)]$Object,
        [switch]$NoNewline,
        $ForegroundColor,
        $BackgroundColor,
        $Separator
    )
    if ($script:Quiet) { return }
    $sp = @{}
    if ($ForegroundColor) { $sp['ForegroundColor'] = $ForegroundColor }
    if ($BackgroundColor) { $sp['BackgroundColor'] = $BackgroundColor }
    if ($Separator) { $sp['Separator'] = $Separator }
    Microsoft.PowerShell.Utility\Write-Host -Object $Object -NoNewline:$NoNewline @sp
}
function Say {
    param([Parameter(Position = 0)][string]$m, [string]$ForegroundColor)
    if ($ForegroundColor) { Write-Host $m -ForegroundColor $ForegroundColor }
    else { Write-Host $m }
}

if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path

$DeliverRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$TrashCmd    = 'C:\Users\jss18\.minimax\bin\mavis-trash.cmd'

# =============================================================================
# 0. 常量：白名单 / 排除名单 / 受保护目录
# =============================================================================

# ---- Windows zip 白名单：精确文件名 ----
$WinExactFiles = @(
    'SyncReplayTool.exe'   # 由本次编译产出，从 $BuildRoot\release 取，绝不从 deploy 取
    'Run.cmd'
    'qt.conf'
    '使用步骤与注意事项.md'
    # Qt 运行时
    'Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Svg.dll'
    # Qt 平台/图像/图标插件依赖
    'libEGL.dll', 'libGLESV2.dll'
    # MSVC 运行库
    'concrt140.dll', 'msvcp140.dll', 'vccorlib140.dll', 'vcruntime140.dll',
    'ucrtbase.dll', 'vcruntime140_1.dll'
    # 随包 GNU 工具
    'rsync.exe', 'ssh.exe', 'scp.exe', 'ssh-keygen.exe', 'ssh-keyscan.exe'
)

# ---- Windows zip 白名单：通配符 ----
$WinPatterns = @('msys-*.dll')

# ---- Windows zip 白名单：整目录收（里面的文件全是 Qt 插件）----
$WinDirs = @('platforms', 'imageformats', 'iconengines')

# =============================================================================
# 排除名单（写死在脚本里，可审计）
#   8 个内部测试探针 + 2 个旧版 exe 副本。
#   这 10 个文件**一律不得进包**。
# =============================================================================
$ExcludedExeNames = @(
    # ---- 内部测试探针（8 个）----
    'cfgprobe.exe',      # 配置读写探针
    'emptycheck.exe',    # 空目录边界探针
    'globprobe.exe',     # 通配符探针
    'guifull.exe',       # 完整界面探针
    'leakprobe.exe',     # 句柄泄漏探针
    'probe.exe',         # 通用探针
    'qssdump.exe',       # Qt 样式表转储探针
    'sendtest.exe',      # 发送端连通性探针
    # ---- 旧版 exe 副本（2 个）----
    'SyncReplayTool-新版.exe',   # 10/16 的旧构建
    'SyncReplayTool2.exe'       # 10/16 的旧构建（且被 Run.cmd 引用 —— 见下面的门禁）
)

# ---- 受保护目录：这些 BuildRoot 会和别人打架 / 让用户"Qt 打不开软件" ----
$ForbiddenBuildRoots = @(
    "$DeliverRoot\release",
    "$DeliverRoot\build-Release\release",
    "$DeliverRoot\uibuild",
    "$DeliverRoot\nightA",
    "$DeliverRoot\nightB",
    "$DeliverRoot\nightC",
    "$DeliverRoot\nightUI"
)

# ---- 受保护输出目录：正式包不能被预演覆盖 ----
$ForbiddenOutDirs = @("$DeliverRoot\交付包-新", "$DeliverRoot\交付包-正式版-20261025")

# ---- 容器红线：这些容器/端口不能碰 ----
$ProtectedContainerNames = @(
    'pkgacc11', 'srt-netfail', 'srtwin500', 'nightB-recv',
    'srt-p0-verify', 'srt-p0-recv', 'srt-nighta-recv'
)
$ProtectedPortRange = @(2430, 2435)   # nightB-recv / srt-nighta-recv 占用

# =============================================================================
# 工具函数
# =============================================================================

function Write-Step { param([string]$Msg) Say "== $Msg" -ForegroundColor Cyan }

function Remove-Recyclable {
    param([string[]]$Path)
    foreach ($p in $Path) {
        if (-not $p) { continue }
        if (-not (Test-Path -LiteralPath $p)) { continue }
        if (Test-Path -LiteralPath $TrashCmd) {
            & $TrashCmd -- $p | Out-Null
        } else {
            Write-Warning "找不到 mavis-trash.cmd，改用 Remove-Item：$p"
            Remove-Item -LiteralPath $p -Recurse -Force
        }
    }
}

function Get-NormalizedPath {
    param([string]$Path)
    if (-not $Path) { return '' }
    try {
        return [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    } catch {
        return $Path.TrimEnd('\')
    }
}

# 判定 $Child 是否等于 $Parent 或位于 $Parent 之下
function Test-IsUnder {
    param([string]$Child, [string]$Parent)
    $c = Get-NormalizedPath $Child
    $p = Get-NormalizedPath $Parent
    if (-not $c -or -not $p) { return $false }
    return ($c -eq $p) -or ($c.StartsWith($p + '\', [System.StringComparison]::OrdinalIgnoreCase))
}

# 确定性 zip：条目名一律用 '/'，按名字排序，保留每个文件的 mtime。
# 不走 Compress-Archive —— PS 5.1 的 Compress-Archive 用 '\' 做分隔符，
# 条目名在两个 PowerShell 版本上不一致，包就没法做字节级复现。
function New-DeterministicZip {
    param(
        [Parameter(Mandatory)][string]$SourceDir,
        [Parameter(Mandatory)][string]$DestZip
    )
    if (Test-Path -LiteralPath $DestZip) { Remove-Recyclable @($DestZip) }
    $srcLen = (Get-NormalizedPath $SourceDir).Length + 1
    $files = @(Get-ChildItem -LiteralPath $SourceDir -Recurse -File | Sort-Object {
        $_.FullName.Substring($srcLen).Replace('\', '/')
    })
    $fs = [System.IO.File]::Open($DestZip, [System.IO.FileMode]::CreateNew,
                                 [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($f in $files) {
                $rel = $f.FullName.Substring($srcLen).Replace('\', '/')
                $entry = $zip.CreateEntry($rel, [System.IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [System.DateTimeOffset]::new($f.LastWriteTime)
                $es = $entry.Open()
                try {
                    $in = [System.IO.File]::OpenRead($f.FullName)
                    try { $in.CopyTo($es) } finally { $in.Dispose() }
                } finally { $es.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $fs.Dispose() }
    return $files.Count
}

# 复制并保留 mtime（Copy-Item 本身保留，但显式设一次更保险）
function Copy-PreserveTime {
    param([string]$Source, [string]$Destination)
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $d = Get-Item -LiteralPath $Destination
    $s = Get-Item -LiteralPath $Source
    # MSVC 运行库那几个 DLL 带只读属性（安装器设的），只读文件改不了 mtime。
    # Copy-Item 本身已经保留 mtime，这里只是兜底；失败也不该让打包挂掉。
    try {
        if ($d.IsReadOnly) { $d.IsReadOnly = $false }
        $d.LastWriteTime = $s.LastWriteTime
    } catch {
        Write-Warning "设置 mtime 失败（$Destination）：$($_.Exception.Message)"
    }
}

# =============================================================================
# 1. 前置门禁（先拦，再动手）
# =============================================================================

Write-Step '1. 前置门禁'

# 1.1 BuildRoot 不能是受保护目录
foreach ($bad in $ForbiddenBuildRoots) {
    if (Test-IsUnder -Child $BuildRoot -Parent $bad) {
        throw "BuildRoot 指向受保护目录：$BuildRoot（属于 $bad）。`n" +
              "  这些目录被别的构建占用，编译会 LNK1104，用户会看到『Qt 打不开软件』。" +
              "请换一个独立目录，例如 $DeliverRoot\nightD。"
    }
}
# 1.2 BuildRoot 不能在源码树里
if (Test-IsUnder -Child $BuildRoot -Parent $ProjectRoot) {
    throw "BuildRoot 在源码树里面：$BuildRoot。构建产物不许落进源码目录。"
}
Say "   BuildRoot   : $BuildRoot  [OK]"

# 1.3 OutDir 不能是正式包目录
foreach ($bad in $ForbiddenOutDirs) {
    if (Test-IsUnder -Child $OutDir -Parent $bad) {
        throw "OutDir 指向正式包目录：$OutDir。预演/打包不允许覆盖正式包。"
    }
}
Write-Host "   OutDir      : $OutDir  [OK]"

# 1.4 容器名不能撞红线
foreach ($p in $ProtectedContainerNames) {
    if ($ContainerName -eq $p -or $ContainerName -like "$p*") {
        throw "容器名撞红线：$ContainerName（受保护：$p）"
    }
}
if ($ContainerName -notlike 'srt-nightd-*') {
    Write-Warning "容器名 $ContainerName 不在 srt-nightd-* 命名空间内 —— 约定用 srt-nightd-*。"
}

# 1.5 端口从 2430~2439 里挑一个没人用的
function Get-FreeContainerPort {
    param([int]$Preferred)
    $busy = @()
    $rows = @(docker ps --format '{{.Names}}|{{.Ports}}' 2>$null)
    foreach ($row in $rows) {
        if ($row -notmatch '\|') { continue }
        $nm = ($row -split '\|')[0]
        if ($nm -in $ProtectedContainerNames) { continue }
        foreach ($m in [regex]::Matches($row, ':(\d{4,5})->')) {
            $busy += [int]$m.Groups[1].Value
        }
    }
    foreach ($p in $ProtectedPortRange) { $busy += $p }
    $cands = @(2430..2439 | Where-Object { $_ -notin $busy })
    if ($Preferred -gt 0 -and $Preferred -in $cands) { return $Preferred }
    if ($cands.Count -gt 0) { return $cands[0] }
    throw '2430~2439 段没有空闲端口了。'
}
$ContainerPort = Get-FreeContainerPort -Preferred $ContainerPort
Write-Host "   Container   : $ContainerName (port $ContainerPort, image $ContainerImage)  [OK]"

# 1.5 输入存在性
foreach ($need in @($RuntimeDir, $PayloadDir)) {
    if (-not (Test-Path -LiteralPath $need)) { throw "缺少目录：$need" }
}
if (-not (Test-Path -LiteralPath $ProjectRoot)) { throw "缺少源码目录：$ProjectRoot" }

# =============================================================================
# 2. 编译（到独立 BuildRoot）
# =============================================================================

$ExePath = Join-Path $BuildRoot 'release\SyncReplayTool.exe'

if (-not $SkipBuild) {
    Write-Step "2. 编译 -> $BuildRoot"
    if ($script:Quiet) {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $ProjectRoot 'scripts\build-win.ps1') -BuildRoot $BuildRoot | Out-Null
    } else {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $ProjectRoot 'scripts\build-win.ps1') -BuildRoot $BuildRoot
    }
    if ($LASTEXITCODE -ne 0) { throw "编译失败（exit=$LASTEXITCODE）" }
}
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "编译产物不存在：$ExePath（-SkipBuild 时请确认 $BuildRoot 里已有上次的编译结果）"
}
$exeItem = Get-Item -LiteralPath $ExePath
Write-Host ("   编译产物   : {0}  {1}  {2} 字节" -f $exeItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $ExePath, $exeItem.Length)

# 源码最新修改时间（用于日志）
$newestSrc = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'src') -Recurse -File |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host ("   最新源文件 : {0}  {1}" -f $newestSrc.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $newestSrc.Name)

# =============================================================================
# 3. Windows zip
# =============================================================================

$WinStage = Join-Path $WorkRoot 'win-stage'
$WinZip   = Join-Path $OutDir '数据同步工具-Windows-x64.zip'
$kStage   = ''
$binStage = ''

if (-not $SkipWinZip) {
    Write-Step '3. Windows zip（白名单）'

    # 3.1 清 staging（幂等：旧产物不会混进新包）
    Remove-Recyclable @($WinStage)
    New-Item -ItemType Directory -Force -Path $WinStage | Out-Null

    # 3.2 收白名单：精确名
    #     主 exe 永远用**本次编译**的那一份（deploy 里那份是旧的）。
    $copied = 0
    $missing = @()
    Copy-PreserveTime -Source $ExePath -Destination (Join-Path $WinStage 'SyncReplayTool.exe')
    $copied++
    foreach ($name in $WinExactFiles) {
        if ($name -eq 'SyncReplayTool.exe') { continue }
        $src = Join-Path $RuntimeDir $name
        if (-not (Test-Path -LiteralPath $src)) {
            # 白名单里的 MSVC 运行库有两个（ucrtbase.dll / vcruntime140_1.dll）
            # 不在 deploy\ 里，只在系统目录。找不到就记为缺失。
            $found = $false
            foreach ($fb in $FallbackRuntimeDirs) {
                if (-not $fb) { continue }
                $cand = Join-Path $fb $name
                if (Test-Path -LiteralPath $cand) { $src = $cand; $found = $true; break }
            }
            if (-not $found) { $missing += $name; continue }
        }
        Copy-PreserveTime -Source $src -Destination (Join-Path $WinStage $name)
        $copied++
    }
    # qt.conf 与运维文档不在 deploy 里，分别从 payload / 源码树取
    if (-not (Test-Path -LiteralPath (Join-Path $WinStage 'qt.conf'))) {
        $qt = Join-Path $PayloadDir 'qt.conf'
        if (-not (Test-Path -LiteralPath $qt)) { $missing += 'qt.conf (payload 里也没有)' }
        else {
            Copy-PreserveTime -Source $qt -Destination (Join-Path $WinStage 'qt.conf')
            $copied++
            $missing = @($missing | Where-Object { $_ -ne 'qt.conf' })
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $WinStage '使用步骤与注意事项.md'))) {
        $doc = Get-ChildItem -LiteralPath $ProjectRoot -File -Filter '*.md' |
               Where-Object { $_.Name -like '*步骤*' -or $_.Name -like '*注意事项*' } | Select-Object -First 1
        if (-not $doc) { $missing += '使用步骤与注意事项.md (源码树里也没有)' }
        else {
            Copy-PreserveTime -Source $doc.FullName -Destination (Join-Path $WinStage '使用步骤与注意事项.md')
            $copied++
            $missing = @($missing | Where-Object { $_ -ne '使用步骤与注意事项.md' })
        }
    }
    if ($missing.Count -gt 0) {
        Write-Warning ("运行时有 {0} 个文件不在源目录里（已跳过）：{1}" -f $missing.Count, ($missing -join ', '))
    }

    # 3.3 收白名单：通配符
    foreach ($pat in $WinPatterns) {
        foreach ($f in @(Get-ChildItem -LiteralPath $RuntimeDir -File -Filter $pat -ErrorAction SilentlyContinue)) {
            Copy-PreserveTime -Source $f.FullName -Destination (Join-Path $WinStage $f.Name)
            $copied++
        }
    }

    # 3.4 收白名单：整目录
    foreach ($d in $WinDirs) {
        $dpath = Join-Path $RuntimeDir $d
        if (-not (Test-Path -LiteralPath $dpath)) { Write-Warning "运行时目录里没有 $d（跳过）"; continue }
        $dst = Join-Path $WinStage $d
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        foreach ($f in @(Get-ChildItem -LiteralPath $dpath -Recurse -File)) {
            $rel = $f.FullName.Substring($dpath.Length + 1)
            $target = Join-Path $dst $rel
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Copy-PreserveTime -Source $f.FullName -Destination $target
            $copied++
        }
    }
    Write-Host "   收进 staging : $copied 个文件"

    # ------------------------------------------------------------------
    # 3.5 排除门禁：10 个探针 / 旧版 exe 一个都不许在
    # ------------------------------------------------------------------
    $stageFiles = @(Get-ChildItem -LiteralPath $WinStage -Recurse -File)
    $hitExcluded = @($stageFiles | Where-Object { $_.Name -in $ExcludedExeNames })
    if ($hitExcluded.Count -gt 0) {
        throw "排除门禁失败：staging 里出现了不该有的 exe -> $($hitExcluded.Name -join ', ')"
    }
    Write-Host "   排除门禁   : 通过（10 个探针/旧版 exe 均不在 staging）"

    # ------------------------------------------------------------------
    # 3.6 启动器门禁 + 归一化
    #   上一版 Run.cmd 启动的是 SyncReplayTool2.exe —— 那个文件不在包里，
    #   客户双击启动器直接失败。而 SyncReplayTool2.exe 又在排除名单里。
    #   所以这里把启动器指向"包里唯一存在的 exe"，并把这件事变成硬门禁。
    # ------------------------------------------------------------------
    $runCmdPath = Join-Path $WinStage 'Run.cmd'
    if (Test-Path -LiteralPath $runCmdPath) {
        $txt = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($runCmdPath))
        $orig = $txt
        # (a) 通用规则：start "" "<任意>.exe" -> start "" "SyncReplayTool.exe"
        $txt = [regex]::Replace($txt, '(?i)(start\s+""\s*")([^"]+\.exe)(")', '${1}SyncReplayTool.exe${3}')
        # (b) 兜底：排除名单里的名字一律换掉
        foreach ($bad in $ExcludedExeNames) {
            $txt = $txt.Replace($bad, 'SyncReplayTool.exe')
        }
        if ($txt -ne $orig) {
            [System.IO.File]::WriteAllText($runCmdPath, $txt, (New-Object System.Text.UTF8Encoding($false)))
            $script:RunCmdNormalized = $true
            Write-Host "   启动器归一化 : Run.cmd 的启动目标已改为 SyncReplayTool.exe" -ForegroundColor Yellow
        }
        # 门禁：Run.cmd 不许再引用任何排除名单里的名字
        foreach ($bad in $ExcludedExeNames) {
            if ($txt.Contains($bad)) {
                throw "启动器门禁失败：Run.cmd 仍然引用被排除的 $bad"
            }
        }
        # 门禁：Run.cmd 指向的 exe 必须真的在包里
        $m = [regex]::Match($txt, '(?i)start\s+""\s*"([^"]+\.exe)"')
        if ($m.Success) {
            $target = $m.Groups[1].Value
            if (-not (Test-Path -LiteralPath (Join-Path $WinStage $target))) {
                throw "启动器门禁失败：Run.cmd 启动 $target，但包里没有这个文件"
            }
            $script:RunCmdTarget = $target
            Write-Host "   启动器门禁   : 通过（Run.cmd -> $target，包内存在）"
        }
    }

    # ------------------------------------------------------------------
    # 3.7 打 zip
    # ------------------------------------------------------------------
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $entryCount = New-DeterministicZip -SourceDir $WinStage -DestZip $WinZip
    $script:ZipEntries = $entryCount
    $script:StagedFiles = @(Get-ChildItem -LiteralPath $WinStage -Recurse -File).Count
    Write-Host ("   zip 条目数  : {0}" -f $entryCount)
    Write-Host ("   zip 大小    : {0} 字节" -f (Get-Item -LiteralPath $WinZip).Length)
}

# =============================================================================
# 4. 麒麟 tar.gz（必须在 Linux 容器里打）
# =============================================================================

$TarPath = Join-Path $OutDir '数据同步工具-麒麟.tar.gz'
$ContainerScript = Join-Path $ProjectRoot 'scripts\package-kylin-container.sh'

if (-not $SkipKylinTar) {
    Write-Step '4. 麒麟 tar.gz（Linux 容器）'

    if (-not (Test-Path -LiteralPath $ContainerScript)) { throw "缺少容器脚本：$ContainerScript" }
    if (-not (Test-Path -LiteralPath $KylinBaseTar)) { throw "缺少基础包：$KylinBaseTar" }
    if (-not (Test-Path -LiteralPath $PayloadDir)) { throw "缺少 payload 目录：$PayloadDir" }

    # 4.1 找 Linux ELF 二进制
    if (-not $KylinBinary) {
        $cands = @()
        foreach ($c in @(
            (Join-Path $DeliverRoot 'build-kylin\release\SyncReplayTool'),
            (Join-Path $DeliverRoot 'nightD\kylin\SyncReplayTool')
        )) { if (Test-Path -LiteralPath $c) { $cands += $c } }
        if ($cands.Count -eq 0) {
            Write-Warning '没找到 Linux 版 SyncReplayTool。请用 -KylinBinary <路径> 指定。'
            Write-Warning '（Windows 上编出来的 exe 不能打进麒麟包 —— 那是 PE，不是 ELF。）'
            throw '缺少 Linux ELF 二进制，麒麟包无法组装。'
        }
        $KylinBinary = $cands[0]
    }
    if (-not (Test-Path -LiteralPath $KylinBinary)) { throw "找不到 Linux 二进制：$KylinBinary" }
    Write-Host "   Linux 二进制: $KylinBinary"

    # 4.2 staging：把 payload 拷一份到自己的工作目录（**不改共享的 payload 目录**）
    $kStage = Join-Path $WorkRoot 'kylin-stage'
    Remove-Recyclable @($kStage)
    New-Item -ItemType Directory -Force -Path $kStage | Out-Null
    Copy-Item -Path (Join-Path $PayloadDir '*') -Destination $kStage -Recurse -Force
    # 二进制单独放一个只有它的目录，挂载时路径干净
    $binStage = Join-Path $WorkRoot 'kylin-bin'
    Remove-Recyclable @($binStage)
    New-Item -ItemType Directory -Force -Path $binStage | Out-Null
    Copy-PreserveTime -Source $KylinBinary -Destination (Join-Path $binStage 'SyncReplayTool')

    # 4.3 输出目录
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    if (Test-Path -LiteralPath $TarPath) { Remove-Recyclable @($TarPath) }

    # 4.4 清掉同名残留容器（只清自己的命名空间）
    $existing = @(docker ps -a --format '{{.Names}}' 2>$null | Where-Object { $_ -eq $ContainerName })
    foreach ($e in $existing) {
        if ($e -in $ProtectedContainerNames) { throw "拒绝清理受保护容器：$e" }
        docker rm -f $e | Out-Null
        Write-Host "   清理残留容器: $e"
    }

    # 4.5 跑容器
    #     只挂**只读**的目录：可写目录挂载会被本机安全策略拦下（"recursive
    #     write/delete target cannot be verified"）。产物因此打在容器自己的
    #     文件系统里，再用 `docker cp` 拷回宿主 —— 这也顺带保证了
    #     "不会污染/覆盖正式包目录"。
    #     容器命令行保持纯 ASCII：中文根目录名走 base64 传进去。
    $rootNameB64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes('数据同步工具-麒麟'))

    Write-Host "   容器内组装 ...（$ContainerName，端口 $ContainerPort）"
    $pkgCmd = "/pkg.sh --binary /build/SyncReplayTool --base-tar /stage/base.tar.gz " +
              "--payload /payload --root-name-b64 $rootNameB64 --out /out/kylin.tar.gz"
    docker create --name $ContainerName --entrypoint /bin/bash `
        -p "${ContainerPort}:22" `
        -v "${binStage}:/build:ro" `
        -v "${kStage}:/payload:ro" `
        -v "${KylinBaseTar}:/stage/base.tar.gz:ro" `
        -v "${ContainerScript}:/pkg.sh:ro" `
        $ContainerImage -c $pkgCmd | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "docker create 失败（exit=$LASTEXITCODE）" }

    $pkgLog = & docker start -a $ContainerName 2>&1
    $pkgRc = $LASTEXITCODE
    $pkgLog | ForEach-Object { Write-Host $_ }
    if ($pkgRc -ne 0) {
        docker rm -f $ContainerName | Out-Null
        throw "容器内打包失败（exit=$pkgRc）"
    }

    # 4.6 把产物拷回宿主（必须先 cp 再删容器）
    if (Test-Path -LiteralPath $TarPath) { Remove-Recyclable @($TarPath) }
    $cpLog = & docker cp "${ContainerName}:/out/kylin.tar.gz" "$TarPath" 2>&1
    $cpRc = $LASTEXITCODE
    docker rm -f $ContainerName | Out-Null
    if ($cpRc -ne 0 -or -not (Test-Path -LiteralPath $TarPath)) {
        $cpLog | ForEach-Object { Write-Host "     $_" }
        throw "容器内打包失败：产物没拷回来（$TarPath）"
    }
    Write-Host ("   tar.gz 已拷回: {0}  {1} 字节" -f $TarPath, (Get-Item -LiteralPath $TarPath).Length)
    # 机器可读标记（容器侧打印）
    foreach ($ln in $pkgLog) {
        $s = [string]$ln
        if ($s -match '^SRT_BIN_SHA256=([0-9a-fA-F]{64})') { $script:KylinMarkers['bin_sha256'] = $Matches[1].ToUpperInvariant() }
        elseif ($s -match '^SRT_ENTRIES=(\d+)') { $script:KylinMarkers['entries'] = [int]$Matches[1] }
        elseif ($s -match '^SRT_SYMLINKS=(\d+)') { $script:KylinMarkers['symlinks'] = [int]$Matches[1] }
        elseif ($s -match '^SRT_SELFCHK_MODE=(\S+)') { $script:KylinMarkers['selfcheck_mode'] = $Matches[1] }
    }
}

# =============================================================================
# 5. 汇总：字节数 + sha256
# =============================================================================

Write-Step '5. 产物清单（字节数 + sha256）'

$artifacts = @()
foreach ($a in @($WinZip, $TarPath)) {
    if (-not $a) { continue }
    if (-not (Test-Path -LiteralPath $a)) { continue }
    $it = Get-Item -LiteralPath $a
    $h = (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash
    $artifacts += [pscustomobject]@{
        Name    = $it.Name
        Path    = $it.FullName
        Bytes   = $it.Length
        Sha256  = $h
    }
    # 注意：PowerShell 变量名**大小写不敏感**，`$artifacts` 与 `$script:Artifacts`
    # 是同一个变量 —— 曾经因此把每个产物记了两遍。分开用不同的名字。
    $script:ArtifactList += [ordered]@{
        name   = $it.Name
        path   = $it.FullName
        bytes  = $it.Length
        sha256 = $h
    }
}
Write-Host ("   产物 {0} 个：" -f $artifacts.Count)
foreach ($a in $artifacts) {
    Write-Host ("     {0}" -f $a.Path)
    Write-Host ("       bytes  : {0}" -f $a.Bytes)
    Write-Host ("       sha256 : {0}" -f $a.Sha256)
}

# =============================================================================
# 6. 清理（临时文件不留）
# =============================================================================

if (-not $KeepStaging) {
    Remove-Recyclable @($WinStage, $kStage, $binStage)
    Write-Step '6. staging 已清理'
} else {
    Write-Step "6. staging 保留在 $WorkRoot"
}

Write-Host ''
Write-Host '=== 打包完成 ===' -ForegroundColor Green
Write-Host '下一步：跑 verify_delivery_package.ps1 做三道机械闸门。'

# =============================================================================
# 7. JSON 汇总（-Json 时这是唯一输出）
# =============================================================================

$jsonReport = [ordered]@{
    generated_at = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    build_root   = $BuildRoot
    out_dir      = $OutDir
    runtime_dir  = $RuntimeDir
    payload_dir  = $PayloadDir
    exe          = [ordered]@{
        path    = $ExePath
        bytes   = $exeItem.Length
        mtime   = $exeItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
        sha256  = (Get-FileHash -LiteralPath $ExePath -Algorithm SHA256).Hash
    }
    newest_source = [ordered]@{
        file  = $newestSrc.Name
        mtime = $newestSrc.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')
    }
    windows = [ordered]@{
        zip             = $WinZip
        entries         = $script:ZipEntries
        staged_files    = $script:StagedFiles
        excluded_names  = $ExcludedExeNames
        run_cmd_normalized = $script:RunCmdNormalized
        run_cmd_target      = $script:RunCmdTarget
    }
    kylin = [ordered]@{
        tar       = $TarPath
        base_tar  = $KylinBaseTar
        binary    = $KylinBinary
        markers   = $script:KylinMarkers
    }
    artifacts = @($script:ArtifactList)
}
# 注意：局部变量不能叫 $json —— PowerShell 变量名大小写不敏感，
# 会把参数 $Json（switch）覆盖掉，后面 `if ($Json)` 就炸了。
$jsonText = $jsonReport | ConvertTo-Json -Depth 8
if ($Json) { Write-Output $jsonText }
