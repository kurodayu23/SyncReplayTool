#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""端到端 argv 抓取：产品**实际**用什么参数调起 rsync。

为什么必须做这一层：
  · --selfcheck 里的 rsync-args-assembly 判据验的是 buildRsyncArgs() 的返回值
    —— 即"代码打算传什么"；
  · 这里验的是"进程真的被这么调起来了" —— 因为产品是 m_proc->start("rsync", args)，
    中间任何一层都可能改写参数（漏传、顺序变、路径被拼错）。

做法：
  1) 把编译好的 exe 复制到一个平铺目录（和 Windows 交付包同构），
     把其中的 rsync.exe 换成一个**记录 argv 的包装器**（C#，见 ArgvDump.cs），
     包装器记录完再把同样的 argv 转给真的 rsync.exe，所以同步照常跑通；
  2) 用 --selfcheck-secrets 驱动产品跑一轮**真实同步**（接收端在容器里）；
  3) 核对记录下来的 argv：必备选项在、不该在的选项不在、口令没进命令行、
     --files-from 用的是相对路径且清单真在工作目录里、远端真收到了文件；
  4) 反向锁：把记录下来的 argv 篡改成"少一个 --protect-args"、"命令行里塞口令"、
     "抽掉 --files-from"三种，检查器必须报红 —— 证明上面那些判据不是空转。

