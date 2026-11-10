# test-d1-probe.ps1 - D1 end-to-end with the headless probe.
# Scenario: no sync rules configured (= "sync every file"), but two files
# carry no suffix (README / Makefile).
# Question: do they reach the receiver, and what does the UI claim?
# ASCII only.
param(
  [int]$Port = 2253,
  [string]$Container = 'srt-d1-recv'
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\scale'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("d1probe-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

# the probe must sit next to ssh/rsync (the product puts its own dir on PATH)
$appDir = (Get-ChildItem (Join-Path $work 'win') -Directory | Select-Object -First 1).FullName
$probeSrc = Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe'
$probeExe = Join-Path $appDir 'srtprobe.exe'
Copy-Item -LiteralPath $probeSrc -Destination $probeExe -Force
Say ("probe = {0}" -f $probeExe)

# ---------------- receiver ----------------
$User = 'srtpw'
try { docker rm -f $Container 2>&1 | Out-Null } catch { }
docker run -d --name $Container -p "${Port}:22" kylin-v11-sshd:latest 2>&1 | Out-Null
Start-Sleep -Seconds 6
docker exec $Container sh -c "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in" 2>&1 | Out-Null
Start-Sleep -Seconds 3

# ---------------- key ----------------
$keydir = Join-Path $work 'keys'
$key = Join-Path $keydir 'id_ed25519'
if (-not (Test-Path $key)) { throw 'key missing - run scale.ps1 once first' }
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
docker exec $Container sh -c "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh" 2>&1 | Out-Null
Say 'receiver + key ready'

# ---------------- source data ----------------
# distinct mtimes -> deterministic row order (scanner sorts by mtime desc)
$data = Join-Path $work 'd1data'
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
$names = @('a.spe','b.spe','notes.txt','README','Makefile')
$base = Get-Date
for ($i = 0; $i -lt $names.Count; $i++) {
  $p = Join-Path $data $names[$i]
  [System.IO.File]::WriteAllBytes($p, [System.Text.Encoding]::UTF8.GetBytes("data-$($names[$i])"))
  [System.IO.File]::SetLastWriteTime($p, $base.AddMinutes(-$i))
}
Say ("source files (row order) = {0}" -f ($names -join ','))

# ---------------- run the probe ----------------
$cfgDir = Join-Path $BuildRoot ('work\probe\cfg\d1-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

$probeOut = Join-Path $logs ("d1probe-$stamp.txt")
$args = @(
  '--root', $data,
  '--host', '127.0.0.1',
  '--port', "$Port",
  '--user', $User,
  '--key', $key,
  '--remote', '/home/srtpw/srt-in/d1',
  '--sync',
  '--dump-rows',
  '--timeout', '120'
)
Say ('running: srtprobe.exe ' + ($args -join ' '))
$p = Start-Process -FilePath $probeExe -ArgumentList $args -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $probeOut -RedirectStandardError (Join-Path $logs ("d1probe-$stamp.err"))
$p.WaitForExit(240000) | Out-Null
Say ("probe exit = {0}" -f $p.ExitCode)
Get-Content $probeOut -Encoding UTF8 | ForEach-Object { Say ("  " + $_) }

# ---------------- what actually arrived ----------------
Say '=== what is on the receiver ==='
$rl = docker exec $Container sh -lc "cd /home/$User/srt-in/d1 2>/dev/null && find . -type f | sort" 2>&1
$remote = @()
foreach ($l in $rl) { if ($l -match '^\./(.+)$') { $remote += $matches[1] } }
Say ("remote files = {0}" -f ($remote -join ', '))
$missing = @()
foreach ($l in $names) { if ($remote -notcontains $l) { $missing += $l } }
Say ("MISSING on receiver = {0}" -f ($(if ($missing.Count) { $missing -join ',' } else { '(none)' })))

# ---- verdict: the excluded files must be ANNOUNCED, not silently dropped ----
$txt = Get-Content $probeOut -Encoding UTF8 -Raw
# The notice text is Chinese; build it from code points so this .ps1 stays
# pure ASCII (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI and would
# mangle the literal).
$noticeKey = -join ([char[]](0x4E0D, 0x5728, 0x540C, 0x6B65, 0x89C4, 0x5219, 0x91CC))
$announced = $txt.Contains($noticeKey)
$countOk = $txt.Contains('3 / 3')
# the probe dumps the rows twice (before and after the sync) - count the
# final "ok" rows only.
$okRows = ($txt -split "`r?`n" | Where-Object { $_ -match '^ROW \d+ ok$' }).Count
$scanN = 0
if ($txt -match 'SCAN files=(\d+)') { $scanN = [int]$matches[1] }
Say ("announced = {0}   count says 3/3 = {1}   scan files = {2}   ok rows = {3}" -f $announced, $countOk, $scanN, $okRows)
if ($missing.Count -eq 0) {
  Say 'VERDICT = all files transferred'
} elseif ($announced -and $countOk -and $scanN -eq 3 -and $okRows -eq 3) {
  Say 'VERDICT = FIXED: the excluded files are announced, not counted, not marked done'
} else {
  Say ('VERDICT = STILL SILENT LOSS: ' + ($missing -join ','))
}

try { docker rm -f $Container 2>&1 | Out-Null } catch { }
Say 'D1-PROBE DONE'
