# askpass-vanish-probe.ps1 - what does the customer see when the askpass
# script disappears AFTER the authentication succeeded?
#
# The product drives password authentication through a short lived helper
# script (srt_askpass_<pid>_XXXXXX.bat) that hands the password to ssh.
# One round uses that script for SEVERAL ssh calls: create the remote
# directories, run rsync, verify what arrived.
#
# So the window "authenticated, but the script is gone" is real: an
# antivirus, a cleanup tool, a second program, or a user can delete the
# file between the first call and the last one.
#
# This bench makes that window wide and deterministic:
#   - the transfer is throttled to 1 MB/s, so the round lasts ~30 s
#   - a watcher deletes every srt_askpass_* / srt_pass_* file in the
#     candidate directories 3 s after rsync has started (i.e. after the
#     first successful authentication)
#   - whatever the product does next is recorded verbatim
#
# What is asserted:
#   1  the data itself still arrives (rsync authenticated before the file
#      was deleted)
#   2  the product must not claim a plain success
#   3  if the round fails, the message must name the real cause (the
#      helper script is gone) and must NOT tell the user to fix the
#      account or the password - the password was right
#   4  no plaintext password file may survive the round
#
# ASCII only (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI and a
# Chinese comment would eat the next line).
param(
  [int]$Port = 2274,
  [string]$Container = 'srt-askpass-recv',
  [string]$User = 'srtpw',
  [string]$Pass = 'SrtTest2026x',
  [int]$BigMB = 30,
  [int]$KillAfterSec = 3
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\askpass'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("askpass-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

$wDone   = -join ([char[]](0x5DF2, 0x5B8C, 0x6210))   # done
$wFailed = -join ([char[]](0x5931, 0x8D25))            # failed
# "account or password" - the wrong thing to tell the user here
$wCred   = -join ([char[]](0x8D26, 0x53F7, 0x5BC6, 0x7801))
$wPasswd = -join ([char[]](0x5BC6, 0x7801))
# helper script / askpass
$wAsk    = -join ([char[]](0x5E2E, 0x52A9, 0x811A, 0x672C))
$wScript = -join ([char[]](0x811A, 0x672C))

# ---------------- probe binary ----------------
$appDir = (Get-ChildItem (Join-Path $BuildRoot 'work\scale\win') -Directory | Select-Object -First 1).FullName
$probeExe = Join-Path $appDir 'srtprobe.exe'
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination $probeExe -Force

# ---------------- receiver (password authentication) ----------------
try { docker rm -f $Container 2>&1 | Out-Null } catch { }
docker run -d --name $Container -p "${Port}:22" kylin-v11-sshd:latest 2>&1 | Out-Null
Start-Sleep -Seconds 6
docker exec $Container sh -c "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in" 2>&1 | Out-Null
Start-Sleep -Seconds 3
# Set the password here instead of trusting the image: the image's srtpw
# password is not the same on every build, and a wrong password would look
# exactly like the defect this bench is about.
docker exec -u root $Container sh -lc "echo '${User}:${Pass}' | chpasswd" 2>&1 | Out-Null
Say 'receiver ready (password authentication)'

$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

# ---------------- source: one throttled file ----------------
$data = Join-Path $work ("data-" + $stamp)
New-Item -ItemType Directory -Force -Path $data | Out-Null
$big = Join-Path $data 'big.dat'
$buf = New-Object byte[] (1MB)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$fs = [System.IO.File]::Create($big)
for ($i = 0; $i -lt $BigMB; $i++) { $rng.GetBytes($buf); $fs.Write($buf, 0, $buf.Length) }
$fs.Close()
$rng.Dispose()
Say ("source = {0} MB, throttled to 1 MB/s" -f $BigMB)

# ---------------- config: rule + bandwidth limit ----------------
$cfg = Join-Path $work ("cfg-" + $stamp)
New-Item -ItemType Directory -Force -Path $cfg | Out-Null
$ini = "[sync]`r`nbandwidthLimitMBps=1`r`n[syncRules]`r`nsize=1`r`n1\suffix=dat`r`n1\remoteSubDir=pwv`r`n"
[System.IO.File]::WriteAllText((Join-Path $cfg 'SyncReplayTool.ini'), $ini, [System.Text.Encoding]::ASCII)
$env:SRT_CONFIG_DIR = $cfg

# ---------------- the candidate directories the askpass may live in ----
$candDirs = @()
foreach ($d in @($env:LOCALAPPDATA, $env:TEMP, $appDir, $env:USERPROFILE)) {
  if ($d -and (Test-Path $d) -and -not ($candDirs -contains $d)) { $candDirs += $d }
}
Say ("askpass candidate directories = {0}" -f ($candDirs -join '; '))

# ---------------- run ----------------
$po = Join-Path $logs ("askpass-$stamp.txt")
$pe = Join-Path $logs ("askpass-$stamp.err")
$a = @('--root', $data, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
       '--password', $Pass, '--remote', "/home/$User/srt-in", '--sync',
       '--timeout', '600')
$p = Start-Process -FilePath $probeExe -ArgumentList $a -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po -RedirectStandardError $pe
# Touching .Handle caches the process handle. Without it the Process object
# throws away the handle and ExitCode comes back empty.
$null = $p.Handle
Say 'probe started'

# Read the probe log while it is being written. Get-Content cannot open the
# file while the child still holds it, so it only sees the content after the
# round ended - that is 30 s too late for this bench.
function Read-Live([string]$path) {
  try {
    $fs = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
      $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
      try { return $sr.ReadToEnd() } finally { $sr.Dispose() }
    } finally { $fs.Dispose() }
  } catch { return '' }
}

