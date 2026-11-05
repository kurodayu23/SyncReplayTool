# scale-probe.ps1 - 100k-file round trip through the product's real sync path.
# Uses the headless probe (no GUI, no mouse). ASCII only.
param(
  [int]$Files = 100000,
  [int]$Port = 2255,
  [string]$Container = 'srt-scale-recv',
  [int]$TimeoutSec = 3600
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\scale'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("scale-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

$appDir = (Get-ChildItem (Join-Path $work 'win') -Directory | Select-Object -First 1).FullName
$probeExe = Join-Path $appDir 'srtprobe.exe'
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination $probeExe -Force

# ---------------- receiver ----------------
$User = 'srtpw'
docker rm -f $Container 2>&1 | Out-Null
docker run -d --name $Container -p "${Port}:22" kylin-v11-sshd:latest 2>&1 | Out-Null
Start-Sleep -Seconds 6
docker exec $Container sh -c "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in" 2>&1 | Out-Null
Start-Sleep -Seconds 3

# ---------------- key ----------------
$keydir = Join-Path $work 'keys'
$key = Join-Path $keydir 'id_ed25519'
if (-not (Test-Path $key)) { throw 'key missing' }
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
docker exec $Container sh -c "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh" 2>&1 | Out-Null
Say 'receiver + key ready'

# ---------------- source data ----------------
$data = Join-Path $work 'scale-data'
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
$sw = [Diagnostics.Stopwatch]::StartNew()
$g = & python (Join-Path $PSScriptRoot 'gen_scale_files.py') $data $Files 2>&1
Say ("generated {0} in {1:N1}s" -f ($g | Out-String).Trim(), $sw.Elapsed.TotalSeconds)
$sw = [Diagnostics.Stopwatch]::StartNew()
$localTree = (& python (Join-Path $PSScriptRoot 'tree_hash.py') $data spe 2>&1 | Select-Object -First 1)
Say ("local tree = {0} ({1:N1}s)" -f $localTree, $sw.Elapsed.TotalSeconds)

# ---------------- run the probe ----------------
$cfgDir = Join-Path $BuildRoot ('work\probe\cfg\scale-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

$probeOut = Join-Path $logs ("scale-$stamp.txt")
$probeErr = Join-Path $logs ("scale-$stamp.err")
$args = @(
  '--root', $data, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
  '--key', $key, '--remote', '/home/srtpw/srt-in/scale',
  '--sync', '--timeout', "$TimeoutSec"
)
Say 'starting probe'
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $probeExe -ArgumentList $args -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $probeOut -RedirectStandardError $probeErr
$peak = 0
$lastLog = ''
while (-not $p.HasExited) {
  Start-Sleep -Seconds 5
  try {
    $p.Refresh()
    if ($p.WorkingSet64 -gt $peak) { $peak = $p.WorkingSet64 }
  } catch { }
  if (Test-Path $probeOut) {
    $tail = (Get-Content $probeOut -Tail 1 -ErrorAction SilentlyContinue)
    if ($tail -and $tail -ne $lastLog) { $lastLog = $tail; Say ("  {0:N0}s ws={1:N0}MB | {2}" -f $sw.Elapsed.TotalSeconds, ($peak/1MB), $tail) }
  }
}
$sw.Stop()
Say ("probe exited {0} after {1:N1}s, peak WS = {2:N0} MB" -f $p.ExitCode, $sw.Elapsed.TotalSeconds, ($peak/1MB))
Get-Content $probeOut -Encoding UTF8 | Select-Object -First 6 | ForEach-Object { Say ("  " + $_) }
Get-Content $probeOut -Encoding UTF8 | Select-Object -Last 8 | ForEach-Object { Say ("  " + $_) }
Say ("log lines = {0}" -f (Get-Content $probeOut | Measure-Object -Line).Lines)

# ---------------- reconcile ----------------
Say '=== reconcile (byte for byte) ==='
$sw = [Diagnostics.Stopwatch]::StartNew()
$remoteCount = docker exec $Container sh -lc "cd /home/$User/srt-in/scale 2>/dev/null && find . -type f | wc -l" 2>&1
Say ("remote files = {0} ({1:N1}s)" -f ($remoteCount | Out-String).Trim(), $sw.Elapsed.TotalSeconds)
$sw = [Diagnostics.Stopwatch]::StartNew()
$rl = docker exec $Container sh -lc "cd /home/$User/srt-in/scale 2>/dev/null && LC_ALL=C find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum" 2>&1
Say ("remote listing hash = {0} ({1:N1}s)" -f ($rl | Out-String).Trim(), $sw.Elapsed.TotalSeconds)
Say ("local  listing hash = {0}" -f $localTree)

docker rm -f $Container 2>&1 | Out-Null
Say 'SCALE-PROBE DONE'
