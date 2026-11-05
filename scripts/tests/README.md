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
