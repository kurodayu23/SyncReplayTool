# junction-probe.ps1 - the local link risk: what happens when the data root
# contains a Windows junction (directory link).
#
# THE RISK
#   A junction is a reparse point: a directory entry that points somewhere
#   else. It is invisible in Explorer's "properties" and it is a normal
#   directory for every API that does not look at FILE_ATTRIBUTE_REPARSE_POINT.
#   Three shapes matter:
#     extlink   -> points OUTSIDE the data root. If the scanner follows it,
#                  files the customer never selected are copied to the
#                  receiver (data leak).
#     looplink  -> points back at an ancestor of the data root. If the
#                  scanner follows it, the walk never ends and the UI just
#                  hangs.
#     deadlink  -> points at a path that does not exist. If the scanner
#                  cannot recognise it, it is dropped without any count and
#                  without any message.
#
#   Windows without Developer Mode cannot create a real symlink without
#   administrator rights, so the file-level symlink shape cannot be built
#   here. A junction covers the directory shape, which is the one that
#   leaks and the one that hangs.
#
# WHAT THIS BENCH ASSERTS
#   A  the scan finishes (no hang on the loop junction)
#   B  only the plain file is in the list (the outside file is not)
#   C  the product tells the user how many entries it skipped
#   D  the receiver gets the plain file
#   E  the receiver does NOT get the outside file  (no leak)
#   F  the receiver does not get the link directories either
#
# ASCII only (Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI and a
# Chinese comment would eat the next line).
param(
  [int]$Port = 2276,
  [string]$Container = 'srt-junction-recv'
)
$ErrorActionPreference = 'Stop'
$BuildRoot = 'D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool'
$work = Join-Path $BuildRoot 'work\junction'
$logs = Join-Path $work 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$out = Join-Path $logs ("junction-$stamp.log")
function Say([string]$m) {
  $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
  Write-Host $line
  Add-Content -LiteralPath $out -Value $line -Encoding UTF8
}

# Chinese words, built from code points so this file stays pure ASCII.
$wYou    = -join ([char[]](0x6709))                      # you / have
$wGe     = -join ([char[]](0x4E2A))                      # ge / a
$wFu     = -join ([char[]](0x7B26, 0x53F7))              # fu hao / symbol
$wLian   = -join ([char[]](0x94FE, 0x63A5))              # lian jie / link
$wMei    = -join ([char[]](0x6CA1))                      # mei / not
$wTong   = -join ([char[]](0x540C, 0x6B65))              # tong bu / sync
# "ge symbol link not synced"  -> the tail of "there are N links that were not synced"
$wLinkTail = $wGe + $wFu + $wLian + $wMei + $wYou + $wTong
$reLinkCount = [regex]::Escape($wYou) + '\s*(\d+)\s*' + [regex]::Escape($wLinkTail)

# ---------------- probe binary ----------------
$appDir = (Get-ChildItem (Join-Path $BuildRoot 'work\scale\win') -Directory | Select-Object -First 1).FullName
$probeExe = Join-Path $appDir 'srtprobe.exe'
Copy-Item -LiteralPath (Join-Path $BuildRoot 'work\probe\build\release\srtprobe.exe') -Destination $probeExe -Force

# ---------------- the lab ----------------
# layout:
#   lab\root        <- the data root handed to the product
#     plain.dat     <- a normal file, must be synced
#     extlink\      -> junction to lab\outside   (leak shape)
#     looplink\     -> junction to lab            (loop shape)
#     deadlink\     -> junction to lab\nowhere    (broken shape)
#   lab\outside
#     secret.dat    <- must never reach the receiver
$lab = Join-Path $work ("lab-$stamp")
$root = Join-Path $lab 'root'
$outside = Join-Path $lab 'outside'
New-Item -ItemType Directory -Force -Path $root, $outside | Out-Null
[System.IO.File]::WriteAllText((Join-Path $root 'plain.dat'), "plain payload`n")
[System.IO.File]::WriteAllText((Join-Path $outside 'secret.dat'), "SECRET payload`n")