# ---------------- the watcher ----------------
# Wait until rsync has started (the log line names the remote user), then
# delete the helper script 3 s later - long after the first successful
# authentication, long before the last one.
$deleted = @()
$killed = $false
$deadline = (Get-Date).AddSeconds(180)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 200
  $cur = ''
  if (Test-Path $po) { $cur = Read-Live $po }
  if ($cur -and $cur.Contains('srtpw@') -and -not $killed) {
    Say 'rsync has started (first authentication done) - waiting before deleting the helper'
    Start-Sleep -Seconds $KillAfterSec
    foreach ($d in $candDirs) {
      foreach ($f in @(Get-ChildItem -LiteralPath $d -File -Force -ErrorAction SilentlyContinue |
                       Where-Object { $_.Name -like 'srt_askpass_*' -or $_.Name -like 'srt_pass_*' })) {
        $deleted += ("{0} ({1} bytes)" -f $f.FullName, $f.Length)
        Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue
      }
    }
    $killed = $true
    Say ("deleted {0} helper file(s)" -f $deleted.Count)
    foreach ($d in $deleted) { Say ("  deleted: " + $d) }
  }
}
$p.WaitForExit(600000) | Out-Null
$p.Refresh()
Say ("probe exit = {0}" -f $p.ExitCode)
$txt = Get-Content $po -Encoding UTF8 -Raw
foreach ($l in ($txt -split "`r?`n")) { if ($l -match '^(SCAN|SYNC|ROW|LOG)') { Say ("  " + $l) } }

# ---------------- what arrived ----------------
$got = docker exec $Container sh -lc "find /home/$User/srt-in/pwv -maxdepth 1 -type f -printf '%f %s\n' 2>/dev/null | LC_ALL=C sort" 2>&1
Say ("receiver = {0}" -f (($got | Out-String).Trim()))
$arrived = ($got | Out-String) -match 'big\.dat\s+1048576'

# ---------------- leftover secret files ----------------
$left = @()
foreach ($d in $candDirs) {
  foreach ($f in @(Get-ChildItem -LiteralPath $d -File -Force -ErrorAction SilentlyContinue |
                   Where-Object { $_.Name -like 'srt_askpass_*' -or $_.Name -like 'srt_pass_*' })) {
    $left += ("{0} ({1} bytes)" -f $f.FullName, $f.Length)
  }
}
Say ("leftover helper files after the round = {0}" -f $left.Count)
foreach ($d in $left) { Say ("  leftover: " + $d) }

# ---------------- verdict ----------------
$status = ''
if ($txt -match 'status="([^"]*)"') { $status = $matches[1] }
$saidDone = $status.Contains($wDone)
$saidFail = $status.Contains($wFailed) -or ($txt -match 'SYNC ok=0')
$mentionsCred = $txt.Contains($wCred) -or $txt.Contains($wPasswd)
$mentionsHelper = $txt.Contains($wAsk) -or $txt.Contains($wScript)
Say ("VERDICT the helper script was deleted mid round = {0}" -f ($deleted.Count -ge 1))
Say ("VERDICT the data still arrived = {0}" -f $arrived)
Say ("VERDICT the product reported a plain done = {0}" -f $saidDone)
Say ("VERDICT the product reported a failure = {0}" -f $saidFail)
Say ("VERDICT the log blames the account or the password = {0}" -f $mentionsCred)
Say ("VERDICT the log names the helper script = {0}" -f $mentionsHelper)
Say ("VERDICT no plaintext helper file survived = {0}" -f ($left.Count -eq 0))

try { docker rm -f $Container 2>&1 | Out-Null } catch { }
Say 'ASKPASS-PROBE DONE'
