#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""远端目标路径边界：中文 / 空格 / 括号 / 0 字节文件 / 深层子目录。

产品形态：--protect-args 让远端路径不过远端 shell（应能正确处理中文空格）；
本测试用产品同款 argv 验证远端落点正确。

每个用例前先清空该远端目录 —— 否则上一轮遗留文件会让"文件数"对不上
（第一版就因为远端目录没清，把上一轮的 .srt-partial 数进去，报了假 FAIL）。
"""
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe_common as pc

SRC = os.path.join(pc.STAGE, "remoteedge")
LOG = "remote-path-edge.log"
RSYNC = pc.rsync_bin()
log = pc.logger(LOG)

env = pc.tool_env()


def prep():
    shutil.rmtree(SRC, ignore_errors=True)
    os.makedirs(os.path.join(SRC, "a"), exist_ok=True)
    open(os.path.join(SRC, "normal.spe"), "wb").write(b"hello")
    open(os.path.join(SRC, "zero.spe"), "wb").write(b"")          # 0 字节
    open(os.path.join(SRC, "utf8 中文.spe"), "wb").write(b"cn")
    files = ["normal.spe", "zero.spe", "utf8 中文.spe"]
    pc.write_list(os.path.join(SRC, ".fl.txt"), files)
    return files


def main():
    files = prep()
    cases = [
        ("中文目录", pc.REMOTE_DIR + "/中文目录"),
        ("含空格目录", pc.REMOTE_DIR + "/with space dir"),
        ("括号目录", pc.REMOTE_DIR + "/dir(paren)"),
        ("深层嵌套", pc.REMOTE_DIR + "/a/b/c/d"),
    ]
    fails = 0
    for label, remote in cases:
        # 先清空该目录再建，保证每次落点统计从零开始
        pc.sh_out("rm -rf '%s'" % remote)
        pc.sh_out("mkdir -p '%s' && chown -R %s:%s %s"
                  % (remote, pc.USER, pc.USER, pc.REMOTE_DIR))
        args = list(pc.BASE_ARGS) + [
            "--checksum", "-e", pc.RSH, "./",
            "%s@%s:%s/" % (pc.USER, pc.HOST, remote)]
        r = subprocess.run([RSYNC] + args, cwd=SRC, env=env,
                           capture_output=True, timeout=300)
        # 核对落点：文件集合必须与源完全一致（不多不少）
        listed = pc.sh_out("ls -A1 '%s' 2>/dev/null | sort" % remote)
        got = [x for x in listed.splitlines() if x and not x.startswith(".srt-partial")]
        zsz = pc.sh_out("stat -c %%s '%s/zero.spe' 2>/dev/null" % remote)
        csz = pc.sh_out("stat -c %%s '%s/utf8 中文.spe' 2>/dev/null" % remote)
        ok = (r.returncode == 0
              and sorted(got) == sorted(files)
              and zsz == "0"
              and csz == "2")
        if not ok:
            fails += 1
        log("%s %-12s rc=%d 收到=%s zero.spe大小=%s 中文名大小=%s"
            % ("PASS" if ok else "FAIL", label, r.returncode,
               ",".join(got) or "(空)", zsz or "-", csz or "-"))
        if not ok and r.stderr:
            log("    stderr: " + r.stderr.decode("utf-8", "replace")[:300])

    log("== 全部用例完成：%d/%d 通过 ==" % (len(cases) - fails, len(cases)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
