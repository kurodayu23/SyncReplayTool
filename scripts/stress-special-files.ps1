<#
.SYNOPSIS
    SyncReplayTool 交付前「暴力/边界」测试：用产品同款 rsync/ssh + 同款 argv 做真实传输，
    覆盖产品源码里已经防御过的与边界相关的场景。测试数据落在干净的临时目录，不与交付包混。

    接收端：Docker 麒麟容器（port 2222,srtpw），见 run-all-tests.ps1 第 4 段口径。
#>
$ErrorActionPreference = 'Continue'
$w  = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\work'
$logs = Join-Path $w 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$log = Join-Path $logs 'stress-20261030.txt'
"===== 暴力/边界压力测试 $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') =====" | Tee-Object $log

$rsync = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\work\stage\tools\rsync.exe'
if (-not (Test-Path $rsync)) {
    # 用交付包 zip 里抽全量 msys 工具集（rsync 依赖 msys-*.dll，缺一个都起不来）
    $zip = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\交付包-20261102-021825\数据同步工具-Windows-x64.zip'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $z = [IO.Compression.ZipFile]::OpenRead($zip)
    $dst = Join-Path $w 'stage\tools'
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    foreach ($e in $z.Entries) {
        if ($e.Name -match '\.(exe|dll)$' -and $e.FullName -notmatch '/') {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $dst $e.Name), $true)
        }
    }
    $z.Dispose()
}
"rsync: $rsync $((Get-Item $rsync).VersionInfo.FileVersion)" | Tee-Object $log

# 环境
$src = Join-Path $w 'stage\stress-src'
$base = Join-Path $w 'stage\stress-base'
Remove-Item $src, $base -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $src, $base | Out-Null

$env:SSH = 'ssh'; $env:RSYNC_RSH='ssh'
$sshArgs = @('-p','2222','-o','PreferredAuthentications=password','-o','PubkeyAuthentication=no','-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=NUL','-o','NumberOfPasswordPrompts=1')
# 用 askpass 供密码
$ap = Join-Path $w 'stage\ap.bat'; Set-Content $ap '@echo SrtTest2026x' -Encoding ASCII
$env:SSH_ASKPASS = $ap; $env:SSH_ASKPASS_REQUIRE='force'; $env:DISPLAY=':0'

# 构造测试文件：特殊字符文件名 + 中文 + 空格 + 带 # 开头 + 大写
# 注意：Windows NTFS 文件名禁止 \ / : * ? " < > | 和控制字符（含换行），
# 所以换行文件名是 Linux 发送端专属场景（rsync --from0 防它），Windows 侧不测。
$specials = @(
    '#hash.spe',  '#.spe', 'space file.spe', '中文文件.spe', 'UPPER.SPE',
    "quote`'name.spe", , 'dollar$file.spe',
    'semi;colon.spe', 'brace{file}.spe', 'plus+file.spe', 'eq=file.spe',
    'amp&file.spe', '逗号,file.spe', 'pa(th).spe', 'percent%file.spe'
)
foreach ($n in $specials) {
    $full = Join-Path $src $n
    [IO.File]::WriteAllBytes($full, (New-Object byte[] (512 + (Get-Random 4096))))
}
# 大文件集：200 个 × 512KB
for ($i=0; $i -lt 200; $i++) {
    $f = Join-Path $src ("bulk{0:D4}.spe" -f $i)
    [IO.File]::WriteAllBytes($f, (New-Object byte[] (512*1024)))
}
# 子目录 + 嵌套
New-Item -ItemType Directory -Force -Path (Join-Path $src 'sub dir\deep') | Out-Null
[IO.File]::WriteAllBytes((Join-Path $src 'sub dir\deep\深目录.spe'), (New-Object byte[] 2048))
[IO.File]::WriteAllBytes((Join-Path $src 'sub dir\a b.spe'), (New-Object byte[] 256))

$all = Get-ChildItem $src -Recurse -File
"文件总数: $($all.Count)  总大小: $([math]::Round(($all | Measure-Object Length -Sum).Sum/1MB,1)) MB" | Tee-Object $log

# 生成 --files-from 清单（相对路径，NUL 分隔）——产品同款
$wd = $src
$listAbs = Join-Path $src '.srt-stress-fl.txt'
$payload = New-Object System.Text.StringBuilder
foreach ($f in $all) {
    $rel = $f.FullName.Substring($wd.Length+1)
    if ($rel.StartsWith('#')) { $rel = './' + $rel }
    [void]$payload.Append($rel); [void]$payload.Append([char]0)
}
[IO.File]::WriteAllText($listAbs, $payload.ToString(), (New-Object System.Text.UTF8Encoding($false)))

function Run-Try($name, [scriptblock]$body) {
    try { & $body; "PASS`t$name" | Tee-Object $log }
    catch { "FAIL`t$name`t$($_.Exception.Message)" | Tee-Object $log }
}

Push-Location $src
# 1) 全量（checksum 口径 == 产品不勾"仅新增"）
Run-Try 'transfer-full-checksum' {
    & $rsync -a --no-o --no-g --partial --partial-dir=.srt-partial --checksum --protect-args --from0 "--files-from=$($listAbs.Substring($wd.Length+1))" --out-format=%n --info=progress2 -z -e "ssh -p 2222 -o PreferredAuthentications=password -o PubkeyAuthentication=no -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o NumberOfPasswordPrompts=1" ./ srtpw@127.0.0.1:/home/srtpw/srt-in/ 2>&1 | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { throw "rc=$LASTEXITCODE" }
}
# 2) 二次同步（应几乎无传输）
Run-Try 'transfer-second-idempotent' {
    & $rsync -a --checksum --protect-args --from0 "--files-from=$($listAbs.Substring($wd.Length+1))" --partial --partial-dir=.srt-partial -e "ssh -p 2222 -o PreferredAuthentications=password -o PubkeyAuthentication=no -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL" ./ srtpw@127.0.0.1:/home/srtpw/srt-in/ 2>&1 | Select-Object -Last 2 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "rc=$LASTEXITCODE" }
}
Pop-Location
"===== 结束 =====" | Tee-Object $log
Get-Content $log | Select-Object -Last 12