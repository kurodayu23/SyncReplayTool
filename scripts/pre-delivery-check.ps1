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
Check '自检 全绿' ($rc -eq 0 -and $fail -eq 0) "PASS=$pass FAIL=$fail rc=$rc"

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
        $ok = ($want -eq $got)
        $d = "$($zip.Name) 清单=$($want.Substring(0,16)) 实际=$($got.Substring(0,16))"
    }
    Check '交付包校验值自洽' $ok $d
} else { Check '交付包校验值自洽' $false '没有交付包' }

Write-Host ""
if ($bad -eq 0) { Write-Host "结论：可以出包（$bad 项失败）" -ForegroundColor Green; exit 0 }
else { Write-Host "结论：不要出包（$bad 项失败）" -ForegroundColor Red; exit 1 }
