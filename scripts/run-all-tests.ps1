<#
.SYNOPSIS
    SyncReplayTool 交付前全量测试一键跑。

.DESCRIPTION
    跑完全部段落，任何一段失败都会在 SUMMARY.md 里标红：

      1. 构建      —— 编到 Qt Creator 活动套件目录（build-Release）
      2. 打包      —— 用刚编出来的那份产物打一个**新**交付包（2026-11-02 新增）
      3. 纯逻辑    —— --selfcheck（不开窗口，参数装配 / 归因 / 看门狗 / 屏幕矩阵）
      4. GUI 交互  —— --selfcheck-gui（点击真实控件）
      5. 真机同步  —— --selfcheck-secrets（Windows 发送端 -> 麒麟接收端，验证无明文口令残留）
      6. 麒麟包    —— 容器内 verify-kylin.sh（wrapper 不挂死、包结构、安装脚本）
      7. 交付门禁  —— verify_delivery_package.ps1（时间戳 / 中文文案 / 结构 / 排除）

    ★ 为什么必须有第 2 段「打包」（2026-11-02）：
      交付门禁验的是「包内 exe 与构建产物逐位一致」。Windows 的 exe **不是确定性构建**
      （PE 里有时间戳），所以**重编一次，字节就变**。
      以前这段脚本没有「打包」，于是「重编 → 跑门禁」必然报
      「内容与源不一致」的**假 FAIL**（2026-11-02 首次复跑就撞上了）。
      加上打包段之后，门禁比的就是刚打出来的那份包 —— 结论才有意义。

    第 5 段依赖一个带 sshd 的麒麟接收端容器，脚本会自己拉起并配好密码；
    容器镜像不在或 docker 未开时该段记 SKIP，不阻塞其余段。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File run-all-tests.ps1
