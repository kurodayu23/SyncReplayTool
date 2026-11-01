<#
.SYNOPSIS
    边界 / 负向用例链（2026-11-02 新增，收编进仓库）。

.DESCRIPTION
    跑 6 个真实传输场景的边界与负向用例：

      remote_path_edge       远端目标路径边界：中文 / 空格 / 括号 / 深层嵌套
      ignore_existing_resume --ignore-existing（仅同步新增文件）下中断 -> 半截不许落真名
      partial_resume         中断续传：真名不落盘 + 半截在 .srt-partial + 重传自愈
      bw_precise             限速档位是否真的限住了（实测速率必须落在档位附近）
      bw_watchdog            限速下看门狗会不会误杀（stdout 最大静默 < 180s）
      concurrent_senders     两路发送端并发写同一远端目录

    ★ 为什么单独一个脚本、而不是并进 run-all-tests.ps1 的七段门禁：
      这几条要 2GB 夹具 + 容器接收端 + 约 4 分钟，而且 bw 那两条对机器负载敏感
      （别的任务同时抢带宽会误报）。交付门禁要的是「稳定、每次都能跑的判定」，
      所以边界链挂在旁边单独跑，跑完把结果贴进测试报告。

    ★ 判据纪律（照搬本项目一贯要求）：
      · 探针必须自己造夹具、自己清接收端 —— 幂等，可重复跑；
      · 每个断言必须打印一行 PASS/FAIL 判决；
      · **没有判决 = FAIL**。崩了的探针绝不能被当成绿的（这是本脚本 11-02 加的闸门）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\run-edge-cases.ps1
#>
[CmdletBinding()]
param(
    [string]$TestsDir = 'D:\AP\AP7000\SyncReplayTool\scripts\tests',
    [string]$WorkRoot = 'D:\AP\AP7000\ai-workspace\SyncReplayTool'
)

$ErrorActionPreference = 'Continue'
$logDir = Join-Path $WorkRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

# NOTE: keep this file ASCII-only. PowerShell 5.1 reads a BOM-less .ps1 as ANSI/GBK,
# so any CJK identifier or string in here turns into mojibake and the script
# fails to parse. (Same trap the project's own STATUS.md records.)
$cases = @(
    @{ n = 'remote_path_edge';       f = 'remote_path_edge.py' },
    @{ n = 'ignore_existing_resume'; f = 'ignore_existing_resume.py' },
    @{ n = 'partial_resume';         f = 'partial_resume.py' },
    @{ n = 'bw_precise';             f = 'bw_precise.py' },
    @{ n = 'bw_watchdog';            f = 'bw_watchdog.py' },
    @{ n = 'concurrent_senders';     f = 'concurrent_senders.py' }
)

$results = @()
foreach ($c in $cases) {
    $path = Join-Path $TestsDir $c.f
    if (-not (Test-Path $path)) {
        $results += [pscustomobject]@{ Case = $c.n; Result = 'SKIP'; Note = 'probe file missing' }
        continue
    }
    Write-Host ("`n########## {0} ##########" -f $c.n)
    $out = & python $path 2>&1
    $rc = $LASTEXITCODE
    $out | Out-File -FilePath (Join-Path $logDir ("edge-" + $c.n + ".log")) -Encoding UTF8
    $out | Select-Object -Last 14 | ForEach-Object { Write-Host ("  " + $_) }

    # A probe must leave an explicit verdict for every assertion it makes.
    # No verdict line at all => the probe crashed => FAIL. Never report green
    # for a probe that produced nothing.
    $passCnt = @($out | Select-String -Pattern 'PASS').Count
    $failCnt = @($out | Select-String -Pattern 'FAIL').Count
    $traceCnt = @($out | Select-String -Pattern 'Traceback|Error|error:|AssertionError').Count
    if ($passCnt -eq 0 -and $failCnt -eq 0) {
        $results += [pscustomobject]@{ Case = $c.n; Result = 'FAIL'; Note = "no verdict (rc=$rc, trace=$traceCnt)" }
    } elseif ($rc -ne 0 -or $failCnt -gt 0 -or $traceCnt -gt 0) {
        $results += [pscustomobject]@{ Case = $c.n; Result = 'FAIL'; Note = "rc=$rc pass=$passCnt fail=$failCnt trace=$traceCnt" }
    } else {
        $results += [pscustomobject]@{ Case = $c.n; Result = 'PASS'; Note = "rc=0, $passCnt verdicts" }
    }
}

Write-Host "`n===== EDGE / NEGATIVE CASE SUMMARY =====" -ForegroundColor Cyan
$results | Format-Table -AutoSize
$results | ConvertTo-Json | Out-File (Join-Path $logDir 'edge-summary.json') -Encoding utf8
if (@($results | Where-Object { $_.Result -eq 'FAIL' }).Count -gt 0) { exit 1 } else { exit 0 }
