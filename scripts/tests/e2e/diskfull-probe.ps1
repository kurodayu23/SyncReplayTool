# diskfull-probe.ps1 - the receiver runs out of disk space mid-transfer.
#
# This is one of the most common failures in the field: the receiving machine
# fills up. The product must
#   1  report the round as FAILED (never "done"),
#   2  blame the RECEIVER, not the local machine and not the network
#      (the local side and the network are both perfectly fine here),
#   3  not leave a half file under the real name on the receiver.
#
# The receiver's home is a tiny tmpfs, so it really runs out of space.
#
# ASCII only.
param(
  [int]$Port = 2260,
  [string]$Container = 'srt-diskfull-recv',
  [int]$TmpfsMB = 4,
  [int]$FileMB = 3,
  [int]$FileCount = 4
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\diskfull'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("diskfull-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

# ---------------- probe binary ----------------
$appDir = (Get-ChildItem (Join-Path $BuildRoot 'work\scale\win') -Directory | Select-Object -First 1).FullName
$probeExe = Join-Path $appDir 'srtprobe.exe'
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination $probeExe -Force

# ---------------- receiver with a tiny home ----------------
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
docker exec $Container sh -c "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh" 2>&1 | Out-Null
$df = docker exec $Container sh -lc "df -h /home/$User/srt-in | tail -n 1" 2>&1
Say ("receiver tmpfs = {0}" -f (($df | Out-String).Trim()))

# ---------------- source: random bytes (rsync -z cannot shrink them) ----------------
$data = Join-Path $work 'data'
if (Test-Path $data) { [System.IO.Directory]::Delete($data, $true) }
New-Item -ItemType Directory -Force -Path $data | Out-Null
$buf = New-Object byte[] (1MB)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
for ($i = 1; $i -le $FileCount; $i++) {
  $p = Join-Path $data ("f$i.spe")
  $fs = [System.IO.File]::Create($p)
  for ($j = 0; $j -lt $FileMB; $j++) { $rng.GetBytes($buf); $fs.Write($buf, 0, $buf.Length) }
  $fs.Close()
}
$rng.Dispose()
Say ("source = {0} files x {1} MB = {2} MB (tmpfs holds {3} MB)" -f $FileCount, $FileMB, ($FileCount*$FileMB), $TmpfsMB)

# ---------------- run ----------------
$cfgDir = Join-Path $work ('cfg-' + $stamp)
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
$env:SRT_CONFIG_DIR = $cfgDir
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

$po = Join-Path $logs ("diskfull-$stamp.txt")
$pe = Join-Path $logs ("diskfull-$stamp.err")
$a = @(
  '--root', $data,
  '--host', '127.0.0.1',
  '--port', "$Port",
  '--user', $User,
  '--key', $key,
  '--remote', "/home/$User/srt-in/dd",
  '--sync',
  '--timeout', '180'
)
Say 'running probe'
$sw = [Diagnostics.Stopwatch]::StartNew()
$pp = Start-Process -FilePath $probeExe -ArgumentList $a -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po -RedirectStandardError $pe
$pp.WaitForExit(300000) | Out-Null
$sw.Stop()
Say ("probe exit = {0} after {1:N1}s" -f $pp.ExitCode, $sw.Elapsed.TotalSeconds)
Get-Content $po -Encoding UTF8 | ForEach-Object { Say ("  " + $_) }

# ---------------- verdict ----------------
$txt = Get-Content $po -Encoding UTF8 -Raw
$failed = ($pp.ExitCode -ne 0)
$mentionsSpace = $txt.Contains('No space left on device')
# attribution layer names, built from code points so this .ps1 stays ASCII
$recvLayer  = -join ([char[]](0x63A5, 0x6536, 0x7AEF))          # receiver
$localLayer = -join ([char[]](0x672C, 0x673A, 0x7AEF))          # local machine
# The network layer's NAME, not the two characters "network": the product's
# own sentence "this is not a network problem" also contains them, so a bare
# substring test would blame the network for a correct attribution.
$netLayer   = -join ([char[]](0x7F51, 0x7EDC, 0x94FE, 0x8DEF, 0x95EE, 0x9898))
$doneWord   = -join ([char[]](0x540C, 0x6B65, 0x5B8C, 0x6210))  # sync completed
Say ("VERDICT round reported as failed = {0}" -f $failed)
Say ("VERDICT mentions 'No space left on device' = {0}" -f $mentionsSpace)
Say ("VERDICT blames receiver = {0}" -f $txt.Contains($recvLayer))
Say ("VERDICT blames local machine = {0}" -f $txt.Contains($localLayer))
Say ("VERDICT blames network = {0}" -f $txt.Contains($netLayer))
if ($txt.Contains($doneWord)) { Say 'WARNING: the log still says the sync completed' }

$real = docker exec $Container sh -lc "find /home/$User -name '*.spe' | wc -l" 2>&1
$part = docker exec $Container sh -lc "find /home/$User -path '*srt-partial*' -type f | wc -l" 2>&1
Say ("receiver: complete .spe files = {0}   files under .srt-partial = {1}" -f (($real | Out-String).Trim()), (($part | Out-String).Trim()))

try { docker rm -f $Container 2>&1 | Out-Null } catch { }
Say 'DISKFULL-PROBE DONE'
