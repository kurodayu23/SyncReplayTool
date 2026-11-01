#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""限速下看门狗是否会误杀：测 rsync 在 --bwlimit 下的**输出静默间隔**。

产品看门狗是「静默超时」：每收到一行 stdout/stderr 就重置，预算最低 180s。
所以只要限速下 rsync 仍周期性输出，就不会被误杀；若出现 >180s 的静默，就会被杀。

做法：50MB 文件 + --bwlimit=512（512 KiB/s，约 100 秒），逐字节记录 stdout 到达时间。
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_common as pc

SRC = os.path.join(pc.STAGE, "bwtest")
LOG = "bw-watchdog.log"
RSYNC = pc.rsync_bin()
log = pc.logger(LOG)

SIZE = 50 * 1024 * 1024
DEST = pc.DEST
WATCHDOG_BUDGET = 180.0

env = pc.tool_env()


def main():
    pc.ensure_file(os.path.join(SRC, "mid.spe"), SIZE, filler=os.urandom(1024))
    pc.write_list(os.path.join(SRC, ".fl.txt"), ["mid.spe"])
    pc.clear_remote()

    base = list(pc.BASE_ARGS) + ["--info=progress2", "-z",
                                 "--bwlimit=512", "-e", pc.RSH, "./", DEST]

    t0 = time.time()
    p = subprocess.Popen([RSYNC] + base, cwd=SRC, env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stamps = []
    while True:
        chunk = p.stdout.read(1)
        if not chunk:
            break
        stamps.append(time.time() - t0)
    p.wait()
    dt = time.time() - t0
    ok_rc = (p.returncode == 0)
    log("%s rc=%d 总耗时=%.1fs stdout 到达 %d 次"
        % ("PASS" if ok_rc else "FAIL", p.returncode, dt, len(stamps)))

    if len(stamps) < 2:
        log("FAIL stdout 太少（%d 次），无法统计间隔" % len(stamps))
        return 1

    gaps = [stamps[i + 1] - stamps[i] for i in range(len(stamps) - 1)]
    max_gap = max(gaps)
    ok_gap = max_gap < WATCHDOG_BUDGET
    log("最大静默间隔 = %.1fs（看门狗最低预算 %.0fs）" % (max_gap, WATCHDOG_BUDGET))
    log("%s 限速下不会被看门狗误杀" % ("PASS" if ok_gap else "FAIL 有 >%.0fs 静默" % WATCHDOG_BUDGET))
    log("平均间隔 %.2fs，共 %d 次输出" % (sum(gaps) / len(gaps), len(stamps)))

    allok = ok_rc and ok_gap
    log("== %s ==" % ("全部通过" if allok else "存在失败项"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
