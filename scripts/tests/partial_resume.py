#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""中断/续传验证（README 承诺的行为）。

README：「传输失败时，程序已传输的部分留在接收端的 .srt-partial/ 目录下，
        没有以原文件名落盘，下次同步会因为大小不一致自动重传，不需要手动删除。」

验证点：
  1) 传大文件中途 kill -> 接收端**真名文件不存在**，.srt-partial/ 里有半截
  2) 再传一次 -> 续传/重传成功，真名文件大小正确
  3) 接收端 md5 与源一致
  4) 传输完成后 .srt-partial 无残留
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_common as pc

SRC = os.path.join(pc.STAGE, "partialtest")
LOG = "partial-resume.log"
RSYNC = pc.rsync_bin()
log = pc.logger(LOG)

SIZE = int(os.environ.get("SRT_BIG_MB", "2048")) * 1024 * 1024
DEST = pc.DEST

env = pc.tool_env()


def md5_local(path):
    r = subprocess.run(["certutil", "-hashfile", path, "MD5"],
                       capture_output=True, text=True).stdout
    for l in r.splitlines():
        v = l.strip().replace(" ", "")
        if len(v) == 32:
            return v.lower()
    return ""


def main():
    pc.ensure_file(os.path.join(SRC, "big.spe"), SIZE)
    pc.write_list(os.path.join(SRC, ".fl.txt"), ["big.spe"])
    pc.clear_remote()

    base = list(pc.BASE_ARGS) + ["--info=progress2", "-e", pc.RSH, "./", DEST]

    # 1) 启动传输，3 秒后硬杀
    log("== 第 1 步：传输 3 秒后强杀 ==")
    p = subprocess.Popen([RSYNC] + base, cwd=SRC, env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    p.kill()
    p.wait()
    time.sleep(1)

    listing = pc.sh_out("ls -la %s/ 2>&1" % pc.REMOTE_DIR)
    log("接收端状态:\n" + listing)

    real = pc.sh_out("test -f %s/big.spe && echo EXISTS || echo ABSENT" % pc.REMOTE_DIR)
    partial = pc.sh_out("find %s/.srt-partial -type f 2>/dev/null | wc -l" % pc.REMOTE_DIR)
    log("真名文件: %s   .srt-partial 文件数: %s" % (real, partial))
    ok1 = (real == "ABSENT")
    log("%s 中断后真名未落盘（README 承诺）" % ("PASS" if ok1 else "FAIL"))
    ok2 = (partial != "0")
    log("%s 半截确实留在 .srt-partial（%s 个文件）" % ("PASS" if ok2 else "FAIL", partial))

    # 2) 再传一次
    log("== 第 2 步：重新同步（应为续传/重传） ==")
    t0 = time.time()
    r2 = subprocess.run([RSYNC] + base, cwd=SRC, env=env,
                        capture_output=True, timeout=3600)
    dt = time.time() - t0
    ok3 = (r2.returncode == 0)
    log("%s 重传 rc=%d %.1fs" % ("PASS" if ok3 else "FAIL", r2.returncode, dt))
    if not ok3 and r2.stderr:
        log("    stderr: " + r2.stderr.decode("utf-8", "replace")[:300])

    # 3) 大小 + md5 校验
    sz_remote = pc.sh_out("stat -c %%s %s/big.spe 2>/dev/null" % pc.REMOTE_DIR)
    sz_local = str(os.path.getsize(os.path.join(SRC, "big.spe")))
    ok4 = (sz_local == sz_remote)
    log("%s 大小：本地=%s 远端=%s" % ("PASS" if ok4 else "FAIL", sz_local, sz_remote or "-"))

    log("计算 md5（大文件，稍等）...")
    lm = md5_local(os.path.join(SRC, "big.spe"))
    rm = pc.sh_out("md5sum %s/big.spe 2>/dev/null" % pc.REMOTE_DIR).split()
    ok5 = bool(lm) and bool(rm) and lm == rm[0].lower()
    log("%s md5：本地=%s 远端=%s" % ("PASS" if ok5 else "FAIL", lm or "?",
                                     rm[0] if rm else "?"))

    # 4) 残留
    left = pc.sh_out("find %s/.srt-partial -type f 2>/dev/null | wc -l" % pc.REMOTE_DIR)
    ok6 = (left == "0")
    log("%s 传输完成后 .srt-partial 残留=%s（应为 0）" % ("PASS" if ok6 else "FAIL", left))

    allok = all([ok1, ok2, ok3, ok4, ok5, ok6])
    log("== %s ==" % ("全部通过" if allok else "存在失败项"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
