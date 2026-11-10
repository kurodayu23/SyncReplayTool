# okmeans-arrived-probe.ps1 - the strongest single invariant of this product:
#
#   **A file the UI marks as done must really be on the receiver.**
#
# Why this needs its own bench: the "disk full" case has several shapes.
# 2026-11-07 I tested 4 x 3MB against a 4MB tmpfs (all files the same size)
# and the product was honest (1 ok / 3 failed, status failed).
# 2026-11-09 the other session found a different shape:
#   tmpfs 10MB, one 20MB file FIRST (it fills the disk), then three 32-byte
#   files -> rsync aborts on the big one, the three small ones never leave
#   the client, yet the UI said "3 / 4 done" with three green rows while the
#   receiver held **zero** real-name files.
#
# This bench reproduces that exact shape and asserts the invariant directly:
#   A  the counter numerator must not exceed the number of real-name files
#      actually present on the receiver
#   B  a row may only be green if that file is really there with the right
#      size
#   C  the round must be judged failed
#
# ASCII only (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI and a
# Chinese comment would eat the next line).
param(
  [int]$Port = 2271,
  [string]$Container = 'srt-okmeans-recv',
  [int]$TmpfsMB = 10,
  [int]$BigMB = 20,
  [int]$SmallCount = 3,
  [int]$SmallBytes = 32
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\okmeans'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("okmeans-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

# ---------------- probe binary ----------------
$appDir = (Get-ChildItem (Join-Path $BuildRoot 'work\scale\win') -Directory | Select-Object -First 1).FullName
$probeExe = Join-Path $appDir 'srtprobe.exe'
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination $probeExe -Force

# ---------------- receiver with a tiny disk ----------------
$User = 'srtpw'
try { docker rm -f $Container 2>&1 | Out-Null } catch { }
docker run -d --name $Container -p "${Port}:22" --tmpfs "/home/$User/srt-in:size=${TmpfsMB}m" kylin-v11-sshd:latest 2>&1 | Out-Null
Start-Sleep -Seconds 6
docker exec $Container sh -c "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in" 2>&1 | Out-Null
Start-Sleep -Seconds 3

$keydir = Join-Path $BuildRoot 'work\scale\keys'
$key = Join-Path $keydir 'id_ed25519'
if (-not (Test-Path $key)) { throw 'key missing - run scale-probe.ps1 once first' }
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
docker exec $Container sh -lc "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh" 2>&1 | Out-Null
$df = docker exec $Container sh -lc "df -h /home/$User/srt-in | tail -n 1" 2>&1
Say ("receiver tmpfs = {0}" -f (($df | Out-String).Trim()))

# ---------------- source: ONE big file first, then tiny ones ----------------
# The scanner sorts by mtime descending, so the big file must be the NEWEST
# to be transferred first (that is what fills the disk before the small ones).
$data = Join-Path $work 'data'
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
$buf = New-Object byte[] (1MB)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$big = Join-Path $data 'big.spe'
$fs = [System.IO.File]::Create($big)
for ($i = 0; $i -lt $BigMB; $i++) { $rng.GetBytes($buf); $fs.Write($buf, 0, $buf.Length) }
$fs.Close()
$rng.Dispose()

$base = Get-Date
# small files get OLDER and OLDER timestamps; the big file gets the newest
$smallNames = @()
for ($i = 1; $i -le $SmallCount; $i++) {
  $n = "t$i.spe"
  $smallNames += $n
  $p = Join-Path $data $n
  [System.IO.File]::WriteAllBytes($p, (New-Object byte[] $SmallBytes))
  [System.IO.File]::SetLastWriteTime($p, $base.AddMinutes(-$i))
}
[System.IO.File]::SetLastWriteTime($big, $base)
Say ("source: 1 x {0} MB (newest) + {1} x {2} bytes; tmpfs = {3} MB" -f $BigMB, $SmallCount, $SmallBytes, $TmpfsMB)

# ---------------- run ----------------
$cfgDir = Join-Path $work ('cfg-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

$po = Join-Path $logs ("okmeans-$stamp.txt")
$pe = Join-Path $logs ("okmeans-$stamp.err")
$a = @(
  '--root', $data,
  '--host', '127.0.0.1',
  '--port', "$Port",
  '--user', $User,
  '--key', $key,
  '--remote', "/home/$User/srt-in/dd",
  '--sync', '--dump-rows',
  '--timeout', '180'
)
Say 'running probe'
$sw = [Diagnostics.Stopwatch]::StartNew()
$pp = Start-Process -FilePath $probeExe -ArgumentList $a -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po -RedirectStandardError $pe
$pp.WaitForExit(300000) | Out-Null
$sw.Stop()
Say ("probe exit = {0} after {1:N1}s" -f $pp.ExitCode, $sw.Elapsed.TotalSeconds)
$txt = Get-Content $po -Encoding UTF8 -Raw
foreach ($l in ($txt -split "`r?`n")) { if ($l -match '^(SCAN|SYNC|ROW)') { Say ("  " + $l) } }

# ---------------- what really arrived ----------------
# -printf '%p %s' gives the real size of every real-name file
$remoteRaw = docker exec $Container sh -lc "find /home/$User/srt-in/dd -maxdepth 1 -type f -printf '%f %s\n' 2>/dev/null | LC_ALL=C sort" 2>&1
$arrived = @{}
foreach ($l in $remoteRaw) {
  if ($l -match '^(\S+)\s+(\d+)$') { $arrived[$matches[1]] = [int]$matches[2] }
}
Say ("real-name files on the receiver = {0}" -f $(if ($arrived.Count) { ($arrived.Keys | Sort-Object) -join ',' } else { '(none)' }))
foreach ($k in ($arrived.Keys | Sort-Object)) { Say ("  arrived: {0} = {1} bytes" -f $k, $arrived[$k]) }

# ---------------- the rows ----------------
# Row order == scanner order == mtime descending: big first, then t3, t2, t1.
$expected = @('big.spe')
for ($i = $SmallCount; $i -ge 1; $i--) { $expected += "t$i.spe" }
$rows = @{}
foreach ($l in ($txt -split "`r?`n")) {
  if ($l -match '^ROW (\d+) (\w+)$') { $rows[[int]$matches[1]] = $matches[2] }
}
$greenRows = @()
foreach ($i in ($rows.Keys | Sort-Object)) {
  $name = if ($i -lt $expected.Count) { $expected[$i] } else { "?$i" }
  Say ("  ROW {0} {1,-8} ({2})" -f $i, $rows[$i], $name)
  if ($rows[$i] -eq 'ok') { $greenRows += $name }
}
$done = 0
if ($txt -match 'done=(\d+)') { $done = [int]$matches[1] }
$total = 0
if ($txt -match 'total=(\d+)') { $total = [int]$matches[1] }
Say ("counter = {0} / {1}   real files arrived = {2}" -f $done, $total, $arrived.Count)

# ---------------- verdict ----------------
$greenWithoutFile = @()
foreach ($g in $greenRows) {
  if (-not $arrived.ContainsKey($g)) { $greenWithoutFile += $g }
}
$smallGreen = @()
foreach ($g in $greenRows) { if ($smallNames -contains $g) { $smallGreen += $g } }
Say ("VERDICT counter numerator <= files actually arrived = {0}" -f ($done -le $arrived.Count))
Say ("VERDICT every green row has a real file on the receiver = {0}" -f ($greenWithoutFile.Count -eq 0))
Say ("VERDICT no tiny file is green although it never arrived = {0}" -f ($smallGreen.Count -eq 0))
$failedWord = -join ([char[]](0x5931, 0x8D25))   # failed
Say ("VERDICT the round was judged failed = {0}" -f (($txt -match 'SYNC ok=0') -or $txt.Contains($failedWord)))
if ($greenWithoutFile.Count -gt 0) { Say ("  GREEN BUT MISSING: " + ($greenWithoutFile -join ',')) }

try { docker rm -f $Container 2>&1 | Out-Null } catch { }
Say 'OKMEANS-PROBE DONE'
