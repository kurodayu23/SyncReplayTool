#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""限速档位是否真的限住了：--bwlimit=<KiB/s> 的实测速率必须落在档位附近。

产品把「限速」做成档位（需求里的几档），底层就是 rsync --bwlimit。
本测试对两个档位各传一次 64MB，断言：
  1) rc=0（限速不会把传输搞失败）
  2) 实测速率 <= 档位 * 1.35（不能超速）
  3) 实测速率 >= 档位 * 0.50（也不能慢到限速没生效 / 链路异常）
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_common as pc

SRC = os.path.join(pc.STAGE, "bwlimit")
LOG = "bw-precise.log"
RSYNC = pc.rsync_bin()
log = pc.logger(LOG)

SIZE = 64 * 1024 * 1024
DEST = pc.DEST

env = pc.tool_env()


def main():
    pc.ensure_file(os.path.join(SRC, "bw.spe"), SIZE)
    pc.write_list(os.path.join(SRC, ".fl.txt"), ["bw.spe"])

    fails = 0
    for kb in (5120, 10240):
        pc.clear_remote()
        t0 = time.time()
        r = subprocess.run(
            [RSYNC] + list(pc.BASE_ARGS) + ["--bwlimit=%d" % kb,
                                             "-e", pc.RSH, "./", DEST],
            cwd=SRC, env=env, capture_output=True, timeout=600)
        dt = time.time() - t0
        mib = (SIZE / 1024.0 / 1024.0) / dt if dt > 0 else 0.0
        limit = kb / 1024.0
        ok_rc = (r.returncode == 0)
        ok_fast = (mib <= limit * 1.35)
        ok_slow = (mib >= limit * 0.50)
        ok = ok_rc and ok_fast and ok_slow
        if not ok:
            fails += 1
        log("%s --bwlimit=%d KiB/s (档位 %.2f MiB/s) -> rc=%d 用时 %.1fs 实测 %.2f MiB/s"
            % ("PASS" if ok else "FAIL", kb, limit, r.returncode, dt, mib))
        if not ok_rc and r.stderr:
            log("    stderr: " + r.stderr.decode("utf-8", "replace")[:300])
        if not ok_fast:
            log("    FAIL 超速：实测 %.2f > 档位上限 %.2f" % (mib, limit * 1.35))
        if not ok_slow:
            log("    FAIL 过慢：实测 %.2f < 档位下限 %.2f（限速可能没生效）"
                % (mib, limit * 0.50))

    log("== 全部档位完成：%d/2 通过 ==" % (2 - fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
