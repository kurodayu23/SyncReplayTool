# scripts/tests —— 无 GUI 测试台架

这些脚本驱动产品**真实的同步路径**，但不开窗口、不用鼠标。
原因：桌面被多个 Agent 共用，外来点击会让应用静默关闭（不是产品崩溃），
鼠标驱动的 GUI 测试因此不可靠。这里的每一项都走产品自带的测试钩子
（`--selfcheck-*` 用的同一批钩子），跑完能拿到机器可判定的结论。

## 目录

| 路径 | 作用 |
|---|---|
| `headless-probe/probe.cpp` | 无 GUI 探针：直接调 `SyncPanel` 的测试钩子，扫描 + 同步 + 打印行状态 |
| `headless-probe/probe.pro` | 探针工程（**链产品源码**，所以改完源码必须重编探针） |
| `headless-probe/build-probe.ps1` | 编译探针（VS2015 x64 + 钉版 SDK，镜像 `scripts/build-win.ps1`） |
| `e2e/*.ps1` | 端到端台架：真容器 + 真 rsync + 真私钥 |
| `e2e/gen_*.py` `tree_hash.py` `cmp_special.py` | 数据生成与逐字节比对 |

## 前置

- Docker 里有接收端镜像 `kylin-v11-sshd:latest`（`docker images` 能看到）。
- 密钥在 `<BuildRoot>\work\scale\keys\id_ed25519`（`.pub` 同目录）。
  首次没有就先跑一次 `scale-probe.ps1` 生成，或手工 `ssh-keygen`。
- `<BuildRoot>` 默认 `D:\AP\AP7000\AP7000-Build-Directories\SyncReplayTool`。

## 用法

```powershell
# 1) 改了产品源码 -> 先重编探针（不重编就是在测上一版）
powershell -ExecutionPolicy Bypass -File scripts\tests\headless-probe\build-probe.ps1

# 2) 跑某一项端到端
powershell -ExecutionPolicy Bypass -File scripts\tests\e2e\test-d1-probe.ps1
powershell -ExecutionPolicy Bypass -File scripts\tests\e2e\scale-probe.ps1 -Files 100000 -Port 2255
```

探针的用法（直接调 exe 时）：

```
probe --root <dir> [--host H --port P --user U --key K --remote R]
      [--rules a,b,c | --no-rules] [--only-new] [--sync] [--timeout S] [--dump-rows]
```

输出是机器可读的四段：`SCAN` / `SYNC` / `ROW` / `LOG`（LOG 是产品自己写的日志原文）。

## 规矩

- **`.ps1` 必须是纯 ASCII**。Windows PowerShell 5.1 读没有 BOM 的 `.ps1` 时按当前 ANSI
  代码页解码，中文字面量会把下一行整行吃掉（无声失败）或者直接报语法错。
  要匹配中文，用字符码构造：`-join ([char[]](0x4E0D, 0x5728))`。
  （确实必须写中文时，唯一的例外是**存成带 UTF-8 BOM** 的文件 ——
  `mutate2-probe.ps1` 就是这么存的，2026-11-07 踩过一次：缺 BOM 时脚本直接解析失败。
  但 BOM 会在某些编辑/转码流程里被悄悄丢掉，所以**默认一律走纯 ASCII**。）
- `.ps1` 里不要硬编码路径，用 `$PSScriptRoot`；产物目录用 `<BuildRoot>`，不要落进仓库。
- 容器里 `rm -rf` 会被拦；`docker rm -f` 对不存在的容器在 PS 5.1 下抛终止性错误，
  台架要包一层 `try/catch`。
- 看门狗下限 180 秒（`SRT_WATCHDOG_MS` 可覆盖）。**覆盖过头会把健康传输判成超时** ——
  3 万文件约 33 秒，测试里要用 90 秒级，别用 30 秒。
- rsync 的 `-z` 会压缩重复字节 ⇒ 造大数据必须用**随机字节**，否则限速 bite 不到。

## 现有用例

| 脚本 | 场景 | 判据 |
|---|---|---|
| `test-d1-probe.ps1` | 无后缀文件（README/Makefile）在自动识别规则下会不会静默漏传 | 被排除的文件必须**报出**、不计数、不标「已完成」 |
| `scale-probe.ps1` | 10 万文件首轮 + 增量轮 | 远端整树 sha256 与本地**逐位一致**；峰值内存 |
| `netcut-probe.ps1` | 传输中拔网线 → 网络恢复 | 第 1 轮诚实失败且无残留进程；恢复后第 2 轮能补齐 |
| `special-probe.ps1` | 特殊文件名（引号/emoji/中文/长名/前导横杠/元字符） | 逐字节一致 |
| `onlynew-probe.ps1` | 「仅同步新增」两轮 | 远端同名文件不覆盖，新增被传，文案诚实 |
| `mutate2-probe.ps1` | 传输中改源文件 | 判据不能说出「远端收到的是修改前的内容」这种假话 |
| `lock-probe.ps1` | 传输中的源文件被 rsync 独占锁定 | 报错可读、不丢文件 |
| `edge-probe.ps1` | 远端路径含中文+空格、本地路径含空格、空目录树、增量轮删除/改名 | 路径转义正确；空目录与"远端不删旧文件"都是**有文档的设计**，不是漏传 |
| `cancel-probe.ps1` | 传输中点「取消」→ 再跑一轮 | 真路径下**没有**半截文件（半截在 `.srt-partial/`）、无残留 rsync/ssh 进程、状态是「已取消」、**下一轮自动重传且 sha256 一致** |
| `diskfull-probe.ps1` | 接收端磁盘满（tmpfs 撑爆） | 必须判**失败**、归因到**接收端**（不是本机/网络）、给出"查磁盘空间"的可执行建议、点名未完成的文件 |
| `okmeans-arrived-probe.ps1` | **界面说完成 ⇒ 远端必须真的有那个文件**（最强的单条不变量） | 1 个 20MB 大文件（最新 ⇒ 先传）先把盘占满，后面 3 个 32 字节小文件根本发不出去。判据：计数分子 ≤ 实际到达数、**每一行绿色都必须在远端有同名同大小的文件**、那 3 个小文件不许是绿的、整轮判失败 |
| `concurrent-probe.ps1` | 两个客户端同时往**同一个**远端目录推数据 | 三段：① 互不干扰（不误报"远端目录被删除或替换"）② **目录真被换掉时仍然报**（保护没被削弱）③ 只清自己作用域的残留，别人的标记原封不动 |