$junctions = @(
  @{ Path = (Join-Path $root 'extlink');  Target = $outside; Broken = $false },
  @{ Path = (Join-Path $root 'looplink'); Target = $lab;     Broken = $false },
  @{ Path = (Join-Path $root 'deadlink'); Target = (Join-Path $lab 'nowhere-at-all'); Broken = $true }
)
foreach ($j in $junctions) {
  if ($j.Broken) {
    # New-Item -ItemType Junction refuses a target that does not exist.
    # mklink /J is the one that can build the broken shape (a junction whose
    # target is gone) - that is exactly the shape a scanner cannot stat.
    $out2 = & cmd.exe /c mklink /J "`"$($j.Path)`"" "`"$($j.Target)`"" 2>&1 | Out-String
    Say ("mklink /J (broken) said: " + ($out2 -replace '\s+', ' ').Trim())
  } else {
    New-Item -ItemType Junction -Path $j.Path -Target $j.Target -ErrorAction Stop | Out-Null
  }
  $it = Get-Item -LiteralPath $j.Path -Force
  Say ("junction created: {0} -> {1} (reparse={2})" -f $it.Name, ($it.Target -join ';'),
       (($it.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0))
}

# ---------------- receiver ----------------
$User = 'srtpw'
try { docker rm -f $Container 2>&1 | Out-Null } catch { }
docker run -d --name $Container -p "${Port}:22" kylin-v11-sshd:latest 2>&1 | Out-Null
Start-Sleep -Seconds 6
docker exec $Container sh -c "mkdir -p /run/sshd; /usr/sbin/sshd -p 22; sleep 1; mkdir -p /home/$User/srt-in; chown -R ${User}:${User} /home/$User/srt-in" 2>&1 | Out-Null
Start-Sleep -Seconds 3

$key = Join-Path $BuildRoot 'work\scale\keys\id_ed25519'
if (-not (Test-Path $key)) { throw 'key missing - run scale-probe.ps1 once first' }
$pub = ((Get-Content "$key.pub" -Raw).Trim() -replace "`r", '')
docker exec $Container sh -lc "mkdir -p /home/$User/.ssh; printf '%s\n' '$pub' > /home/$User/.ssh/authorized_keys; chmod 700 /home/$User/.ssh; chmod 600 /home/$User/.ssh/authorized_keys; chown -R ${User}:${User} /home/$User/.ssh" 2>&1 | Out-Null
Say 'receiver + key ready'

$env:QT_QPA_PLATFORM = 'offscreen'
$env:PATH = 'C:\Qt\Qt5.9.6\5.9.6\msvc2015_64\bin;' + $env:PATH

# ---------------- run ----------------
function Run-Probe([string]$tag, [string]$sub, [string]$dataRoot) {
  $cfg = Join-Path $work ("cfg-$tag-$stamp")
  New-Item -ItemType Directory -Force -Path $cfg | Out-Null
  $ini = "[syncRules]`r`nsize=1`r`n1\suffix=dat`r`n1\remoteSubDir=$sub`r`n"
  [System.IO.File]::WriteAllText((Join-Path $cfg 'SyncReplayTool.ini'), $ini, [System.Text.Encoding]::ASCII)
  $po = Join-Path $logs ("junction-$tag-$stamp.txt")
  $pe = Join-Path $logs ("junction-$tag-$stamp.err")
  $a = @('--root', $dataRoot, '--host', '127.0.0.1', '--port', "$Port", '--user', $User,
         '--key', $key, '--remote', "/home/$User/srt-in", '--sync', '--dump-rows',
         '--timeout', '90')
  $env:SRT_CONFIG_DIR = $cfg
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $probeExe -ArgumentList $a -WorkingDirectory $appDir -PassThru -NoNewWindow -RedirectStandardOutput $po -RedirectStandardError $pe
  $p.WaitForExit(180000) | Out-Null
  $sw.Stop()
  $p.Refresh()
  $txt = Get-Content $po -Encoding UTF8 -Raw
  Say ("probe[{0}] finished in {1:N1}s" -f $tag, $sw.Elapsed.TotalSeconds)
  foreach ($l in ($txt -split "`r?`n")) { if ($l -match '^(SCAN|SYNC|ROW)') { Say ("  " + $l) } }
  foreach ($l in ($txt -split "`r?`n")) { if ($l -match '^\s*LOG\b' -and $l.Contains($wLian)) { Say ("  " + $l) } }
  return $txt
}

$txt = Run-Probe 'p1' 'j1' $root

# ---------------- verdicts ----------------
$scanLine = ($txt -split "`r?`n") | Where-Object { $_ -match '^SCAN\b' } | Select-Object -First 1
$noHang   = ($null -ne $scanLine) -and ($txt -notmatch 'PROBE_FAIL scan-timeout')
$fileCount = -1
if ($scanLine -match 'files=(\d+)') { $fileCount = [int]$matches[1] }
$rows = @($txt -split "`r?`n" | Where-Object { $_ -match '^ROW\b' })
$onlyPlain = ($fileCount -eq 1)

$linkCount = 0
foreach ($l in ($txt -split "`r?`n")) {
  if ($l -notmatch '^\s*LOG\b') { continue }
  $m = [regex]::Match($l, $reLinkCount)
  if ($m.Success) { $linkCount = [int]$m.Groups[1].Value }
}

$recv = docker exec $Container sh -lc "find /home/$User/srt-in -printf '%y %P\n' 2>/dev/null" 2>&1 | Out-String
Say ("receiver tree:`n" + $recv)
# The file lands under the configured remoteSubDir, so match the tail of the
# path, not the whole relative path.
$gotPlain  = ($recv -match '(?m)^\s*f\s+.*plain\.dat\s*$')
$gotSecret = ($recv -match '(?m)^\s*f\s+.*secret\.dat\s*$')
$gotExt    = ($recv -match 'extlink')
$gotLoop   = ($recv -match 'looplink')
$gotDead   = ($recv -match 'deadlink')
$syncOk = ($txt -match 'SYNC ok=1')

# ---------------- phase 2: the control ----------------
# The same content, but as a plain file inside the data root. If this one is
# NOT picked up, then "the outside file did not arrive" in phase 1 proves
# nothing - it would just mean the .dat rule does not match at all.
$root2 = Join-Path $lab 'root2'
New-Item -ItemType Directory -Force -Path $root2 | Out-Null
[System.IO.File]::WriteAllText((Join-Path $root2 'plain.dat'), "plain payload`n")
[System.IO.File]::WriteAllText((Join-Path $root2 'secret.dat'), "SECRET payload`n")
$txt2 = Run-Probe 'p2' 'j2' $root2
$scanLine2 = ($txt2 -split "`r?`n") | Where-Object { $_ -match '^SCAN\b' } | Select-Object -First 1
$fileCount2 = -1
if ($scanLine2 -match 'files=(\d+)') { $fileCount2 = [int]$matches[1] }
$recv2 = docker exec $Container sh -lc "find /home/$User/srt-in -printf '%y %P\n' 2>/dev/null" 2>&1 | Out-String
$controlSecret = ($recv2 -match '(?m)^\s*f\s+.*secret\.dat\s*$')
Say ("CONTROL the same file as a plain file IS listed (files={0}) = {1}" -f $fileCount2, ($fileCount2 -eq 2))
Say ("CONTROL the same file as a plain file DOES reach the receiver = {0}" -f $controlSecret)

Say ("VERDICT A the scan finished (no hang on the loop junction) = {0}" -f $noHang)
Say ("VERDICT B only the plain file is in the list (files={0}) = {1}" -f $fileCount, $onlyPlain)
Say ("VERDICT C the product reported the skipped links (n={0}) = {1}" -f $linkCount, ($linkCount -ge 1))
Say ("VERDICT D the receiver got the plain file = {0}" -f $gotPlain)
Say ("VERDICT E the receiver did NOT get the outside file = {0}" -f (-not $gotSecret))
Say ("VERDICT F the receiver got no link directory = {0}" -f ((-not $gotExt) -and (-not $gotLoop) -and (-not $gotDead)))

$pass = $noHang -and $onlyPlain -and ($linkCount -ge 1) -and $gotPlain -and (-not $gotSecret) -and (-not $gotExt) -and (-not $gotLoop) -and (-not $gotDead) -and ($fileCount2 -eq 2) -and $controlSecret

# ---------------- cleanup ----------------
# A junction must be removed with rmdir: Remove-Item -Recurse follows it and
# would delete the target's content.
foreach ($j in $junctions) {
  if (Test-Path -LiteralPath $j.Path) { cmd.exe /c rmdir "$($j.Path)" 2>&1 | Out-Null }
}
try { docker rm -f $Container 2>&1 | Out-Null } catch { }

if ($pass) {
  Say 'JUNCTION-PROBE PASS'
  exit 0
}
Say 'JUNCTION-PROBE FAIL'
exit 1
