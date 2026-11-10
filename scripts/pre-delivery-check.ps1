# 交付前体检：一条命令给出「能不能出包」的判定
# 用法: powershell -File scripts\pre-delivery-check.ps1
$ErrorActionPreference = 'Continue'
$root = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$proj = 'D:\AP\AP7000\SyncReplayTool'
$bad = 0

function Check($name, $ok, $detail) {
    if ($ok) { Write-Host ("  PASS  {0,-34} {1}" -f $name, $detail) -ForegroundColor Green }
    else { Write-Host ("  FAIL  {0,-34} {1}" -f $name, $detail) -ForegroundColor Red; $script:bad++ }
}

Write-Host "===== 交付前体检 =====" -ForegroundColor Cyan

# 1 工作区干净
$st = (& git -C $proj status --porcelain 2>$null)
Check 'git 工作区干净' ([string]::IsNullOrWhiteSpace(($st -join ''))) '未提交改动应为 0'

# 2 构建指纹与 HEAD 一致
$head = (& git -C $proj rev-parse HEAD 2>$null)
$stampFile = Join-Path $root 'build-Release\build-stamp.txt'
$stamp = @{}
if (Test-Path $stampFile) { Get-Content $stampFile | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { $stamp[$Matches[1].Trim()] = $Matches[2].Trim() } } }
Check 'exe 指纹 == HEAD' ($stamp['git-head'] -eq $head) "stamp=$($stamp['git-head']) head=$head"
Check 'exe 构建时工作区干净' ($stamp['git-tree'] -eq 'clean') "git-tree=$($stamp['git-tree'])"

# 3 三处 exe 一致
$exes = @(
    (Join-Path $root 'build-Release\release\SyncReplayTool.exe'),
    (Join-Path $root 'release\SyncReplayTool.exe'),
    (Join-Path $root 'deploy\SyncReplayTool.exe')
)
$hashes = $exes | ForEach-Object { if (Test-Path $_) { (Get-FileHash $_ -Algorithm SHA256).Hash.Substring(0,16) } else { 'MISSING' } }
Check 'release/deploy/构建产物同哈希' (($hashes | Select-Object -Unique).Count -eq 1) ($hashes -join ' / ')

# 4 自检
$cfg = Join-Path $env:TEMP ("cfg_predel_" + [guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Force -Path $cfg | Out-Null
$env:SRT_CONFIG_DIR = $cfg; $env:QT_QPA_PLATFORM = 'offscreen'
$o = cmd /c ('"' + $exes[0] + '" --selfcheck 2>&1') 2>&1
$rc = $LASTEXITCODE
Remove-Item Env:SRT_CONFIG_DIR,Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
$pass = @($o | Where-Object { $_ -match '^PASS' }).Count
$fail = @($o | Where-Object { $_ -match '^FAIL' }).Count
# **必须给 PASS 数一个下界**（2026-11-10 审查 P1-1）：
# 原来判据是 `$rc -eq 0 -and $fail -eq 0`。把一个"立刻 exit 0 但什么都不打印"的
# 程序替换成 exe，$rc=0、$fail=0、$pass=0 ⇒ 这一项照样判绿 —— 闸门自己假通过。
# 现在要求至少 100 条 PASS（当前真实值 104），数量对不上就是自检没真跑。
Check '自检 全绿' ($rc -eq 0 -and $fail -eq 0 -and $pass -ge 100) "PASS=$pass FAIL=$fail rc=$rc（PASS 至少 100）"

# 5 最新交付包的校验值自洽
$pkg = Get-ChildItem $root -Directory -Filter '交付包-*' | Sort-Object Name | Select-Object -Last 1
if ($pkg) {
    $mf = Get-ChildItem $pkg.FullName -Filter '*版本与校验值.txt' | Select-Object -First 1
    $zip = Get-ChildItem $pkg.FullName -Filter '*.zip' | Select-Object -First 1
    $ok = $false; $d = 'no manifest'
    if ($mf -and $zip) {
        # 按行解析清单（不用 PostContext，那个取法不可靠，曾误判"包坏了"）
        $lines = Get-Content $mf.FullName
        $want = $null
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match [regex]::Escape($zip.Name)) {
                for ($j = $i; $j -lt [Math]::Min($i + 4, $lines.Count); $j++) {
                    if ($lines[$j] -match 'sha256\s*:\s*([0-9A-Fa-f]{64})') { $want = $Matches[1].ToUpper(); break }
                }
                break
            }
        }
        $got = (Get-FileHash $zip.FullName -Algorithm SHA256).Hash.ToUpper()
        # **$want 可能为 $null**（2026-11-10 审查 P1-2）：
        # 清单里没找到该 zip 的 sha256 行时，$want 停在 $null，
        # 原来第 67 行的 $want.Substring(0,16) 会抛"不能对值为 Null 的表达式调用方法"，
        # 在 $ErrorActionPreference='Continue' 下这是终止性语句错误 ⇒ 脚本中断、
        # 第 5 项根本没记 FAIL、最后那句「结论：不要出包」也不会打印。
        # 现在先判空：找不到摘要本身就是失败，并且说清楚是"清单里没有"而不是"不匹配"。
        if (-not $want) {
            $ok = $false
            $d = "$($zip.Name) 清单里没找到它的 sha256 行（实际 $($got.Substring(0,16))）"
        } else {
            $ok = ($want -eq $got)
            $d = "$($zip.Name) 清单=$($want.Substring(0,16)) 实际=$($got.Substring(0,16))"
        }
    }
    Check '交付包校验值自洽' $ok $d
} else { Check '交付包校验值自洽' $false '没有交付包' }

Write-Host ""
if ($bad -eq 0) { Write-Host "结论：可以出包（$bad 项失败）" -ForegroundColor Green; exit 0 }
else { Write-Host "结论：不要出包（$bad 项失败）" -ForegroundColor Red; exit 1 }
