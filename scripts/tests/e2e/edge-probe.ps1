# edge-probe.ps1 - boundary cases nobody has touched yet.
#
# Covers, in one container session:
#   A  remote path contains Chinese characters AND a space
#   B  local tree contains empty directories
#   C  incremental round: remote file deleted / local file deleted / local renamed
#
# All three answer the same question: does the product tell the truth?
#
# Encoding note: Chinese text must not cross the docker CLI boundary (the CLI
# hands native args to the console code page, and stdout comes back decoded
# with it). So every container-side command here is pure ASCII, and the full
# tree listing is pulled with `docker cp` (byte exact) and read as UTF-8.
#
# ASCII only (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI and a
# Chinese literal would mangle the next line).
param(
  [int]$Port = 2258,
  [string]$Container = 'srt-edge-recv'
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\edge'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("edge-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

# ---------------- chinese text, built from code points ----------------
$zhSync = -join ([char[]](0x6570, 0x636E, 0x540C, 0x6B65))   # data sync
$zhSub  = -join ([char[]](0x5B50, 0x76EE, 0x5F55))           # sub dir
$zhTest = -join ([char[]](0x6D4B, 0x8BD5))                   # test

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

# ---------------- local tree ----------------
# the local path has a space too, on purpose
$data = Join-Path $work ("$zhTest data")
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $data 'emptyA') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $data 'deep\emptyB') | Out-Null
$names = @('a.spe','b.spe','c.spe')
$base = Get-Date
for ($i = 0; $i -lt $names.Count; $i++) {
  $p = Join-Path $data $names[$i]
  [System.IO.File]::WriteAllBytes($p, [System.Text.Encoding]::UTF8.GetBytes("content-$($names[$i])"))
  [System.IO.File]::SetLastWriteTime($p, $base.AddMinutes(-$i))
}
Say ("local root = {0}" -f $data)
Say 'empty dirs = emptyA, deep/emptyB'

# remote root: chinese + space
$remoteRoot = "/home/$User/$zhSync $zhSub"
Say ("remote root = {0}" -f $remoteRoot)

# ---------------- probe runner ----------------
$cfgDir = Join-Path $work ('cfg-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

function Q([string]$v) { if ($v -match '\s') { return '"' + $v + '"' } else { return $v } }

function Invoke-Probe([string]$tag, [int]$timeoutSec = 120) {
  $po = Join-Path $logs ("edge-$tag-$stamp.txt")
  $pe = Join-Path $logs ("edge-$tag-$stamp.err")
  $a = @(
    '--root', (Q $data),
    '--host', '127.0.0.1',
    '--port', "$Port",
    '--user', $User,
    '--key', (Q $key),
    '--remote', (Q $remoteRoot),
    '--sync',
    '--timeout', "$timeoutSec"
  )
  Say ("[$tag] running probe")
  $pp = Start-Process -FilePath $probeExe -ArgumentList $a -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po -RedirectStandardError $pe
  $pp.WaitForExit(($timeoutSec + 120) * 1000) | Out-Null
  Say ("[$tag] exit = {0}" -f $pp.ExitCode)
  Get-Content $po -Encoding UTF8 | ForEach-Object { Say ("[$tag] " + $_) }
}

# Pull the receiver's whole tree with docker cp (byte exact) and decode UTF-8.
function Get-RemoteTree {
  $dump = Join-Path $logs ("tree-$stamp.txt")
  if (Test-Path $dump) { [System.IO.File]::Delete($dump) }
  docker exec $Container sh -lc "find /home/$User -mindepth 1 | LC_ALL=C sort > /tmp/srt-tree.txt" 2>&1 | Out-Null
  docker cp ("{0}:/tmp/srt-tree.txt" -f $Container) $dump 2>&1 | Out-Null
  if (-not (Test-Path $dump)) { return @() }
  $txt = [System.IO.File]::ReadAllText($dump, [System.Text.Encoding]::UTF8)
  $res = @()
  foreach ($l in ($txt -split "`r?`n")) {
    if ($l) { $res += $l }
  }
  return $res
}

# ASCII-only counter, used as a cross-check that does not depend on decoding.
function Count-Remote([string]$namePattern) {
  $c = docker exec $Container sh -lc "find /home/$User -name '$namePattern' | wc -l" 2>&1
  return [int](($c | Out-String).Trim())
}

# ================= round 1 =================
Say '=========== ROUND 1: first transfer ==========='
Invoke-Probe 'r1'
$t1 = Get-RemoteTree
Say ("remote tree after round 1:")
foreach ($l in $t1) { Say ("    " + $l) }
Say ("count a.spe = {0}" -f (Count-Remote 'a.spe'))
$emptyDirs = @(docker exec $Container sh -lc "find /home/$User -type d -empty | wc -l" 2>&1)
Say ("empty dirs under home = {0}" -f (($emptyDirs | Out-String).Trim()))

# ================= round 2: remote file deleted =================
Say '=========== ROUND 2: a file was deleted on the receiver ==========='
docker exec $Container sh -lc "find /home/$User -name 'b.spe' -delete" 2>&1 | Out-Null
Say ("count b.spe before round 2 = {0}" -f (Count-Remote 'b.spe'))
Invoke-Probe 'r2'
$t2 = Get-RemoteTree
Say ("count b.spe after round 2 = {0}" -f (Count-Remote 'b.spe'))
Say ("VERDICT deleted-remote-file restored = {0}" -f ((Count-Remote 'b.spe') -gt 0))

# ================= round 3: local file deleted =================
Say '=========== ROUND 3: a file was deleted on the client ==========='
[System.IO.File]::Delete((Join-Path $data 'c.spe'))
Invoke-Probe 'r3'
Say ("count c.spe after round 3 = {0}" -f (Count-Remote 'c.spe'))
Say ("VERDICT stale-remote-file still on receiver = {0}" -f ((Count-Remote 'c.spe') -gt 0))

# ================= round 4: local rename =================
Say '=========== ROUND 4: a file was renamed on the client ==========='
[System.IO.File]::Move((Join-Path $data 'a.spe'), (Join-Path $data 'renamed.spe'))
Invoke-Probe 'r4'
Say ("count a.spe      after round 4 = {0}" -f (Count-Remote 'a.spe'))
Say ("count renamed.spe after round 4 = {0}" -f (Count-Remote 'renamed.spe'))
Say ("VERDICT old name still on receiver = {0}" -f ((Count-Remote 'a.spe') -gt 0))

try { docker rm -f $Container 2>&1 | Out-Null } catch { }
Say 'EDGE-PROBE DONE'
