# -*- coding: utf-8 -*-
"""边界 / 负向用例的公共件（2026-11-02 收编进仓库）。

为什么收编进仓库：这些探针是**测试资产**，不是 AI 临时产物。
原先只活在 ai-workspace，用户把那个目录清空就全没了（上一轮的
scripts/run-all-tests.ps1 与 kylin-payload 已经因为同样的理由收编过一次）。

路径口径（全部可用环境变量覆盖，默认值与仓库里 run-all-tests.ps1 一致）：
  SRT_WORKROOT    工作根（日志/夹具）   默认 D:\\AP\\AP7000\\ai-workspace\\SyncReplayTool
  SRT_STAGE_DIR   夹具目录              默认 <WORKROOT>\\stage
  SRT_TOOLS_DIR   rsync/ssh 所在目录     默认 AP7000-Build-Directories\\...\\build-Release\\release
  SRT_CONTAINER   接收端容器名           默认 srt-e2e-recv
  SRT_REMOTE_HOST / SRT_REMOTE_PORT / SRT_REMOTE_USER / SRT_REMOTE_PASS

每个探针必须守三条规矩（run-edge-cases.ps1 会检查）：
  1) 幂等  —— 自己准备夹具、开跑前先清接收端；
  2) 明确  —— 每一个断言都打印一行 PASS/FAIL 判决；
  3) 诚实  —— 崩了的探针不会留下判决，runner 把「没判决」判成 FAIL，
               绝不把崩了当成绿的。

★ 踩过的坑：known_hosts 的落点。
  ssh 选项 UserKnownHostsFile=NUL 在**打包进来的 msys 版 ssh.exe** 上
  不是空设备 —— 它会在工作目录里真的创建一个名为 NUL 的文件（内容就是
  known_hosts）。产品源码里用的是 /dev/null（见 RsyncRunner::nullKnownHosts），
  这里的探针必须跟产品一致，否则每跑一次就在夹具目录里留垃圾文件。
"""
import os
import subprocess
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

WORKROOT = os.environ.get(
    "SRT_WORKROOT", r"D:\AP\AP7000\ai-workspace\SyncReplayTool")
STAGE = os.environ.get("SRT_STAGE_DIR", os.path.join(WORKROOT, "stage"))
TOOLS = os.environ.get(
    "SRT_TOOLS_DIR",
    r"D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool\build-Release\release")
LOGDIR = os.path.join(WORKROOT, "logs")

CONTAINER = os.environ.get("SRT_CONTAINER", "srt-e2e-recv")
HOST = os.environ.get("SRT_REMOTE_HOST", "127.0.0.1")
PORT = os.environ.get("SRT_REMOTE_PORT", "2222")
USER = os.environ.get("SRT_REMOTE_USER", "srtpw")
PASS = os.environ.get("SRT_REMOTE_PASS", "SrtTest2026x")
REMOTE_DIR = "/home/%s/srt-in" % USER

# 口令助记脚本（ssh 的 SSH_ASKPASS）：只往数据文件里写口令，脚本用 type 读，
# 不经过 shell 解析 —— 与产品 createAskpass() 的做法一致。
AP_BAT = os.path.join(STAGE, "ap.bat")

# 与产品 RsyncRunner::buildRsyncArgs() 一致的固定选项集
BASE_ARGS = ["-a", "--no-o", "--no-g", "--partial", "--partial-dir=.srt-partial",
              "--protect-args", "--from0", "--files-from=.fl.txt",
              "--out-format=%n"]

# /dev/null 而不是 NUL —— 见文件头「踩过的坑」
RSH = ("ssh -p %s -o PreferredAuthentications=password "
       "-o PubkeyAuthentication=no -o StrictHostKeyChecking=no "
       "-o UserKnownHostsFile=/dev/null -o NumberOfPasswordPrompts=1" % PORT)

DEST = "%s@%s:%s/" % (USER, HOST, REMOTE_DIR)


def _prepare_askpass():
    os.makedirs(STAGE, exist_ok=True)
    with open(AP_BAT, "w", encoding="ascii", errors="ignore") as f:
        f.write("@echo %s\n" % PASS)
    return AP_BAT


_prepare_askpass()


def tool_env():
    env = dict(os.environ)
    env["PATH"] = TOOLS + ";" + env.get("PATH", "")
    env["SSH_ASKPASS"] = AP_BAT
    env["SSH_ASKPASS_REQUIRE"] = "force"
    env["DISPLAY"] = ":0"
    return env


def rsync_bin():
    return os.path.join(TOOLS, "rsync.exe")


def sh(cmd):
    # 容器里说的是 UTF-8，Windows 本地是 GBK：用默认编码解会直接抛
    # UnicodeDecodeError（中文文件名必炸）。
    return subprocess.run(["docker", "exec", CONTAINER, "sh", "-c", cmd],
                          capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def sh_out(cmd):
    return sh(cmd).stdout.strip()


def clear_remote():
    """清空接收端目录（含隐藏文件）。"""
    sh_out("rm -rf '%s'/* '%s'/.[!.]* 2>/dev/null" % (REMOTE_DIR, REMOTE_DIR))


def ensure_dir(path):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    return path


def ensure_file(path, size_bytes, filler=b"\0"):
    """造一个正好 size_bytes 的文件；已经存在且大小对得上就复用。"""
    if os.path.exists(path) and os.path.getsize(path) == size_bytes:
        return path
    ensure_dir(path)
    reps = max(1, (4 * 1024 * 1024) // len(filler))
    chunk = filler * reps
    with open(path, "wb") as f:
        left = size_bytes
        while left > 0:
            n = min(len(chunk), left)
            f.write(chunk[:n])
            left -= n
    return path


def write_list(path, names):
    """rsync --files-from 清单：NUL 分隔，UTF-8 文件名。"""
    ensure_dir(path)
    with open(path, "wb") as f:
        for n in names:
            f.write(n.encode("utf-8") + b"\0")


def logger(name):
    os.makedirs(LOGDIR, exist_ok=True)
    path = os.path.join(LOGDIR, name)

    def log(msg):
        line = "%s %s" % (time.strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        with open(path, "a", encoding="utf-8") as f:
            f.write(line + "\n")

    return log
