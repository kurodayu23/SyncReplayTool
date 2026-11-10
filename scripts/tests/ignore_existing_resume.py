#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最关键的静默丢数据防线：--ignore-existing（仅同步新增文件）模式下中断。

风险链条：若半截文件以真名落盘 → 下次同步 --ignore-existing 看到同名文件就跳过
        → 远端永久残缺、界面还报"已完成"，且永远不会自愈。
产品用 --partial-dir=.srt-partial 规避。本测试验证它真的规避住了。

步骤：
  1) 大文件，--ignore-existing 传输 3 秒后强杀
  2) 断言：接收端**没有** big.spe 真名（半截只在 .srt-partial/）
  3) 再传一次（还是 --ignore-existing）
  4) 断言：真名存在、大小一致、md5 与源一致   <- 证明自愈
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_common as pc

SRC = os.path.join(pc.STAGE, "partialtest")
LOG = "ignore-existing-resume.log"
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

    # 产品「仅同步新增文件」口径
    base = list(pc.BASE_ARGS) + [
        "--ignore-existing", "--info=progress2",
        "-e", pc.RSH, "./", DEST]

    log("== 第 1 步：--ignore-existing 传输 3 秒后强杀 ==")
    p = subprocess.Popen([RSYNC] + base, cwd=SRC, env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    p.kill()
    p.wait()
    time.sleep(1)

    real = pc.sh_out("test -f %s/big.spe && echo EXISTS || echo ABSENT" % pc.REMOTE_DIR)
    partial = pc.sh_out("ls -l %s/.srt-partial/ 2>/dev/null | tail -2" % pc.REMOTE_DIR)
    log("真名: %s" % real)
    log(".srt-partial: %s" % (partial.replace("\n", " | ") or "(空)"))
    ok1 = (real == "ABSENT")
    log("%s 半截未落真名（--ignore-existing 下不会被误判为「远端已有」）"
        % ("PASS" if ok1 else "FAIL"))

    log("== 第 2 步：再跑一次 --ignore-existing（应重传并自愈） ==")
    t0 = time.time()
    r = subprocess.run([RSYNC] + base, cwd=SRC, env=env, capture_output=True, timeout=3600)
    dt = time.time() - t0
    ok2 = (r.returncode == 0)
    log("%s 重传 rc=%d %.1fs" % ("PASS" if ok2 else "FAIL", r.returncode, dt))
    if not ok2 and r.stderr:
        log("    stderr: " + r.stderr.decode("utf-8", "replace")[:300])

    sz = pc.sh_out("stat -c %%s %s/big.spe 2>/dev/null" % pc.REMOTE_DIR)
    szl = str(os.path.getsize(os.path.join(SRC, "big.spe")))
    ok3 = (sz == szl)
    log("%s 大小 本地=%s 远端=%s" % ("PASS" if ok3 else "FAIL", szl, sz or "-"))

    rm = pc.sh_out("md5sum %s/big.spe 2>/dev/null" % pc.REMOTE_DIR).split()
    lm = md5_local(os.path.join(SRC, "big.spe"))
    ok4 = bool(lm) and bool(rm) and lm == rm[0].lower()
    log("%s md5 本地=%s 远端=%s" % ("PASS" if ok4 else "FAIL", lm or "?",
                                    rm[0] if rm else "?"))

    left = pc.sh_out("find %s/.srt-partial -type f 2>/dev/null | wc -l" % pc.REMOTE_DIR)
    ok5 = (left == "0")
    log("%s .srt-partial 残留=%s" % ("PASS" if ok5 else "FAIL", left))

    allok = all([ok1, ok2, ok3, ok4, ok5])
    log("== %s ==  " % ("全部通过" if allok else "存在失败项"))
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
