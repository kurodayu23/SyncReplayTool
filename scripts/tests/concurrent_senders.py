#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两路发送端并发写同一远端目录：验证互不破坏。

客户场景：多台机器各跑一份本工具，往同一台接收端的同一目录投数据
（或者一台机器上一次没传完、用户又开了另一台）。
验证点：
  1) 两路都 rc=0
  2) 接收端两套文件都完整、每个都是 4MB
  3) 没有互相删掉/覆盖（10 个文件一个不少）
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_common as pc

BASE = os.path.join(pc.STAGE, "concurrent")
LOG = "concurrent-senders.log"
RSYNC = pc.rsync_bin()
log = pc.logger(LOG)

DEST = pc.DEST
FILE_SIZE = 4 * 1024 * 1024

env = pc.tool_env()


def main():
    expected = []
    for side in ("A", "B"):
        d = os.path.join(BASE, side)
        os.makedirs(d, exist_ok=True)
        names = []
        for i in range(1, 6):
            n = "%s%02d.spe" % (side.lower(), i)
            pc.ensure_file(os.path.join(d, n), FILE_SIZE,
                           filler=(b"A" if side == "A" else b"B"))
            names.append(n)
            expected.append(n)
        pc.write_list(os.path.join(d, ".fl.txt"), names)

    pc.clear_remote()
    log("== 同时启动两路 rsync ==")
    procs = []
    for side in ("A", "B"):
        d = os.path.join(BASE, side)
        p = subprocess.Popen(
            [RSYNC] + list(pc.BASE_ARGS) + ["-e", pc.RSH, "./", DEST],
            cwd=d, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        procs.append((side, p))
    rc_ok = True
    for side, p in procs:
        try:
            _, err = p.communicate(timeout=600)
        except subprocess.TimeoutExpired:
            p.kill()
            _, err = p.communicate()
        good = (p.returncode == 0)
        rc_ok = rc_ok and good
        log("%s %s 路 rc=%d%s" % ("PASS" if good else "FAIL", side, p.returncode,
                                  "" if good else " stderr="
                                  + err.decode("utf-8", "replace")[:200]))

    listing = pc.sh_out("ls -A1 %s 2>/dev/null | sort" % pc.REMOTE_DIR)
    got = [x for x in listing.splitlines() if x.endswith(".spe")]
    sizes = {}
    for n in got:
        s = pc.sh_out("stat -c %%s '%s/%s' 2>/dev/null" % (pc.REMOTE_DIR, n))
        sizes[n] = s

    log("接收端文件: %s" % " ".join(got))
    log("大小: %s" % " ".join("%s:%s" % (k, v) for k, v in sorted(sizes.items())))

    names_ok = (sorted(got) == sorted(expected))
    size_ok = bool(got) and all(v == str(FILE_SIZE) for v in sizes.values())
    log("%s 两路文件集合完整（共 %d 个，期望 %d 个）"
        % ("PASS" if names_ok else "FAIL", len(got), len(expected)))
    log("%s 每个文件大小均为 %d 字节"
        % ("PASS" if size_ok else "FAIL", FILE_SIZE))

    allok = rc_ok and names_ok and size_ok
    log("== %s ==" % ("全部通过" if allok else "存在失败项"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