依赖：.NET Framework 的 csc.exe（Windows 自带）；路径可用 SRT_* 环境变量覆盖。
"""
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import probe_common as pc

HERE = os.path.dirname(os.path.abspath(__file__))
# app 目录（exe + DLL + 记录器）**必须建在 git 仓库外面**。
#
# 踩过的坑（2026-11-02 实测）：app 目录放在仓库里
# （scripts/tests/argv_capture/app）时，产品跑 --selfcheck-secrets 会稳定失败：
#   INFO selfcheck-src dir=C:/Users/.../Temp/srt-sc-src-<pid> mkpath=0 probeWritten=0
#   FAIL no-files-scanned ...
# 同一个二进制、同一条路径、同一个用户，放在
# ai-workspace\SyncReplayTool\argv-app 下就 mkpath=1、正常通过。
# 也就是说：进程从**仓库目录内**启动时，Qt 在 %TEMP% 建目录这一步会失败，
# 探针文件因此没落地、扫不到文件、口令链路一次都没跑。
# 触发条件的确切机制还没查清（不是联接点、不是 Defender 拦截、仓库与
# ai-workspace 同盘同卷），但结论已经稳定复现多次 —— 台架躲开这个位置即可。
APPDIR = os.environ.get("SRT_ARGV_APPDIR",
                         os.path.join(pc.WORKROOT, "argv-app"))
DUMPF = os.path.join(HERE, "argv-dump.txt")
LOGF = os.path.join(pc.LOGDIR, "argv-capture.log")

SRC_EXE = os.environ.get(
    "SRT_EXE",
    r"D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\build-Release\release\SyncReplayTool.exe")
CSC = os.environ.get(
    "SRT_CSC", r"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe")

HOST = pc.HOST
PORT = pc.PORT
USER = pc.USER
PASS = pc.PASS
REMOTE = pc.REMOTE_DIR + "/argv"

# 必须出现在 argv 里的选项（生产装配的固定部分）
REQUIRED = ["-a", "--no-o", "--no-g", "--partial", "--partial-dir=.srt-partial",
            "--protect-args", "--from0"]
# 正常同步模式下**不允许**出现的选项
FORBIDDEN = ["--ignore-existing", "--bwlimit=0", "--password", "--rsync-password"]

log_f = None


def log(m):
    line = "%s %s" % (time.strftime("%H:%M:%S"), m)
    print(line, flush=True)
    if log_f:
        log_f.write(line + "\n")
        log_f.flush()


def sh(cmd):
    return pc.sh(cmd)


def _force_rmtree(path):
    """删干净：先去掉只读属性，否则上次 copy 带过来的只读位会让下次覆盖失败。"""
    if not os.path.isdir(path):
        return
    for root, dirs, files in os.walk(path):
        for f in files:
            try:
                os.chmod(os.path.join(root, f), 0o666)
            except Exception:
                pass
    shutil.rmtree(path, ignore_errors=True)


def build_app_dir():
    _force_rmtree(APPDIR)
    os.makedirs(APPDIR, exist_ok=True)
    src_dir = os.path.dirname(SRC_EXE)
    # 平铺结构：exe + 依赖 DLL + ssh 等工具，和 Windows 交付包同构。
    # rsync.exe 不复制 —— 那个位置放记录器。
    for name in os.listdir(src_dir):
        p = os.path.join(src_dir, name)
        if os.path.isdir(p):
            if name in ("platforms", "iconengines", "imageformats"):
                shutil.copytree(p, os.path.join(APPDIR, name))
            continue
        if name.lower().endswith(".dll") or name.lower() in (
                "ssh.exe", "scp.exe", "ssh-keyscan.exe", "ssh-keygen.exe", "run.cmd"):
            shutil.copyfile(p, os.path.join(APPDIR, name))
    shutil.copyfile(SRC_EXE, os.path.join(APPDIR, "SyncReplayTool.exe"))
    # 编译记录器
    r = subprocess.run([CSC, "/nologo", "/target:exe",
                        "/out:" + os.path.join(APPDIR, "rsync.exe"),
                        os.path.join(HERE, "ArgvDump.cs")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        log("FAIL 记录器编译失败: " + r.stdout + r.stderr)
        return False
    log("app 目录就绪: %s" % APPDIR)
    return True


def prepare_isolated_config():
    """给子进程一份**隔离**的配置，不碰用户真实配置。

    ★ 为什么必须隔离（2026-11-02 实测踩到）：
      产品的配置在 %APPDATA%\\AP\\SyncReplayTool.ini。用户机器上这条配置里
      autoSyncEnabled=true —— 于是自检进程一起来，自动同步调度器就开始按
      用户配置里的目录扫文件、抢走 SyncPanel 的文件列表，
      自检自己的 probe.spe 反而被挤掉，报「no-files-scanned」空转 60 秒。
      更糟的是自检结束后，它把测试用的 host/port/remoteRoot/localRoot
      **写进了用户的真实配置**（用户的连接信息被覆盖成 127.0.0.1:2222 和
      一个已经不存在的临时目录）。

      产品自带 SRT_CONFIG_DIR 开关（ConfigStore::configDirOverride），
      就是为这种场景准备的 —— 设一个绝对路径，配置读写全部落到那儿。
      注意不能用 APPDATA 环境变量来隔离：QSettings 走 SHGetFolderPath，
      读注册表不看环境变量（源码里有同样的说明）。

    隔离目录里预置一份最小配置：一条 spe 后缀规则（probe.spe 要能被扫到），
    自动同步关掉。
    """
    cfg_dir = os.path.join(pc.STAGE, "argv-capture-config")
    os.makedirs(cfg_dir, exist_ok=True)
    ini = os.path.join(cfg_dir, "SyncReplayTool.ini")
    with open(ini, "w", encoding="utf-8") as f:
        f.write("[app]\n")
        f.write("rememberSettings=true\n")
        f.write("\n[sync]\n")
        f.write("autoSyncEnabled=false\n")
        f.write("onlyNewFiles=false\n")
        f.write("autoSyncIntervalSec=30\n")
        f.write("bandwidthLimitMBps=0\n")
        f.write("\n[syncRules]\n")
        f.write("size=1\n")
        f.write("1\\suffix=spe\n")
        f.write("1\\remoteSubDir=\n")
        f.write("\n[target]\n")
        f.write("host=\n")
        f.write("port=22\n")
        f.write("user=\n")
        f.write("remoteRoot=\n")
        f.write("localRoot=\n")
        f.write("autoAcceptHostKey=true\n")
    return cfg_dir


def run_product():
    if os.path.exists(DUMPF):
        os.remove(DUMPF)
    sh("rm -rf '%s' && mkdir -p '%s' && chown -R %s:%s %s"
       % (REMOTE, REMOTE, USER, USER, pc.REMOTE_DIR))
    env = dict(os.environ)
    env["PATH"] = APPDIR + ";" + pc.TOOLS + ";" + env.get("PATH", "")
    env["SRT_CONFIG_DIR"] = prepare_isolated_config()
    env["SRT_ARGV_DUMP"] = DUMPF
    env["SRT_REAL_RSYNC"] = os.path.join(pc.TOOLS, "rsync.exe")
    env["SRT_SC_HOST"] = HOST
    env["SRT_SC_PORT"] = PORT
    env["SRT_SC_USER"] = USER
    env["SRT_SC_PASS"] = PASS
    env["SRT_SC_REMOTE"] = REMOTE
    env["SRT_SC_REPORT"] = os.path.join(HERE, "secrets-report.txt")
    t0 = time.time()
    try:
        r = subprocess.run([os.path.join(APPDIR, "SyncReplayTool.exe"),
                            "--selfcheck-secrets", "1"],
                           env=env, capture_output=True, timeout=300,
                           cwd=APPDIR)
    except subprocess.TimeoutExpired:
        log("FAIL 产品 300 秒未退出")
        return None
    out = (r.stdout or b"").decode("utf-8", "replace") + \
          (r.stderr or b"").decode("utf-8", "replace")
    with open(os.path.join(pc.LOGDIR, "argv-capture-product.log"), "w",
              encoding="utf-8") as f:
        f.write(out)
    log("产品退出码=%d 用时=%.1fs" % (r.returncode, time.time() - t0))
    for line in out.splitlines():
        if line.strip():
            log("  | " + line.strip())
    return out


def parse_dump():
    if not os.path.exists(DUMPF):
        return None
    with open(DUMPF, encoding="utf-8") as f:
        text = f.read()
    blocks = text.split("=== INVOCATION ===")
    if len(blocks) < 2:
        return None
    argv = []
    cwd = ""
    cwd_files = []
    for line in blocks[1].strip().splitlines():
        m = re.match(r"^\[(\d+)\] (.*)$", line)
        if m:
            argv.append(m.group(2))
        elif line.startswith("CWD_FILE="):
            cwd_files.append(line[9:])
        elif line.startswith("CWD="):
            cwd = line[4:]
    return argv, cwd, cwd_files


def _norm(p):
    return os.path.normcase(os.path.normpath(p)) if p else ""


def parse_selfcheck_dir(out):
    """取产品自报的探针根（INFO selfcheck-src ... dir=<path>）。"""
    m = re.search(r"selfcheck-src\b[^\r\n]*?\bdir=(\S+)", out or "")
    return m.group(1) if m else ""


def check_argv(argv, cwd, remote_files, cwd_files, expect_cwd=""):
    """返回 (ok, [verdict lines])。判定逻辑单独成函数，反向锁直接喂它假 argv。"""
    v = []

    def verdict(ok, text):
        v.append("%s %s" % ("PASS" if ok else "FAIL", text))
        return ok

    ok = True
    for opt in REQUIRED:
        ok &= verdict(opt in argv, "argv 含 %s" % opt)

    fl = [a for a in argv if a.startswith("--files-from=")]
    ok &= verdict(len(fl) == 1, "argv 恰好一个 --files-from（实际 %d 个）" % len(fl))
    if fl:
        val = fl[0].split("=", 1)[1]
        rel = verdict(not re.match(r"^[A-Za-z]:", val) and "/" not in val,
                      "--files-from 用相对路径（%s）" % val)
        ok &= rel
        if rel:
            # 清单文件是产品写在**本地根目录**里的临时文件，rsync 起跑时必须就在
            # 工作目录中（否则 rsync 找不到它）；传完产品会把它删掉。
            ok &= verdict(val in cwd_files,
                          "清单文件在 rsync 起跑时就在工作目录里（%s）" % val)

    e_idx = [i for i, a in enumerate(argv) if a == "-e"]
    ok &= verdict(len(e_idx) == 1, "argv 恰好一个 -e（实际 %d 个）" % len(e_idx))
    if len(e_idx) == 1:
        i = e_idx[0]
        sshv = argv[i + 1] if i + 1 < len(argv) else ""
        ok &= verdict(sshv.startswith("ssh ") or " ssh " in sshv,
                      "-e 的值是 ssh 命令（%r）" % sshv)
        ok &= verdict(("-p %s" % PORT) in sshv, "-e 里端口是 %s" % PORT)
        # DISPLAY 只在 Linux 形态（env DISPLAY=:0 ssh ...）里出现：
        # Windows 上没有 env 可用，产品刻意退回裸 "ssh"，
        # 此时 DISPLAY 无意义（Windows OpenSSH 不看它）。
        if sshv.startswith("ssh "):
            ok &= verdict("env " not in sshv,
                          "Windows 形态：-e 是裸 ssh（无 env 包装）")
        else:
            ok &= verdict("DISPLAY=" in sshv,
                          "Linux 形态：-e 里带 DISPLAY（老 ssh 靠它才肯用 askpass）")

    dest = argv[-1] if argv else ""
    ok &= verdict(dest.rstrip("/") == ("%s@%s:%s" % (USER, HOST, REMOTE)).rstrip("/"),
                  "远端目标正确（%s）" % dest)

    for opt in FORBIDDEN:
        ok &= verdict(not any(opt in a for a in argv), "argv 不含 %s" % opt)

    leaked = [a for a in argv if PASS in a]
    ok &= verdict(not leaked, "命令行里没有明文口令")

    # 产品 2026-11-02 起：探针根建在 %TEMP%\srt-sc-src-<pid>，**跑完自己删掉**。
    # 所以判据不能要求"目录现在还在"，而要要求"cwd 就是产品自报的那个探针根"：
    #   还在        = 还没清理（也算对，判据不该因此报红）
    #   已不在      = 清理过了（**期望行为**）
    #   与自报不一致 = 错（反查到了别处的目录）
    if expect_cwd:
        cwd_ok = bool(cwd) and _norm(cwd) == _norm(expect_cwd)
    else:
        cwd_ok = bool(cwd) and re.search(r"srt-sc-src-\d+$", cwd) is not None
    ok &= verdict(cwd_ok, "工作目录是产品自报的探针根（%s）" % cwd)

    ok &= verdict(len(remote_files) >= 1,
                  "接收端真收到文件（%s）" % (",".join(remote_files) or "-"))
    return ok, v


def real_config_fingerprint():
    """用户真实配置的指纹（mtime + 大小 + sha256）。跑完必须一模一样。"""
    ini = os.path.join(os.environ.get("APPDATA", ""), "AP", "SyncReplayTool.ini")
    if not os.path.exists(ini):
        return None
    import hashlib
    st = os.stat(ini)
    with open(ini, "rb") as f:
        h = hashlib.sha256(f.read()).hexdigest()
    return (ini, st.st_mtime, st.st_size, h)


def main():
    global log_f
    # Windows 控制台默认 GBK，产品的输出里有 UTF-8 字符，直接 print 会抛
    # UnicodeEncodeError 把测试台自己搞崩（看起来像产品失败，其实是台架的锅）。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    os.makedirs(pc.LOGDIR, exist_ok=True)
    log_f = open(LOGF, "w", encoding="utf-8")
    cfg_before = real_config_fingerprint()

    if not os.path.exists(SRC_EXE):
        log("FAIL 找不到编译产物 %s" % SRC_EXE)
        return 1
    if not os.path.exists(CSC):
        log("FAIL 找不到 csc.exe（.NET Framework 编译器）%s" % CSC)
        return 1
    if not build_app_dir():
        return 1

    out = run_product()
    if out is None:
        return 1

    parsed = parse_dump()
    if parsed is None:
        log("FAIL 没有抓到 argv（记录器没被调用）")
        return 1
    argv, cwd, cwd_files = parsed
    expect_cwd = parse_selfcheck_dir(out)
    log("抓到 argv（%d 项，cwd=%s）:" % (len(argv), cwd))
    if expect_cwd:
        log("产品自报探针根：%s（现存=%s）" % (expect_cwd, os.path.isdir(expect_cwd)))
    for i, a in enumerate(argv):
        log("  [%d] %s" % (i, a))

    remote_files = [x for x in sh("ls -A1 '%s' 2>/dev/null" % REMOTE).stdout.split()
                    if x and not x.startswith(".srt-partial")]
    ok, verdicts = check_argv(argv, cwd, remote_files, cwd_files, expect_cwd)
    for line in verdicts:
        log(line)

    # 临时清单传完必须被产品删掉（不能留在用户的源目录里）
    fl = [a for a in argv if a.startswith("--files-from=")]
    if fl:
        list_path = os.path.join(cwd, fl[0].split("=", 1)[1])
        gone = not os.path.exists(list_path)
        log("%s 同步结束后临时清单已清理（%s）" % ("PASS" if gone else "FAIL", list_path))
        ok &= gone

    # ---- 反向锁：检查器本身不能是空转的 ----
    neg1 = list(argv)
    if "--protect-args" in neg1:
        neg1.remove("--protect-args")
    nok1, _ = check_argv(neg1, cwd, remote_files, cwd_files, expect_cwd)
    log("%s 反向锁①：抽掉 --protect-args 后检查器必须报红" % ("PASS" if not nok1 else "FAIL"))
    ok &= not nok1

    neg2 = list(argv) + ["--password=" + PASS]
    nok2, _ = check_argv(neg2, cwd, remote_files, cwd_files, expect_cwd)
    log("%s 反向锁②：命令行里塞进口令后检查器必须报红" % ("PASS" if not nok2 else "FAIL"))
    ok &= not nok2

    neg3 = [a for a in argv if not a.startswith("--files-from=")]
    nok3, _ = check_argv(neg3, cwd, remote_files, cwd_files, expect_cwd)
    log("%s 反向锁③：抽掉 --files-from 后检查器必须报红" % ("PASS" if not nok3 else "FAIL"))
    ok &= not nok3

    # 反向锁④：把工作目录换成别的目录，这条判据必须报红（证明它不是恒真的）
    neg4_cwd = os.path.join(pc.WORKROOT, "argv-app")
    nok4, _ = check_argv(argv, neg4_cwd, remote_files, cwd_files, expect_cwd)
    log("%s 反向锁④：换掉工作目录后检查器必须报红" % ("PASS" if not nok4 else "FAIL"))
    ok &= not nok4

    clean = "SECRETS_ALL_CLEAN" in (out or "")
    log("%s 产品自检报告口令无残留（SECRETS_ALL_CLEAN）" % ("PASS" if clean else "FAIL"))
    ok &= clean

    # 隔离是否真的生效：用户真实配置必须一个字节都没变
    cfg_after = real_config_fingerprint()
    if cfg_before is None:
        untouched = (cfg_after is None)
        log("%s 用户真实配置没被探针改动（跑前就不存在）" % ("PASS" if untouched else "FAIL"))
    else:
        untouched = (cfg_before == cfg_after)
        log("%s 用户真实配置没被探针改动（%s）"
            % ("PASS" if untouched else "FAIL", cfg_before[0]))
    ok &= untouched

    log("== %s ==" % ("全部通过" if ok else "存在失败项"))
    log_f.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
