# cancel-probe.ps1 - user clicks "cancel" in the middle of a transfer.
#
# What it checks:
#   1  the product really stops (no rsync / ssh left running afterwards)
#   2  no half-written file lands under the real name on the receiver
#   3  the UI says "canceled", not "done"
#
# The transfer is slowed down with the product's own bandwidth limit
# (config key sync/bandwidthLimitMBps) so the cancel lands mid-transfer
# instead of after it.
#
# ASCII only.
param(
  [int]$Port = 2259,
  [string]$Container = 'srt-cancel-recv',
  [int]$FileMB = 20,
  [int]$CancelAfterMs = 2000
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\cancel'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("cancel-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

# ---------------- probe binary ----------------
$appDir = (Get-ChildItem (Join-Path $BuildRoot 'work\scale\win') -Directory | Select-Object -First 1).FullName
$probeExe = Join-Path $appDir 'srtprobe.exe'
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination $probeExe -Force

# ---------------- receiver ----------------
$User = 'srtpw'
try { docker rm -f $Container 2>&1 | Out-Null } catch { }
docker run -d --name $Container -p "${Port}:22" kylin-v11-sshd:latest 2>&1 | Out-Null
Start-Sleep -Seconds 6
docker exec $Container sh -c "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in" 2>&1 | Out-Null
Start-Sleep -Seconds 3

$keydir = Join-Path $BuildRoot 'work\scale\keys'
$key = Join-Path $keydir 'id_ed25519'
if (-not (Test-Path $key)) { throw 'key missing - run scale-probe.ps1 once first' }
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
docker exec $Container sh -c "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh" 2>&1 | Out-Null
Say 'receiver + key ready'

# ---------------- source: one big random file ----------------
$data = Join-Path $work 'data'
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
$big = Join-Path $data 'big.spe'
$buf = New-Object byte[] (1MB)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
$fs = [System.IO.File]::Create($big)
for ($i = 0; $i -lt $FileMB; $i++) {
  $rng.GetBytes($buf)
  $fs.Write($buf, 0, $buf.Length)
}
$fs.Close()
$rng.Dispose()
Say ("source = {0} ({1} MB of random bytes)" -f $big, $FileMB)

# ---------------- isolated config with a bandwidth limit ----------------
$cfgDir = Join-Path $work ('cfg-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$ini = Join-Path $cfgDir 'SyncReplayTool.ini'
$iniText = "[sync]`r`nbandwidthLimitMBps=1`r`n"
[System.IO.File]::WriteAllText($ini, $iniText, [System.Text.Encoding]::GetEncoding(936))
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

# ---------------- processes before ----------------
function Get-ToolProcs {
  $p = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -in @('rsync.exe','ssh.exe','ssh-keygen.exe') }
  return @($p | ForEach-Object { "$($_.Name):$($_.ProcessId)" })
}
$before = Get-ToolProcs
Say ("tool processes before = {0}" -f ($(if ($before.Count) { $before -join ',' } else { '(none)' })))

# ---------------- run + cancel ----------------
$po = Join-Path $logs ("cancel-$stamp.txt")
$pe = Join-Path $logs ("cancel-$stamp.err")
$a = @(
  '--root', $data,
  '--host', '127.0.0.1',
  '--port', "$Port",
  '--user', $User,
  '--key', $key,
  '--remote', "/home/$User/srt-in/cancel",
  '--sync',
  '--timeout', '90',
  '--cancel-after', "$CancelAfterMs"
)
Say 'running probe (it will cancel itself after ' + $CancelAfterMs + ' ms)'
$sw = [Diagnostics.Stopwatch]::StartNew()
$pp = Start-Process -FilePath $probeExe -ArgumentList $a -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po -RedirectStandardError $pe
$pp.WaitForExit(180000) | Out-Null
$sw.Stop()
Say ("probe exit = {0} after {1:N1}s" -f $pp.ExitCode, $sw.Elapsed.TotalSeconds)
Get-Content $po -Encoding UTF8 | ForEach-Object { Say ("  " + $_) }

# ---------------- processes after ----------------
Start-Sleep -Seconds 3
$after = Get-ToolProcs
Say ("tool processes after = {0}" -f ($(if ($after.Count) { $after -join ',' } else { '(none)' })))
$leaked = @($after | Where-Object { $before -notcontains $_ })
Say ("VERDICT leftover tool processes = {0}" -f ($(if ($leaked.Count) { $leaked -join ',' } else { '(none)' })))

# ---------------- what is on the receiver ----------------
# The real path and the partial dir must be told apart: rsync's --partial-dir
# keeps the half file inside .srt-partial/ under its ORIGINAL name, so
# `find -name big.spe` matches that one too and the verdict would be wrong.
$realPath = docker exec $Container sh -lc "find /home/$User/srt-in/cancel -maxdepth 1 -name 'big.spe' | wc -l" 2>&1
$partList = docker exec $Container sh -lc "find /home/$User -path '*srt-partial*' -exec ls -l {} + 2>/dev/null | wc -l" 2>&1
$partNames = docker exec $Container sh -lc "find /home/$User -path '*srt-partial*' -printf '%p %s\n' 2>/dev/null | LC_ALL=C sort" 2>&1
$allNames = docker exec $Container sh -lc "find /home/$User/srt-in -printf '%p %s\n' 2>/dev/null | LC_ALL=C sort" 2>&1
Say ("receiver: file at the REAL path = {0}" -f (($realPath | Out-String).Trim()))
Say ("receiver entries under .srt-partial = {0}" -f (($partList | Out-String).Trim()))
foreach ($l in $partNames) { Say ("  partial: " + $l) }
foreach ($l in $allNames) { Say ("  tree   : " + $l) }
Say ("VERDICT half file landed under the real name = {0}" -f ([int](($realPath | Out-String).Trim()) -gt 0))

# ---------------- round 2: the next round must repair it ----------------
# The manual promises: the half file is not under the real name, and the next
# round retransmits it (no manual cleanup needed). If the next round skipped
# it, the receiver would keep a TRUNCATED file under the real name - the
# customer would see a complete-looking file with the wrong content.
Say '=========== ROUND 2: next round, no cancel ==========='
$po2 = Join-Path $logs ("cancel2-$stamp.txt")
$pe2 = Join-Path $logs ("cancel2-$stamp.err")
$a2 = @(
  '--root', $data,
  '--host', '127.0.0.1',
  '--port', "$Port",
  '--user', $User,
  '--key', $key,
  '--remote', "/home/$User/srt-in/cancel",
  '--sync',
  '--timeout', '180'
)
$sw2 = [Diagnostics.Stopwatch]::StartNew()
$pp2 = Start-Process -FilePath $probeExe -ArgumentList $a2 -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po2 -RedirectStandardError $pe2
$pp2.WaitForExit(300000) | Out-Null
$sw2.Stop()
Say ("probe exit = {0} after {1:N1}s" -f $pp2.ExitCode, $sw2.Elapsed.TotalSeconds)
Get-Content $po2 -Encoding UTF8 | ForEach-Object { Say ("  " + $_) }

$sz = docker exec $Container sh -lc "find /home/$User/srt-in/cancel -maxdepth 1 -name 'big.spe' -printf '%s\n' 2>/dev/null" 2>&1
$remoteSize = [int](($sz | Select-Object -First 1 | Out-String).Trim())
Say ("remote big.spe size = {0} bytes (local = {1} bytes)" -f $remoteSize, (Get-Item $big).Length)
Say ("VERDICT remote file is complete = {0}" -f ($remoteSize -eq (Get-Item $big).Length))
$lsha = (Get-FileHash -LiteralPath $big -Algorithm SHA256).Hash.ToLower()
$rsha = (docker exec $Container sh -lc "sha256sum /home/$User/srt-in/cancel/big.spe 2>/dev/null" 2>&1 | Out-String).Trim()
Say ("local  sha256 = {0}" -f $lsha)
Say ("remote sha256 = {0}" -f $rsha)
Say ("VERDICT content identical = {0}" -f ($rsha.ToLower().StartsWith($lsha)))

try { docker rm -f $Container 2>&1 | Out-Null } catch { }
Say 'CANCEL-PROBE DONE'
