# lock-probe.ps1 - what happens to a source file WHILE rsync is transferring it?
# Questions:
#   1. can the customer's own program still WRITE that file? (sharing violation?)
#   2. can it be renamed / deleted?
#   3. can a file that is queued but not yet transferred be written?
# ASCII only.
param(
  [int]$Port = 2262,
  [string]$Container = 'srt-lock-recv',
  [int]$MB = 200,
  [int]$BwMBps = 2,
  [int]$ProbeAtSec = 6
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\scale'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("lock-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}
function DQ([string[]]$argv) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $o = & docker @argv 2>&1
  $ErrorActionPreference = $old
  return (($o | Out-String).Trim())
}
function Try-Step([string]$label, [scriptblock]$sb) {
  try {
    & $sb | Out-Null
    Say ("  {0} -> OK" -f $label)
    return $true
  } catch {
    $m = $_.Exception.Message
    if ($m.Length -gt 110) { $m = $m.Substring(0, 110) }
    Say ("  {0} -> BLOCKED: {1}" -f $label, $m)
    return $false
  }
}

$appDir = (Get-ChildItem (Join-Path $work 'win') -Directory | Select-Object -First 1).FullName
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination (Join-Path $appDir 'srtprobe-lk.exe') -Force

$User = 'srtpw'
DQ @('rm', '-f', $Container) | Out-Null
DQ @('run', '-d', '--name', $Container, '-p', "${Port}:22", 'kylin-v11-sshd:latest') | Out-Null
Start-Sleep -Seconds 6
DQ @('exec', $Container, 'sh', '-c', "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in") | Out-Null
Start-Sleep -Seconds 3
$key = Join-Path $work 'keys\id_ed25519'
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
DQ @('exec', $Container, 'sh', '-lc', "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh") | Out-Null
Say 'receiver + key ready'

$data = Join-Path $work 'lock-data'
New-Item -ItemType Directory -Force -Path $data | Out-Null
$big = Join-Path $data 'aaa-big.spe'
$queued = Join-Path $data 'zzz-queued.spe'
$rng = New-Object System.Random 4242
$fs = [System.IO.File]::Create($big)
$buf = New-Object byte[] (1MB)
for ($i = 0; $i -lt $MB; $i++) { $rng.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }
$fs.Close()
[System.IO.File]::WriteAllText($queued, "queued file`n")
Say ("aaa-big.spe = {0} MB random, zzz-queued.spe = 1 small file" -f $MB)

$cfgDir = Join-Path $BuildRoot ('work\probe\cfg\lock-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
[System.IO.File]::WriteAllText((Join-Path $cfgDir 'SyncReplayTool.ini'),
  "[sync]`r`nbandwidthLimitMBps=$BwMBps`r`n", (New-Object System.Text.UTF8Encoding($false)))
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

$probeOut = Join-Path $logs ("lock-$stamp.txt")
$probeErr = Join-Path $logs ("lock-$stamp.err")
$pargs = @(
  '--root', $data, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
  '--key', $key, '--remote', '/home/srtpw/srt-in/lock',
  '--rules', 'spe', '--sync', '--timeout', '400'
)
Say ('starting probe (bwlimit = {0} MB/s)' -f $BwMBps)
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $appDir 'srtprobe-lk.exe') -ArgumentList (($pargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ') -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $probeOut -RedirectStandardError $probeErr
$null = $p.Handle

$done = $false
while (-not $p.HasExited) {
  Start-Sleep -Milliseconds 400
  try { $p.Refresh() } catch { }
  if (-not $done -and $sw.Elapsed.TotalSeconds -ge $ProbeAtSec) {
    $done = $true
    Say ("=== probing the source files at t={0:N1}s (transfer in flight) ===" -f $sw.Elapsed.TotalSeconds)
    Say '  --- file currently being transferred (aaa-big.spe) ---'
    Try-Step 'open for WRITE (what the customer data program does)' { $fs3 = [System.IO.File]::Open($big, 'Open', 'Write'); $fs3.Close() } | Out-Null
    Try-Step 'open for READ' { $fs3 = [System.IO.File]::Open($big, 'Open', 'Read'); $fs3.Close() } | Out-Null
    Try-Step 'rename' { Rename-Item -LiteralPath $big -NewName 'renamed.spe' -ErrorAction Stop } | Out-Null
    Try-Step 'delete' { Remove-Item -LiteralPath $big -Force -ErrorAction Stop } | Out-Null
    Say '  --- file still queued, not yet transferred (zzz-queued.spe) ---'
    Try-Step 'open for WRITE' { $fs3 = [System.IO.File]::Open($queued, 'Open', 'Write'); $fs3.Close() } | Out-Null
    Try-Step 'open for WRITE + change content' { $fs3 = [System.IO.File]::Open($queued, 'Open', 'Write'); $fs3.Write([byte[]]@(88,89,90), 0, 3); $fs3.Close() } | Out-Null
    Say '--- rsync processes still alive ---'
    Get-CimInstance Win32_Process -Filter "Name='rsync.exe'" | ForEach-Object { Say ('  rsync pid=' + $_.ProcessId) }
  }
}
$null = $p.WaitForExit(900000)
$sw.Stop()
$rc = -1
try { $rc = $p.ExitCode } catch { }
Say ("probe exit={0} after {1:N1}s" -f $rc, $sw.Elapsed.TotalSeconds)
Get-Content $probeOut -Encoding UTF8 -ErrorAction SilentlyContinue |
  Where-Object { $_ -notmatch '^ROW ' -and $_ -notmatch 'states=\[' } | ForEach-Object { Say ('  ' + $_) }

$rf = DQ @('exec', $Container, 'sh', '-lc', "cd /home/$User/srt-in/lock 2>/dev/null && ls -la | tail -n +2")
Say '--- remote dir ---'
Say $rf
DQ @('rm', '-f', $Container) | Out-Null
Say 'LOCK DONE'