#>
[CmdletBinding()]
param(
    [string]$QtKitBuild = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\build-Release',
    [string]$Exe        = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\build-Release\release\SyncReplayTool.exe',
    [string]$PackageRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool',
    # 工作根放**构建目录**，不放 ai-workspace：后者是会被定期清空的临时区，
    # 清一次就再也跑不成测试了（2026-11-02 已把默认路径搬到这里）。
    [string]$WorkRoot   = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\work',
    [string]$Container  = 'srt-e2e-recv',
    [int]   $E2ePort    = 2222,
    [string]$E2eUser    = 'srtpw',
    [string]$E2ePass    = 'SrtTest2026x',
    [switch]$SkipBuild,
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Continue'
$logs = Join-Path $WorkRoot 'logs'
$stage = Join-Path $WorkRoot 'stage'
New-Item -ItemType Directory -Force -Path $logs, $stage | Out-Null

# ---------------------------------------------------------------- 配置隔离
#
# 这一整轮测试跑的都是产品的**探针模式**（--selfcheck / --selfcheck-gui /
# --screenshot / --selfcheck-secrets），而探针模式会把本机目录、目标地址这些
# 写进**用户自己的** %APPDATA%\AP\SyncReplayTool.ini：
#   · --selfcheck-secrets 写 host/port/remoteRoot/localRoot
#   · --screenshot 写 localRoot（截图探针把样例目录塞进去）
# 跑完一轮测试，用户自己的连接信息就被覆盖成 127.0.0.1:2222 和一个
# 已经不存在的临时目录 —— 界面随即显示「本机目录不存在」。
# （2026-11-02 实测踩到，已把用户那台机器的配置还原。）
#
# 隔离用产品自带的 SRT_CONFIG_DIR 开关（ConfigStore::configDirOverride）。
# 注意不能用 APPDATA 环境变量：QSettings 走 SHGetFolderPath，读注册表不看环境变量
# （ConfigStore.cpp 里写了同样的说明）。
$cfgDir = Join-Path $stage 'selfcheck-config'
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$cfgIni = Join-Path $cfgDir 'SyncReplayTool.ini'
if (-not (Test-Path $cfgIni)) {
    $cfgLines = @('[app]','rememberSettings=true','','[sync]','autoSyncEnabled=false','onlyNewFiles=false','autoSyncIntervalSec=30','bandwidthLimitMBps=0','','[syncRules]','size=1','1\suffix=spe','1\remoteSubDir=','','[target]','host=','port=22','user=','remoteRoot=','localRoot=','autoAcceptHostKey=true')
    $cfgLines | Out-File -FilePath $cfgIni -Encoding UTF8
}
$env:SRT_CONFIG_DIR = $cfgDir

$results = New-Object System.Collections.ArrayList
function Record($name, $status, $detail) {
    [void]$results.Add([pscustomobject]@{ 段 = $name; 结果 = $status; 说明 = $detail })
    $color = if ($status -eq 'PASS') { 'Green' } elseif ($status -eq 'SKIP') { 'Yellow' } else { 'Red' }
    Write-Host ("  {0,-6} {1}  {2}" -f $status, $name, $detail) -ForegroundColor $color
}

# ---------------------------------------------------------------- 1. 构建
if (-not $SkipBuild) {
    Write-Host "`n===== 1. 构建（Qt 5.9.6 / msvc2015_64）=====" -ForegroundColor Cyan
    # exe 被占用时 MSVC 只会报 LNK1104「无法打开文件 release\SyncReplayTool.exe」，
    # 看起来像编译错误，实际是上一轮的产品进程（或被别处启动的实例）没退。
    # 不先清掉的话每轮构建都假红（2026-11-04 实测撞过一次）。
    Get-Process SyncReplayTool -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host ("  停掉占用 exe 的进程 pid=" + $_.Id) -ForegroundColor Yellow
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 800
    & powershell -ExecutionPolicy Bypass -File 'D:\AP\AP7000\SyncReplayTool\scripts\build-win.ps1' -BuildRoot $QtKitBuild 2>&1 |
        Out-File -FilePath (Join-Path $logs 'build.log') -Encoding UTF8
    if ($LASTEXITCODE -eq 0 -and (Test-Path $Exe)) {
        $newestSrc = Get-ChildItem 'D:\AP\AP7000\SyncReplayTool\src\*.cpp','D:\AP\AP7000\SyncReplayTool\src\*.h' |
                     Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ((Get-Item $Exe).LastWriteTime -lt $newestSrc.LastWriteTime) {
            Record '构建' 'FAIL' "产物比源码旧（$($newestSrc.Name)），Qt Creator 里看到的不是新代码"
        } else {
            Record '构建' 'PASS' "exe $((Get-Item $Exe).LastWriteTime)"
        }
    } else {
        Record '构建' 'FAIL' "build-win.ps1 退出码 $LASTEXITCODE"
    }
} else {
    Record '构建' 'SKIP' '按参数跳过'
}

# ---------------------------------------------------------------- 1b. 麒麟交叉编译
#
# 为什么七段里必须有这一段（2026-11-05 实测）：构建段只编 Windows，麒麟二进制
# 靠另外一条命令手工编。改了源码只跑七段 → 麒麟包里还是几小时前的旧代码，
# 而"打包"段照样把旧二进制装进去、门禁也照样能过（除非文案变了才暴露）。
# 实测就撞上了：麒麟二进制比源码旧 502 分钟，闸门二报「1/453 条文案找不到」。
# 所以：源码比麒麟二进制新 → 自动重编；没改就跳过（省 6 分钟）。
if (-not $SkipBuild) {
    $kylinBin = Join-Path $PackageRoot 'build-kylin\release\SyncReplayTool'
    $newestSrcAll = Get-ChildItem 'D:\AP\AP7000\SyncReplayTool\src\*.cpp','D:\AP\AP7000\SyncReplayTool\src\*.h' |
                    Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $kylinStale = (-not (Test-Path $kylinBin)) -or
                  ((Get-Item $kylinBin).LastWriteTime -lt $newestSrcAll.LastWriteTime)
    if ($kylinStale) {
        Write-Host "`n===== 1b. 麒麟交叉编译（源码比产物新）=====" -ForegroundColor Cyan
        $tc = Join-Path $PackageRoot 'qt596-toolchain'
        if (-not (Test-Path $tc)) {
            Record '麒麟编译' 'FAIL' "找不到 $tc（交叉编译工具链不在）"
        } else {
            # -v 的值必须先拼成字符串：写成 -v (Join-Path ...)`:/work 时
            # PowerShell 会把它拆成两个参数，docker 报 "invalid reference format"（rc=125）。
            #
            # ★ 这两行**必须排在 docker run 之前**（2026-11-05 13:53 实测踩过）：
            #   写成
            #       docker run --rm `
            #           -v "${tc}:/toolchain:ro" `
            #           -v "D:\...\:ro" `
            #       $kbWork = Join-Path ...
            #           New-Item ...
            #       $kbOut  = Join-Path ...
            #           -v "${kbWork}:/work" `
            #           -v "${kbOut}:/out" `
            #   PowerShell 会在**没有反引号**的那行（$kbWork = ...）把整个命令截断：
            #   docker 只收到 `run --rm -v ... -v ...`（**没有镜像名**），
            #   报 "invalid reference format"（rc=125），而后面那两行 -v 变成
            #   PowerShell 自己的命令（"-v" 不是 cmdlet）直接炸。
            #   ⇒ 变量先算好，docker 命令写成一条**完整连续**的续行块。
            $kbWork = Join-Path $WorkRoot 'kylin-build'
            New-Item -ItemType Directory -Force -Path $kbWork | Out-Null
            $kbOut  = Join-Path $PackageRoot 'build-kylin'
            docker run --rm `
                -v "${tc}:/toolchain:ro" `
                -v "D:\AP\AP7000\SyncReplayTool:/src:ro" `
                -v "${kbWork}:/work" `
                -v "${kbOut}:/out" `
                --entrypoint /bin/bash kylin-v10-real:latest `
                -c "cd /src && bash scripts/build-kylin-qt596.sh" 2>&1 |
                Out-File -FilePath (Join-Path $logs 'kylin-build.log') -Encoding UTF8
            if ($LASTEXITCODE -eq 0 -and (Test-Path $kylinBin)) {
                Record '麒麟编译' 'PASS' "二进制 $((Get-Item $kylinBin).LastWriteTime)"
            } else {
                Record '麒麟编译' 'FAIL' "rc=$LASTEXITCODE（见 logs\kylin-build.log）"
            }
        }
    } else {
        Record '麒麟编译' 'SKIP' '产物已比源码新'
    }
} else {
    Record '麒麟编译' 'SKIP' '按参数跳过构建'
}

# ---------------------------------------------------------------- 2. 打包
#
# ★ 这一段是 2026-11-02 补上的，理由写在文件头的 .DESCRIPTION 里：
#   Windows 的 exe 不是确定性构建（PE 时间戳），重编一次字节就变。
#   没有这一段时，「重编 → 跑交付门禁」必然报「内容与源不一致」的假 FAIL。
#   打完包之后，第 6/7 段验的就是刚打出来的那份 —— 结论才有意义。
$pkgDir = $null
if ($SkipPackage) {
    $d = Get-ChildItem $PackageRoot -Directory |
         Where-Object { $_.Name -like '交付包*' } |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($d) { $pkgDir = $d }
    Record '打包' 'SKIP' "按参数跳过，沿用 $($d.Name)"
} else {
    Write-Host "`n===== 2. 打包（用刚编出来的那份产物）=====" -ForegroundColor Cyan
    $pkgPath = Join-Path $PackageRoot ('交付包-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $pkgPath | Out-Null
    # 必须把刚交叉编译出来的麒麟二进制传进去。不传的话 package-delivery.ps1
    # 会用它的默认值/跳过对账，麒麟包里就不是这一轮的产物 ——
    # 交付门禁那句「未提供 -KylinBinary，跳过冒烟对账」正是在提示这件事（2026-11-04 发现）。
    $kylinBin = Join-Path $PackageRoot 'build-kylin\release\SyncReplayTool'
    if (-not (Test-Path $kylinBin)) {
        Record '打包' 'FAIL' "找不到麒麟二进制 $kylinBin（先跑 build-kylin-qt596.sh）"
        $kylinBin = $null
    }
    $pkgArgs = @('-BuildRoot', $QtKitBuild, '-OutDir', $pkgPath, '-SkipBuild')
    if ($kylinBin) { $pkgArgs += @('-KylinBinary', $kylinBin) }
    & powershell -ExecutionPolicy Bypass -File 'D:\AP\AP7000\SyncReplayTool\scripts\package-delivery.ps1' @pkgArgs 2>&1 |
        Out-File -FilePath (Join-Path $logs 'package.log') -Encoding UTF8
    $pkgRc = $LASTEXITCODE
    $winZip = Get-ChildItem $pkgPath -Filter '*Windows*.zip' -ErrorAction SilentlyContinue | Select-Object -First 1
    $tar    = Get-ChildItem $pkgPath -Filter '*麒麟*.tar.gz' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pkgRc -eq 0 -and $winZip -and $tar) {
        $pkgDir = Get-Item $pkgPath
        Record '打包' 'PASS' "$($winZip.Name) / $($tar.Name)"
    } else {
        Record '打包' 'FAIL' "rc=$pkgRc（见 logs\package.log）"
    }
}

if (-not (Test-Path $Exe)) {
    Record '后续全部' 'FAIL' "找不到 $Exe"
} else {

# ---------------------------------------------------------------- 2. 纯逻辑自检
Write-Host "`n===== 2. --selfcheck =====" -ForegroundColor Cyan
cmd /c "`"$Exe`" --selfcheck > `"$logs\selfcheck.txt`" 2>&1"
$rc = $LASTEXITCODE
$pass = (Select-String -Path (Join-Path $logs 'selfcheck.txt') -Pattern '^PASS').Count
$fail = (Select-String -Path (Join-Path $logs 'selfcheck.txt') -Pattern '^FAIL').Count
if ($rc -eq 0 -and $fail -eq 0) { Record '纯逻辑自检' 'PASS' "PASS=$pass FAIL=0" }
else { Record '纯逻辑自检' 'FAIL' "rc=$rc PASS=$pass FAIL=$fail" }

# ---------------------------------------------------------------- 2b. 源码静态扫描
#
# ★ 为什么必须有这一段（2026-11-05 实测）：
#   scripts/scan_no_cjk_qstringliteral.py 从写下来那天起就写着「防止再漏网的闸门」，
#   但它**从来没有接进任何一段** —— 于是它要防的东西一次也没真被检查过。
#   这次接进来立刻抓到一处真 bug：
#     u8"…\xNN…" 字面量里 \xNN 是**字符**不是字节（Qt 5.9.6 / MSVC2015 /utf-8 实测），
#     fromUtf8 解出来是 15 个 Latin-1 字符的乱码；
#     而自检里比对同一句文案的判据**也拿乱码比对** ⇒ 那条判据恒绿，
#     产品明明打了「复制未成功」，判据却报 failMsg=0 —— 判据自己把 bug 盖住了。
#   源码层面拦不住这种（字面量里根本没有汉字字节，grep 看不见），
#   所以必须让这段常驻门禁。
Write-Host "`n===== 2b. 源码静态扫描（中文字面量 4 条检查）=====" -ForegroundColor Cyan
$scanTool = 'D:\AP\AP7000\SyncReplayTool\scripts\scan_no_cjk_qstringliteral.py'
if (-not (Test-Path $scanTool)) {
    Record '源码扫描' 'FAIL' "找不到 $scanTool"
} else {
    $scanOut = Join-Path $logs 'scan-cjk.txt'
    & python $scanTool 2>&1 | Out-File -FilePath $scanOut -Encoding UTF8
    $scanRc = $LASTEXITCODE
    if ($scanRc -eq 0) { Record '源码扫描' 'PASS' '中文字面量 4 条检查全过' }
    else { Record '源码扫描' 'FAIL' "rc=$scanRc（见 logs\scan-cjk.txt）" }
}

# ---------------------------------------------------------------- 3. GUI 交互自检
Write-Host "`n===== 3. --selfcheck-gui =====" -ForegroundColor Cyan
cmd /c "`"$Exe`" --selfcheck-gui > `"$logs\selfcheck-gui.txt`" 2>&1"
$rc = $LASTEXITCODE
$summary = (Select-String -Path (Join-Path $logs 'selfcheck-gui.txt') -Pattern 'GUI_SUMMARY').Line
if ($rc -eq 0 -and $summary -match 'fail=0') { Record 'GUI 交互' 'PASS' $summary.Trim() }
else { Record 'GUI 交互' 'FAIL' "rc=$rc $summary" }

# ---------------------------------------------------------------- 3b. 截图探针
Write-Host "`n===== 3b. 截图探针 =====" -ForegroundColor Cyan
$shotRoot = Join-Path $WorkRoot 'probes\shotroot'
New-Item -ItemType Directory -Force -Path $shotRoot | Out-Null
1..3 | ForEach-Object { Set-Content -Path (Join-Path $shotRoot "sample$_.spe") -Value ('x' * 2048) }
$env:SRT_SHOT_ROOT = $shotRoot
cmd /c "`"$Exe`" --screenshot `"$WorkRoot\probes\win-main-latest.png`""
cmd /c "`"$Exe`" --screenshot-settings `"$WorkRoot\probes\win-settings-latest.png`""
$m = Join-Path $WorkRoot 'probes\win-main-latest.png'
$s = Join-Path $WorkRoot 'probes\win-settings-latest.png'
if ((Test-Path $m) -and (Test-Path $s) -and (Get-Item $m).Length -gt 8000 -and (Get-Item $s).Length -gt 8000) {
    Record '截图探针' 'PASS' "主窗 $((Get-Item $m).Length) B / 设置 $((Get-Item $s).Length) B"
} else {
    Record '截图探针' 'FAIL' '截图缺失或过小（可能是空窗）'
}
Remove-Item Env:SRT_SHOT_ROOT -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- 4. 真机同步（发送端 -> 麒麟接收端）
Write-Host "`n===== 4. --selfcheck-secrets（真实同步）=====" -ForegroundColor Cyan
$dockerOk = $false
docker version --format '{{.Server.Version}}' | Out-Null
if ($LASTEXITCODE -eq 0) { $dockerOk = $true }
if (-not $dockerOk) {
    Record '真机同步' 'SKIP' 'docker 未运行，跳过往返传输验证'
} else {
    docker rm -f $Container 2>&1 | Out-Null
    docker run -d --name $Container -p "${E2ePort}:22" kylin-v11-sshd:latest 2>&1 | Out-Null
    Start-Sleep -Seconds 8
    # 容器里的 sshd 默认没起，需要手动拉起；用分号串，别用 || （PS 5.1 会解析它）
    $inner = "echo '$($E2eUser):$($E2ePass)' | chpasswd; mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$($E2eUser)/srt-in; chown -R $($E2eUser):$($E2eUser) /home/$($E2eUser)/srt-in"
    docker exec $Container sh -c $inner | Out-Null
    Start-Sleep -Seconds 3
    $env:SRT_SC_HOST = '127.0.0.1'
    $env:SRT_SC_PORT = "$E2ePort"
    $env:SRT_SC_USER = $E2eUser
    $env:SRT_SC_PASS = $E2ePass
    $env:SRT_SC_REMOTE = "/home/$E2eUser/srt-in"
    cmd /c "`"$Exe`" --selfcheck-secrets 2 > `"$logs\selfcheck-secrets.txt`" 2>&1"
    $rc = $LASTEXITCODE
    $tail = (Get-Content (Join-Path $logs 'selfcheck-secrets.txt') -Tail 3) -join ' | '
    if ($rc -eq 0 -and $tail -match 'SECRETS_ALL_CLEAN') { Record '真机同步' 'PASS' '2 轮真实传输成功且无明文口令残留' }
    else { Record '真机同步' 'FAIL' $tail }
    Remove-Item Env:SRT_SC_HOST, Env:SRT_SC_PORT, Env:SRT_SC_USER, Env:SRT_SC_PASS, Env:SRT_SC_REMOTE -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- 5. 麒麟包容器验证
Write-Host "`n===== 5. 麒麟包容器验证 =====" -ForegroundColor Cyan
if (-not $pkgDir) {
    Record '麒麟包验证' 'SKIP' '没有可用的交付包目录'
} else {
    $tar = Get-ChildItem $pkgDir.FullName -Filter '*麒麟*.tar.gz' | Select-Object -First 1
    if (-not $tar) {
        Record '麒麟包验证' 'SKIP' "$($pkgDir.Name) 里没有麒麟包"
    } else {
        Copy-Item $tar.FullName (Join-Path $stage 'srt-kylin.tar.gz') -Force
        $out = docker run --rm -v "${stage}:/pkg:ro" -v "D:\AP\AP7000\SyncReplayTool\scripts:/scripts:ro" `
                --entrypoint /bin/bash kylin-v10-real:latest -c "bash /scripts/verify-kylin.sh /pkg/srt-kylin.tar.gz" 2>&1
        $out | Out-File (Join-Path $logs 'verify-kylin.log') -Encoding UTF8
        $line = ($out | Select-String 'PASS=\d+\s+FAIL=\d+').Line
        if ($line -match 'FAIL=0') { Record '麒麟包验证' 'PASS' $line.Trim() }
        else { Record '麒麟包验证' 'FAIL' ($out | Select-Object -Last 3) }
    }
}

# ---------------------------------------------------------------- 6. 交付门禁
Write-Host "`n===== 6. 交付门禁 =====" -ForegroundColor Cyan
if (-not $pkgDir) {
    Record '交付门禁' 'SKIP' '没有可用的交付包目录'
} else {
    # 必须把 -BuildRoot 指给"包内 exe 的出处"，否则闸门一会拿 scripts 默认的
    # nightD 旧 exe 去比对新包 —— 源码一改，包内 exe 与旧 nightD 必然不同，
    # 门禁就报「内容与源不一致」的假 FAIL（本脚本 2026-10-30 因此误报过一次）。
    # 默认取 Qt Creator 套件目录：构建段就是往那儿编的，包也是同一份源码打的。
    $gateBuildRoot = if ($env:SRT_GATE_BUILDROOT) { $env:SRT_GATE_BUILDROOT } else { $QtKitBuild }
    # 必须用 cmd 做**字节级**重定向。verify_delivery_package.ps1 是外部 powershell
    # 进程，中文走控制台编码（本机 GBK）；用 PowerShell 管道接它，会先按 UTF-8
    # 解码 GBK 字节（乱码）、再以 UTF-8 写文件 —— 双重编码损坏且不可逆，
    # 「总判定」四个字再也匹配不回来（2026-11-04 实测：日志里明明写着 PASS，
    # 本脚本却报「日志里没有总判定行」，两种编码都读不出来）。
    # 判定**只看退出码**，不解析日志里的中文。
    #
    # 为什么不解析：verify_delivery_package.ps1 的输出中文走控制台编码，
    # 经 PowerShell 管道会被按 UTF-8 误解码再以 UTF-8 写盘，双重编码损坏后
    # 「总判定」再也匹配不回来（2026-11-04 实测：日志里明明写着 PASS，
    # 本脚本报「没有总判定行」）。用 Start-Process / cmd 做字节重定向也会踩
    # 另一个坑：那个上下文里 Get-FileHash 不可用，门禁脚本第一行就崩。
    #
    # 退出码的语义是明确的：0 = 三道闸门全 PASS；非 0 = 有闸门 FAIL 或脚本崩了。
    # 宁可「崩了也算红」，不能让它假绿。
    $gateLogPath = Join-Path $logs 'delivery-gate.log'
    & powershell -ExecutionPolicy Bypass -File 'D:\AP\AP7000\SyncReplayTool\scripts\verify_delivery_package.ps1' `
        -PackageDir $pkgDir.FullName -BuildRoot $gateBuildRoot 2>&1 |
        Out-File -FilePath $gateLogPath -Encoding UTF8
    $gateRc = $LASTEXITCODE
    if ($gateRc -eq 0) {
        Record '交付门禁' 'PASS' 'rc=0（三道闸门全绿，详 logs\delivery-gate.log）'
    } else {
        $tail = @()
        if (Test-Path $gateLogPath) {
            $tail = (Get-Content $gateLogPath -Encoding UTF8 -ErrorAction SilentlyContinue | Select-Object -Last 6)
        }
        Record '交付门禁' 'FAIL' ("rc=$gateRc BuildRoot=$gateBuildRoot 尾部：" + ($tail -join ' | '))
    }
}
}

# ---------------------------------------------------------------- 汇总
$bad = ($results | Where-Object 结果 -eq 'FAIL').Count
$summaryPath = Join-Path $WorkRoot 'SUMMARY.md'
$md = @()
$md += "# SyncReplayTool 全量测试汇总"
$md += ""
$md += "- 时间：$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
$md += "- 结论：$(if ($bad -eq 0) { '全部通过' } else { "$bad 段失败" })"
$md += ""
$md += "| 段 | 结果 | 说明 |"
$md += "|---|---|---|"
foreach ($r in $results) { $md += "| $($r.段) | $($r.结果) | $($r.说明 -replace '\|','/') |" }
$md += ""
$md += "日志目录：$logs"
$md | Out-File $summaryPath -Encoding UTF8
Write-Host "`n汇总：$summaryPath" -ForegroundColor Cyan
$results | Format-Table -AutoSize
# 隔离用的环境变量在整轮跑完之前一直保留（各段都在同一个隔离配置里跑），
# 到这里才撤掉，免得中途某段没继承到。
Remove-Item Env:SRT_CONFIG_DIR -ErrorAction SilentlyContinue
if ($bad -gt 0) { exit 1 } else { exit 0 }
