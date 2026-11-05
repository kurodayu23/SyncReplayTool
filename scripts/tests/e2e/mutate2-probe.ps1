# mutate2-probe.ps1 - modify a file that is still QUEUED (not yet transferred).
# The product says "the receiver got the pre-change content" for any file whose
# size/mtime changed during the round. Is that statement true for a queued file?
# ASCII only.
param(
  [int]$Port = 2264,
  [string]$Container = 'srt-mut2-recv',
  [int]$MB = 120,
  [int]$BwMBps = 2,
  [int]$MutateAtSec = 6
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\scale'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("mut2-$stamp.log")
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

$appDir = (Get-ChildItem (Join-Path $work 'win') -Directory | Select-Object -First 1).FullName
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination (Join-Path $appDir 'srtprobe-m2.exe') -Force

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

$data = Join-Path $work 'mut2-data'
New-Item -ItemType Directory -Force -Path $data | Out-Null
$big = Join-Path $data 'aaa-big.spe'
$queued = Join-Path $data 'zzz-queued.spe'
$rng = New-Object System.Random 4242
$fs = [System.IO.File]::Create($big)
$buf = New-Object byte[] (1MB)
for ($i = 0; $i -lt $MB; $i++) { $rng.NextBytes($buf); $fs.Write($buf, 0, $buf.Length) }
$fs.Close()
$origText = 'ORIGINAL-CONTENT-0001'
$newText = 'NEWCONTENT-CHANGED-9999-DIFFERENT-SIZE'
[System.IO.File]::WriteAllText($queued, $origText)
Say ("aaa-big.spe = {0} MB random; zzz-queued.spe = '{1}'" -f $MB, $origText)

$cfgDir = Join-Path $BuildRoot ('work\probe\cfg\mut2-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
[System.IO.File]::WriteAllText((Join-Path $cfgDir 'SyncReplayTool.ini'),
  "[sync]`r`nbandwidthLimitMBps=$BwMBps`r`n", (New-Object System.Text.UTF8Encoding($false)))
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

$probeOut = Join-Path $logs ("mut2-$stamp.txt")
$probeErr = Join-Path $logs ("mut2-$stamp.err")
$pargs = @(
  '--root', $data, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
  '--key', $key, '--remote', '/home/srtpw/srt-in/mut2',
  '--rules', 'spe', '--sync', '--timeout', '400'
)
Say ('starting probe (bwlimit = {0} MB/s)' -f $BwMBps)
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $appDir 'srtprobe-m2.exe') -ArgumentList (($pargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ') -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $probeOut -RedirectStandardError $probeErr
$null = $p.Handle

$done = $false
while (-not $p.HasExited) {
  Start-Sleep -Milliseconds 400
  try { $p.Refresh() } catch { }
  if (-not $done -and $sw.Elapsed.TotalSeconds -ge $MutateAtSec) {
    $done = $true
    Say ("  changing zzz-queued.spe while it is still QUEUED (t={0:N1}s)" -f $sw.Elapsed.TotalSeconds)
    [System.IO.File]::WriteAllText($queued, $newText)
    Say ("  local content now = '{0}'" -f $newText)
  }
}
$null = $p.WaitForExit(900000)
$sw.Stop()
$rc = -1
try { $rc = $p.ExitCode } catch { }
Say ("probe exit={0} after {1:N1}s" -f $rc, $sw.Elapsed.TotalSeconds)
Get-Content $probeOut -Encoding UTF8 -ErrorAction SilentlyContinue |
  Where-Object { $_ -notmatch '^ROW ' -and $_ -notmatch 'states=\[' } | ForEach-Object { Say ('  ' + $_) }

Say '=== what is actually on the receiver? ==='
$remoteTxt = DQ @('exec', $Container, 'sh', '-lc', "cd /home/$User/srt-in/mut2 2>/dev/null && cat zzz-queued.spe 2>/dev/null; echo")
Say ("remote zzz-queued.spe = '{0}'" -f $remoteTxt.Trim())
if ($remoteTxt.Trim() -eq $newText) {
  Say 'VERDICT = the receiver got the NEW content => the product''s claim "the receiver got the pre-change content" is FALSE'
} elseif ($remoteTxt.Trim() -eq $origText) {
  Say 'VERDICT = the receiver got the OLD content => the product''s claim is TRUE'
} else {
  Say ('VERDICT = unexpected content: ' + $remoteTxt)
}
$rcount = DQ @('exec', $Container, 'sh', '-lc', "cd /home/$User/srt-in/mut2 2>/dev/null && ls -l | tail -n +2")
Say '--- remote dir ---'
Say $rcount
DQ @('rm', '-f', $Container) | Out-Null
Say 'MUT2 DONE'
