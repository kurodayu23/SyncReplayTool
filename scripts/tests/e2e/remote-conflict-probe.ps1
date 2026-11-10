# remote-conflict-probe.ps1 - D3: a file on the receiver is silently replaced
# by a directory that comes from the client.
#
# Field shape (this happens whenever a customer renames a file into a
# folder of the same name, or copies a folder over an older file):
#
#   receiver : <remoteRoot>/<sub>/conflict.dat   = a 12 byte FILE
#   client   : <localRoot>/conflict.dat/inner.dat = a DIRECTORY
#
# Measured on 2026-11-10 (real container, real rsync, real key auth):
#   rsync creates the directory on the receiver, and creating a directory
#   where a file lives means the file is unlinked. The 12 bytes are gone.
#   The product reports status="done", ok=1, 1/1, and the log contains no
#   hint at all - rsync does not complain, it just replaces the file.
#   => a customer loses a file on the receiver and the UI says it worked.
#
# The opposite direction IS protected today: receiver has the DIRECTORY,
# client has the FILE. rsync fails with code 23 ("could not make way for
# new regular file"), the product says "the receiver already has a
# directory with that name" and the round is judged failed, the receiver
# keeps its data. Phase 2 keeps that behaviour green - the protection is
# one sided, and the missing half is D3.
#
# What this bench asserts:
#   phase 1  the receiver file must survive, and the round must not be
#            reported as a plain success with no hint
#   phase 2  the receiver directory must survive, the round must be
#            judged failed, and the log must name the conflict
#
# ASCII only (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI and a
# Chinese comment would eat the next line).
param(
  [int]$Port = 2273,
  [string]$Container = 'srt-conflict-recv'
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\conflict'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("conflict-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

# Chinese words we look for in the log, built from code points so this
# file stays pure ASCII.
$wConflict = -join ([char[]](0x51B2, 0x51B2))   # conflict
$wSameName = -join ([char[]](0x540C, 0x540D))   # same name
$wDeleted  = -join ([char[]](0x5220, 0x9664))   # deleted
$wReplaced = -join ([char[]](0x66FF, 0x6362))   # replaced
$wBlocking = -join ([char[]](0x6321, 0x8DEF))   # in the way
$wDone     = -join ([char[]](0x5DF2, 0x5B8C, 0x6210))  # done
$wFailed   = -join ([char[]](0x5931, 0x8D25))   # failed
$hintWords = @($wConflict, $wSameName, $wDeleted, $wReplaced, $wBlocking)

# A hint only counts on a WARN or ERR line. Searching the whole text gives a
# false positive: the product always logs "if the target host is replaced,
# the program cannot tell" (INFO) on every connection, and that sentence
# contains the word "replaced".
function Has-Hint([string]$txt) {
  foreach ($l in ($txt -split "`r?`n")) {
    if ($l -notmatch '^\s*LOG\s+\[[^\]]*\]\s+(WARN|ERR)\b') { continue }
    foreach ($h in $hintWords) { if ($l.Contains($h)) { return $true } }
  }
  return $false
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

$key = Join-Path $BuildRoot 'work\scale\keys\id_ed25519'
if (-not (Test-Path $key)) {
  $key = Join-Path $BuildRoot 'work\keymode\keys\id_ed25519'
}
if (-not (Test-Path $key)) { throw 'key missing - run scale-probe.ps1 once first' }
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
docker exec $Container sh -lc "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh" 2>&1 | Out-Null
Say 'receiver + key ready'

$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

# The receiver side file content (12 bytes) - it must survive phase 1.
$fileBody = "i am a file`n"
$dirBody  = "keep me`n"

function New-Run([string]$tag, [string]$sub, [string]$localRoot) {
  $cfg = Join-Path $work ("cfg-$tag-" + $stamp)
  New-Item -ItemType Directory -Force -Path $cfg | Out-Null
  # The rule carries the remote sub directory. The probe must NOT be given
  # --rules: that would replace the rule and drop remoteSubDir.
  $ini = "[syncRules]`r`nsize=1`r`n1\suffix=dat`r`n1\remoteSubDir=$sub`r`n"
  [System.IO.File]::WriteAllText((Join-Path $cfg 'SyncReplayTool.ini'), $ini, [System.Text.Encoding]::ASCII)
  $po = Join-Path $logs ("conflict-$tag-$stamp.txt")
  $pe = Join-Path $logs ("conflict-$tag-$stamp.err")
  $a = @('--root', $localRoot, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
         '--key', $key, '--remote', "/home/$User/srt-in", '--sync', '--dump-rows',
         '--timeout', '180')
  $env:SRT_CONFIG_DIR = $cfg
  $p = Start-Process -FilePath $probeExe -ArgumentList $a -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po -RedirectStandardError $pe
  $p.WaitForExit(300000) | Out-Null
  $p.Refresh()
  $txt = Get-Content $po -Encoding UTF8 -Raw
  return @{ exit = $p.ExitCode; text = $txt }
}

# ---------------- phase 1: receiver FILE vs client DIRECTORY ----------------
Say '=========== PHASE 1: receiver file vs client directory (D3) ==========='
$sub1 = 'd3a'
$local1 = Join-Path $work ("src1-" + $stamp)
New-Item -ItemType Directory -Force -Path (Join-Path $local1 'conflict.dat') | Out-Null
[System.IO.File]::WriteAllText((Join-Path $local1 'conflict.dat\inner.dat'), "inner payload`n")
docker exec $Container sh -lc "mkdir -p /home/$User/srt-in/$sub1; printf 'i am a file\n' > /home/$User/srt-in/$sub1/conflict.dat; chown -R ${User}:${User} /home/$User/srt-in/$sub1" 2>&1 | Out-Null
$before = docker exec $Container sh -lc "find /home/$User/srt-in/$sub1 -maxdepth 1 -name 'conflict.dat' -printf '%y %s\n' 2>/dev/null" 2>&1
Say ("receiver before = {0}" -f (($before | Out-String).Trim()))
$r1 = New-Run 'p1' $sub1 $local1
foreach ($l in ($r1.text -split "`r?`n")) { if ($l -match '^(SCAN|SYNC|ROW)') { Say ("  " + $l) } }
$statusLine = ''
if ($r1.text -match 'status="([^"]*)"') { $statusLine = $matches[1] }
$after = docker exec $Container sh -lc "find /home/$User/srt-in/$sub1 -maxdepth 1 -name 'conflict.dat' -printf '%y %s\n' 2>/dev/null" 2>&1
Say ("receiver after  = {0}" -f (($after | Out-String).Trim()))
$body = docker exec $Container sh -lc "cat /home/$User/srt-in/$sub1/conflict.dat 2>/dev/null" 2>&1
$bodyTxt = ($body | Out-String).Trim()
$stillFile = (($after | Out-String) -match '(?m)^\s*f\s+12\s*$')
$bodyOk = ($bodyTxt -eq $fileBody.Trim())
$saidDone = $statusLine.Contains($wDone)
$hasHint = Has-Hint $r1.text
Say ("VERDICT the receiver file is still a file = {0}" -f $stillFile)
Say ("VERDICT the receiver file still has its 12 bytes = {0}" -f $bodyOk)
Say ("VERDICT the status is a plain done = {0}" -f $saidDone)
Say ("VERDICT the log names the conflict = {0}" -f $hasHint)
Say ("VERDICT no data loss on the receiver = {0}" -f ($stillFile -and $bodyOk))
Say ("VERDICT the user is warned = {0}" -f ((-not $saidDone) -or $hasHint))

# ---------------- phase 2: receiver DIRECTORY vs client FILE ----------------
Say '=========== PHASE 2: receiver directory vs client file (must stay protected) ==========='
$sub2 = 'd3b'
$local2 = Join-Path $work ("src2-" + $stamp)
New-Item -ItemType Directory -Force -Path $local2 | Out-Null
[System.IO.File]::WriteAllText((Join-Path $local2 'conflict.dat'), "i am now a file`n")
docker exec $Container sh -lc "mkdir -p /home/$User/srt-in/$sub2/conflict.dat; printf 'keep me\n' > /home/$User/srt-in/$sub2/conflict.dat/keep.dat; chown -R ${User}:${User} /home/$User/srt-in/$sub2" 2>&1 | Out-Null
$r2 = New-Run 'p2' $sub2 $local2
foreach ($l in ($r2.text -split "`r?`n")) { if ($l -match '^(SCAN|SYNC|ROW)') { Say ("  " + $l) } }
$status2 = ''
if ($r2.text -match 'status="([^"]*)"') { $status2 = $matches[1] }
$after2 = docker exec $Container sh -lc "find /home/$User/srt-in/$sub2 -maxdepth 1 -name 'conflict.dat' -printf '%y %s\n' 2>/dev/null" 2>&1
$keep = docker exec $Container sh -lc "cat /home/$User/srt-in/$sub2/conflict.dat/keep.dat 2>/dev/null" 2>&1
Say ("receiver after  = {0}" -f (($after2 | Out-String).Trim()))
$stillDir = (($after2 | Out-String) -match '(?m)^\s*d\s+')
$keepOk = ((($keep | Out-String).Trim()) -eq $dirBody.Trim())
$failed2 = ($status2.Contains($wFailed)) -or ($r2.text -match 'SYNC ok=0')
$hint2 = Has-Hint $r2.text
Say ("VERDICT the receiver directory survived = {0}" -f $stillDir)
Say ("VERDICT the file inside it survived = {0}" -f $keepOk)
Say ("VERDICT the round was judged failed = {0}" -f $failed2)
Say ("VERDICT the log names the conflict = {0}" -f $hint2)

# ---------------- verdict ----------------
$ok1 = $stillFile -and $bodyOk -and ((-not $saidDone) -or $hasHint)
$ok2 = $stillDir -and $keepOk -and $failed2 -and $hint2
Say ("RESULT phase 1 (D3 fixed) = {0}" -f $ok1)
Say ("RESULT phase 2 (control still protected) = {0}" -f $ok2)
if ($ok1 -and $ok2) {
  Say 'CONFLICT-PROBE PASS'
  try { docker rm -f $Container 2>&1 | Out-Null } catch { }
  exit 0
}
Say 'CONFLICT-PROBE FAIL - the receiver can still lose a file without a word'
try { docker rm -f $Container 2>&1 | Out-Null } catch { }
exit 1
