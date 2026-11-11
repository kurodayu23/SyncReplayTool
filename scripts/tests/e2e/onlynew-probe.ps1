# onlynew-probe.ps1 - the "only sync new files" toggle, two rounds.
# Promise under test: a file that already exists on the receiver is NOT
# overwritten, even when the local copy changed.
# ASCII only.
param(
  [int]$Port = 2265,
  [string]$Container = 'srt-onlynew-recv'
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\scale'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("onlynew-$stamp.log")
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
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination (Join-Path $appDir 'srtprobe-on.exe') -Force

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

$data = Join-Path $work 'onlynew-data'
New-Item -ItemType Directory -Force -Path $data | Out-Null
[System.IO.File]::WriteAllText((Join-Path $data 'a.spe'), 'A-ORIGINAL')
[System.IO.File]::WriteAllText((Join-Path $data 'b.spe'), 'B-ORIGINAL')

$cfgDir = Join-Path $BuildRoot ('work\probe\cfg\onlynew-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

function Run-Round([string]$tag, [bool]$onlyNew) {
  $probeOut = Join-Path $logs ("onlynew-$tag-$stamp.txt")
  $probeErr = Join-Path $logs ("onlynew-$tag-$stamp.err")
  $a = @('--root', $data, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
          '--key', $key, '--remote', '/home/srtpw/srt-in/onlynew',
          '--rules', 'spe', '--sync', '--timeout', '120')
  if ($onlyNew) { $a += '--only-new' }
  Say ("=== round $tag (only-new = {0}) ===" -f $onlyNew)
  $p = Start-Process -FilePath (Join-Path $appDir 'srtprobe-on.exe') -ArgumentList (($a | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ') -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $probeOut -RedirectStandardError $probeErr
  $null = $p.Handle
  $null = $p.WaitForExit(300000)
  Get-Content $probeOut -Encoding UTF8 -ErrorAction SilentlyContinue |
    Where-Object { $_ -notmatch '^ROW ' -and $_ -notmatch 'states=\[' } | ForEach-Object { Say ('  ' + $_) }
}
function Remote-Content([string]$name) {
  $r = DQ @('exec', $Container, 'sh', '-lc', "cat /home/$User/srt-in/onlynew/$name 2>/dev/null; echo")
  return $r.Trim()
}

Run-Round '1' $false
Say ("remote a.spe = '{0}'" -f (Remote-Content 'a.spe'))
Say ("remote b.spe = '{0}'" -f (Remote-Content 'b.spe'))

Say '=== change a.spe (same size, different content) and add c.spe ==='
[System.IO.File]::WriteAllText((Join-Path $data 'a.spe'), 'A-MODIFIED')
[System.IO.File]::WriteAllText((Join-Path $data 'c.spe'), 'C-NEW')
Start-Sleep -Seconds 2

Run-Round '2' $true
Say ("remote a.spe = '{0}'   (local = 'A-MODIFIED')" -f (Remote-Content 'a.spe'))
Say ("remote b.spe = '{0}'" -f (Remote-Content 'b.spe'))
Say ("remote c.spe = '{0}'   (local = 'C-NEW')" -f (Remote-Content 'c.spe'))

$la = (Remote-Content 'a.spe')
$lc = (Remote-Content 'c.spe')
if ($la -eq 'A-ORIGINAL') { Say 'VERDICT a.spe = NOT overwritten (matches the promise)' }
elseif ($la -eq 'A-MODIFIED') { Say 'VERDICT a.spe = WAS overwritten (breaks the promise)' }
else { Say ("VERDICT a.spe = unexpected '{0}'" -f $la) }
if ($lc -eq 'C-NEW') { Say 'VERDICT c.spe = transferred (correct)' } else { Say ("VERDICT c.spe = '{0}' (expected C-NEW)" -f $lc) }

DQ @('rm', '-f', $Container) | Out-Null
Say 'ONLYNEW DONE'
