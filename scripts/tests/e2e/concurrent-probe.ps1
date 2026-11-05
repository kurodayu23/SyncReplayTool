# concurrent-probe.ps1 - two clients pushing into the SAME receiver directory.
#
# This is a normal field setup: a workstation and a PC both sync their data
# into one folder on the server.
#
# Each client leaves a marker file in the target directory so it can tell
# "the directory was replaced during my transfer" from "my transfer worked".
# The marker name and the "clear stale markers" glob both carry a client
# scope (hostname + hash of the config dir), so one client never removes
# another client's live marker.
#
# Three phases:
#   1  two clients, same target dir  -> neither reports a false alarm
#      (before the fix the first one did: its data arrived intact and the
#       product still said "the remote directory was deleted or replaced")
#   2  the target dir REALLY is replaced mid-transfer -> the product MUST
#      still report it (a fix that disables the protection is worse than the
#      bug)
#   3  stale marker cleanup: our own stale marker is removed, another
#      client's marker is left alone
#
# ASCII only.
param(
  [int]$Port = 2267,
  [string]$Container = 'srt-conc-recv',
  [int]$BigMB = 20,
  [int]$StartDelaySec = 5
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\conc'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("conc-$stamp.log")
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

# ---------------- local trees ----------------
$dirA = Join-Path $work 'clientA'
$dirB = Join-Path $work 'clientB'
$dirC = Join-Path $work 'clientC'
foreach ($d in @($dirA, $dirB, $dirC)) {
  if (Test-Path $d) { [System.IO.Directory]::Delete($d, $true) }
  New-Item -ItemType Directory -Force -Path $d | Out-Null
}
$buf = New-Object byte[] (1MB)
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
function New-RandomFile([string]$path, [int]$mb) {
  $f = [System.IO.File]::Create($path)
  for ($i = 0; $i -lt $mb; $i++) { $rng.GetBytes($buf); $f.Write($buf, 0, $buf.Length) }
  $f.Close()
}
New-RandomFile (Join-Path $dirA 'a.spe') $BigMB
New-RandomFile (Join-Path $dirB 'b.spe') 1
New-RandomFile (Join-Path $dirC 'c.spe') $BigMB
$rng.Dispose()
Say ("clientA = {0} MB (slow), clientB = 1 MB (fast), clientC = {0} MB (slow)" -f $BigMB)

# ---------------- isolated configs ----------------
$cfgA = Join-Path $work ('cfgA-' + $stamp)
$cfgB = Join-Path $work ('cfgB-' + $stamp)
$cfgC = Join-Path $work ('cfgC-' + $stamp)
foreach ($c in @($cfgA, $cfgB, $cfgC)) { New-Item -ItemType Directory -Force -Path $c | Out-Null }
# the slow clients are throttled so they are still running when we interfere
foreach ($c in @($cfgA, $cfgC)) {
  [System.IO.File]::WriteAllText((Join-Path $c 'SyncReplayTool.ini'),
    "[sync]`r`nbandwidthLimitMBps=1`r`n", [System.Text.Encoding]::GetEncoding(936))
}
$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

function Get-Markers([string]$relDir) {
  $m = docker exec $Container sh -lc "find /home/$User/srt-in/$relDir -maxdepth 1 -name '.srt-verify*' -printf '%f\n' 2>/dev/null | LC_ALL=C sort" 2>&1
  $r = @()
  foreach ($l in $m) { if ($l) { $r += $l.Trim() } }
  return $r
}

# ================= phase 1: two clients, same target dir =================
Say '=========== PHASE 1: two clients, same target dir ==========='
$remote = 'shared'
$outA = Join-Path $logs ("concA-$stamp.txt")
$outB = Join-Path $logs ("concB-$stamp.txt")
$errA = Join-Path $logs ("concA-$stamp.err")
$errB = Join-Path $logs ("concB-$stamp.err")
$argsA = @('--root', $dirA, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
          '--key', $key, '--remote', "/home/$User/srt-in/$remote", '--sync', '--timeout', '180')
$argsB = @('--root', $dirB, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
          '--key', $key, '--remote', "/home/$User/srt-in/$remote", '--sync', '--timeout', '180')

$env:SRT_CONFIG_DIR = $cfgA
$pa = Start-Process -FilePath $probeExe -ArgumentList $argsA -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $outA -RedirectStandardError $errA
Say 'clientA started'
Start-Sleep -Seconds $StartDelaySec

$env:SRT_CONFIG_DIR = $cfgB
$pb = Start-Process -FilePath $probeExe -ArgumentList $argsB -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $outB -RedirectStandardError $errB
Say 'clientB started'

# while clientA is still transferring, look at the markers both clients left
$seen = @{}
for ($i = 0; $i -lt 12; $i++) {
  foreach ($m in (Get-Markers $remote)) { $seen[$m] = $true }
  Start-Sleep -Milliseconds 500
}
foreach ($k in $seen.Keys) { Say ("  marker seen: " + $k) }

$pa.WaitForExit(300000) | Out-Null
$pb.WaitForExit(300000) | Out-Null
$txtA = Get-Content $outA -Encoding UTF8 -Raw
$txtB = Get-Content $outB -Encoding UTF8 -Raw
$swapped = -join ([char[]](0x88AB, 0x5220, 0x9664, 0x6216, 0x66FF, 0x6362))
Say ("clientA: ok={0}" -f ($txtA -match 'SYNC ok=1'))
Say ("clientB: ok={0}" -f ($txtB -match 'SYNC ok=1'))
Say ("VERDICT clientA false alarm = {0}" -f $txtA.Contains($swapped))
Say ("VERDICT clientB false alarm = {0}" -f $txtB.Contains($swapped))
$sz = docker exec $Container sh -lc "find /home/$User/srt-in/$remote -maxdepth 1 -name 'a.spe' -printf '%s\n' 2>/dev/null" 2>&1
$remoteSize = [int](($sz | Select-Object -First 1 | Out-String).Trim())
Say ("VERDICT clientA data intact = {0}" -f ($remoteSize -eq (Get-Item (Join-Path $dirA 'a.spe')).Length))
$markers = @($seen.Keys)
Say ("DEBUG seen={0} markers={1} first=[{2}]" -f $seen.Count, $markers.Count, (@($markers)[0]))

# 作用域 = 主机名（下划线化、截 24）+ '-' + 配置目录路径的 md5 前 8 位。
# 这里按产品同样的公式算一遍（QDir::absolutePath 在 Windows 上出的是正斜杠）。
$hostEnv = $env:COMPUTERNAME
if (-not $hostEnv) { $hostEnv = [System.Environment]::MachineName }
$hostRaw = $hostEnv.ToLower()
$hostKeep = -join ($hostRaw.ToCharArray() | ForEach-Object {
  if ($_ -match '[a-z0-9\-]') { $_ } else { '_' } })
if ($hostKeep.Length -gt 24) { $hostKeep = $hostKeep.Substring(0, 24) }
$md5 = [System.Security.Cryptography.MD5]::Create()
$cfgPathA = $cfgA.Replace('\', '/')
$hashHexA = -join ($md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($cfgPathA)) |
                   ForEach-Object { $_.ToString('x2') })
$scopeA = $hostKeep + '-' + $hashHexA.Substring(0, 8)
$cfgPathB = $cfgB.Replace('\', '/')
$hashHexB = -join ($md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($cfgPathB)) |
                   ForEach-Object { $_.ToString('x2') })
$scopeB = $hostKeep + '-' + $hashHexB.Substring(0, 8)
Say ("scope for clientA = {0}" -f $scopeA)
Say ("scope for clientB = {0}" -f $scopeB)
Say ("VERDICT the two clients have different scopes = {0}" -f ($scopeA -ne $scopeB))

# ================= phase 2: the dir really is replaced =================
Say '=========== PHASE 2: the target dir is really replaced mid-transfer ==========='
$remote2 = 'swapped'
$outC = Join-Path $logs ("concC-$stamp.txt")
$errC = Join-Path $logs ("concC-$stamp.err")
$argsC = @('--root', $dirC, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
          '--key', $key, '--remote', "/home/$User/srt-in/$remote2", '--sync', '--timeout', '180')
$env:SRT_CONFIG_DIR = $cfgC
$pc = Start-Process -FilePath $probeExe -ArgumentList $argsC -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $outC -RedirectStandardError $errC
Say 'clientC started (slow)'
Start-Sleep -Seconds $StartDelaySec
# move the directory aside and create a fresh one: same name, new inode
docker exec $Container sh -lc "mv /home/$User/srt-in/$remote2 /home/$User/srt-in/${remote2}-old && mkdir -p /home/$User/srt-in/$remote2 && chown -R ${User}:${User} /home/$User/srt-in/$remote2" 2>&1 | Out-Null
Say 'target dir replaced'
$pc.WaitForExit(300000) | Out-Null
$txtC = Get-Content $outC -Encoding UTF8 -Raw
Say ("clientC: ok={0}" -f ($txtC -match 'SYNC ok=1'))
Say ("VERDICT the real replacement IS reported = {0}" -f $txtC.Contains($swapped))
if ($txtC.Contains($swapped)) {
  foreach ($l in ($txtC -split "`r?`n")) { if ($l -match 'LOG.*(srt|SS|swapped)') { } }
}

# ================= phase 3: stale marker cleanup is scoped =================
Say '=========== PHASE 3: stale marker cleanup only touches our own ==========='
# 作用域在上一段已经按产品公式算好了。
Say ("scope for clientA = {0}" -f $scopeA)
$scopeHit = @($markers | Where-Object { $_ -like ".srt-verify-$scopeA-*" })
Say ("VERDICT the marker the product wrote carries that scope = {0}" -f ($scopeHit.Count -ge 1))

docker exec $Container sh -lc "printf x > '/home/$User/srt-in/$remote/.srt-verify-$scopeA-999-1'" 2>&1 | Out-Null
docker exec $Container sh -lc "printf x > '/home/$User/srt-in/$remote/.srt-verify-otherclient-999-1'" 2>&1 | Out-Null
$before = Get-Markers $remote
Say ("markers before the sync = {0}" -f ($before -join ', '))
$outD = Join-Path $logs ("concD-$stamp.txt")
$errD = Join-Path $logs ("concD-$stamp.err")
$argsD = @('--root', $dirA, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
          '--key', $key, '--remote', "/home/$User/srt-in/$remote", '--sync', '--timeout', '180')
$env:SRT_CONFIG_DIR = $cfgA
$pd = Start-Process -FilePath $probeExe -ArgumentList $argsD -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $outD -RedirectStandardError $errD
$pd.WaitForExit(300000) | Out-Null
$after = Get-Markers $remote
Say ("markers after the sync = {0}" -f ($after -join ', '))
Say ("VERDICT our own stale marker was cleaned = {0}" -f (-not ($after -contains ".srt-verify-$scopeA-999-1")))
Say ("VERDICT the other client's marker was left alone = {0}" -f ($after -contains '.srt-verify-otherclient-999-1'))

try { docker rm -f $Container 2>&1 | Out-Null } catch { }
Say 'CONC-PROBE DONE'
