# netcut-probe.ps1 - pull the network cable mid-sync, then restore it.
#   * the round must END (with an honest failure), not loop forever
#   * no orphan rsync/ssh processes may pile up while it is hanging
#   * after the network is back, the next round must work
# ASCII only.
param(
  [int]$Files = 30000,
  [int]$Port = 2257,
  [string]$Container = 'srt-netcut-recv',
  [int]$WatchdogMs = 90000
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\scale'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("netcut-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}
# docker wrapper: a native command writing to stderr must NOT kill the script.
function DQ([string[]]$argv) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $o = & docker @argv 2>&1
  $ErrorActionPreference = $old
  return (($o | Out-String).Trim())
}
function Count-Children {
  $n = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^(rsync|ssh)\.exe$' }).Count
  return $n
}
function Start-Probe([string[]]$pargs, [string]$oFile, [string]$eFile) {
  $quoted = ($pargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
  $p = Start-Process -FilePath (Join-Path $appDir 'srtprobe.exe') -ArgumentList $quoted `
        -WorkingDirectory $appDir -PassThru -NoNewWindow `
        -RedirectStandardOutput $oFile -RedirectStandardError $eFile
  # Touch .Handle once so the process handle is cached: without it .ExitCode
  # is unreliable (Start-Process -PassThru does not keep the handle).
  $null = $p.Handle
  return $p
}

$appDir = (Get-ChildItem (Join-Path $work 'win') -Directory | Select-Object -First 1).FullName
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination (Join-Path $appDir 'srtprobe.exe') -Force

$User = 'srtpw'
DQ @('rm', '-f', $Container) | Out-Null
DQ @('run', '-d', '--name', $Container, '-p', "${Port}:22", 'kylin-v11-sshd:latest') | Out-Null
Start-Sleep -Seconds 6
DQ @('exec', $Container, 'sh', '-c', "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in") | Out-Null
Start-Sleep -Seconds 3
$keydir = Join-Path $work 'keys'
$key = Join-Path $keydir 'id_ed25519'
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
DQ @('exec', $Container, 'sh', '-lc', "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh") | Out-Null
Say 'receiver + key ready'

$data = Join-Path $work 'netcut-data'
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
& python (Join-Path $PSScriptRoot 'gen_scale_files.py') $data $Files 2>&1 | Out-Null
Say ("source files = {0}" -f $Files)

$cfgDir = Join-Path $BuildRoot ('work\probe\cfg\netcut-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH
$env:SRT_WATCHDOG_MS = "$WatchdogMs"

$pargs = @(
  '--root', $data, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
  '--key', $key, '--remote', '/home/srtpw/srt-in/netcut',
  '--sync', '--timeout', '300'
)
Say '=== round 1: cut the cable mid-sync ==='
$sw = [Diagnostics.Stopwatch]::StartNew()
$h = Start-Probe $pargs (Join-Path $logs ("netcut-$stamp.txt")) (Join-Path $logs ("netcut-$stamp.err"))
$p = $h

$paused = $false
$maxKids = 0
while (-not $p.HasExited) {
  Start-Sleep -Seconds 2
  try { $p.Refresh() } catch { }
  $kids = Count-Children
  if ($kids -gt $maxKids) { $maxKids = $kids }
  if (-not $paused -and $sw.Elapsed.TotalSeconds -gt 10) {
    Say ("  pulling the cable (docker pause) at t={0:N0}s, rsync/ssh procs={1}" -f $sw.Elapsed.TotalSeconds, $kids)
    DQ @('pause', $Container) | Out-Null
    $paused = $true
  }
  if ($sw.Elapsed.TotalSeconds -gt 240) { Say '  TIMEOUT - killing the probe'; try { $p.Kill() } catch { }; break }
}
$sw.Stop()
$rc1 = -1
try { $p.WaitForExit(5000) | Out-Null; $rc1 = $p.ExitCode } catch { }
Say ("round 1 finished: exit={0} after {1:N1}s, max concurrent rsync/ssh = {2}" -f $rc1, $sw.Elapsed.TotalSeconds, $maxKids)
Say ("  leftovers right after the round: {0}" -f (Count-Children))
Start-Sleep -Seconds 5
Say ("  leftovers 5s later: {0}" -f (Count-Children))
$txt = Get-Content (Join-Path $logs ("netcut-$stamp.txt")) -Encoding UTF8 -ErrorAction SilentlyContinue
$txt | Where-Object { $_ -and $_ -notmatch 'states=\[' } | Select-Object -Last 16 | ForEach-Object { Say ('  ' + $_) }

Say '=== restoring the network ==='
DQ @('unpause', $Container) | Out-Null
Start-Sleep -Seconds 3

Say '=== round 2: the next round must work ==='
$sw2 = [Diagnostics.Stopwatch]::StartNew()
$h2 = Start-Probe $pargs (Join-Path $logs ("netcut2-$stamp.txt")) (Join-Path $logs ("netcut2-$stamp.err"))
$p2 = $h2
$null = $p2.WaitForExit(600000)
$sw2.Stop()
$rc2 = -1
try { $rc2 = $p2.ExitCode } catch { }
Say ("round 2 finished: exit={0} after {1:N1}s" -f $rc2, $sw2.Elapsed.TotalSeconds)
$txt2 = Get-Content (Join-Path $logs ("netcut2-$stamp.txt")) -Encoding UTF8 -ErrorAction SilentlyContinue
$txt2 | Where-Object { $_ -and $_ -notmatch 'states=\[' } | Select-Object -Last 8 | ForEach-Object { Say ('  ' + $_) }

$rc = DQ @('exec', $Container, 'sh', '-lc', "cd /home/$User/srt-in/netcut 2>/dev/null && find . -type f | wc -l")
Say ("remote files now = {0} (source = {1})" -f $rc, $Files)
DQ @('rm', '-f', $Container) | Out-Null
Say 'NETCUT DONE'
