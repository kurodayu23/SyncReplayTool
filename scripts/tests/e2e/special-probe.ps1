# special-probe.ps1 - hostile file names through the real sync path.
# ASCII only.
param(
  [int]$Port = 2259,
  [string]$Container = 'srt-special-recv'
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\scale'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("special-$stamp.log")
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
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination (Join-Path $appDir 'srtprobe-sp.exe') -Force

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

$data = Join-Path $work 'special-data'
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
$gen = & python (Join-Path $PSScriptRoot 'gen_special_files.py') $data 2>&1
Say ("generated: " + (($gen | Select-Object -First 1) | Out-String).Trim())
$manifest = Join-Path $work 'special-manifest.json'

$cfgDir = Join-Path $BuildRoot ('work\probe\cfg\special-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

$probeOut = Join-Path $logs ("special-$stamp.txt")
$probeErr = Join-Path $logs ("special-$stamp.err")
$pargs = @(
  '--root', $data, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
  '--key', $key, '--remote', '/home/srtpw/srt-in/special',
  '--rules', 'spe', '--sync', '--timeout', '300'
)
Say 'starting probe (rules = spe)'
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $appDir 'srtprobe-sp.exe') -ArgumentList (($pargs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ') -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $probeOut -RedirectStandardError $probeErr
$null = $p.Handle
$null = $p.WaitForExit(600000)
$sw.Stop()
$rc = -1
try { $rc = $p.ExitCode } catch { }
Say ("probe exit={0} after {1:N1}s" -f $rc, $sw.Elapsed.TotalSeconds)

$lines = Get-Content $probeOut -Encoding UTF8 -ErrorAction SilentlyContinue
foreach ($l in ($lines | Where-Object { $_ -notmatch '^ROW ' -and $_ -notmatch 'states=\[' })) { Say ('  ' + $l) }
$rows = $lines | Where-Object { $_ -match '^ROW ' }
Say ("  rows = {0}" -f $rows.Count)

Say '=== remote listing (sha256sum) ==='
$remoteTxt = Join-Path $logs ("special-remote-$stamp.txt")
$manifestOut = DQ @('exec', $Container, 'sh', '-lc', "cd /home/$User/srt-in/special 2>/dev/null && LC_ALL=C find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum")
[System.IO.File]::WriteAllText($remoteTxt, $manifestOut, (New-Object System.Text.UTF8Encoding($false)))
Say ("remote manifest lines = {0}" -f (Get-Content $remoteTxt | Measure-Object -Line).Lines)
Get-Content $remoteTxt | ForEach-Object { Say ('  ' + $_) }

Say '=== compare ==='
$cmp = & python (Join-Path $PSScriptRoot 'cmp_special.py') $manifest $remoteTxt 2>&1
$cmp | ForEach-Object { Say ('  ' + ($_ | Out-String).TrimEnd()) }
Say ("compare exit = {0}" -f $LASTEXITCODE)

DQ @('rm', '-f', $Container) | Out-Null
Say 'SPECIAL DONE'
