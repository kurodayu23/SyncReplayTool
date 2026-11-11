# SyncReplayTool 缺陷与风险台账

> 这份台账放在**仓库里并由 git 跟踪**，因为放在构建目录下的那份在一次大规模清理里
> 被当成"旧构建目录"删掉了（构建目录不归 git 管，删了就没了）。
> 台账只写**实测过的**结论，没测的一律写"未验证"。

最后更新：2026-11-11

---

## 一、缺陷（实测确认）

### D3（P1）远端同名文件被本地目录替换 → 静默丢数据

- 形态：接收端 `sub/conflict.dat` 是 12 字节**文件**，本机 `conflict.dat/` 是**目录**。
- 实测（2026-11-10，真实容器 + 真 rsync + 密钥认证）：
  远端文件被 unlink、变成目录、12 字节内容**没了**；产品状态仍是「已完成」、
  1/1 行全绿、日志里**没有任何**提示。
- DSH 的修复（提交 `34e7d72`）：传输前对"清单隐含的一级目录名"做只读 stat 探测
  （`[ -e ] && [ ! -d ]` ⇒ 打印 `SRT-COLLIDE:<path>`），解析点
  `RsyncRunner.cpp:4386`（`ensureRemoteDirs()`）。
- 修后实测：日志出现 `SRT-COLLIDE:/home/srtpw/srt-in/d3a/conflict.dat` 与
  ⚠「目标电脑上 … 原本是一个**文件**，不是目录。本次同步已按你的设置把它替换成了
  目录（原文件内容不再保留）…」。
- **结论：现状 = 从"完全静默"变成"有明确告警的静默丢失"。**
  数据仍然丢，状态仍然报「已完成」。源码注释明写「只加提示，不改行为」。
- 反向形态（远端是目录、本地是文件）产品**一直拦得住**：
  rsync code 23 → 报「目标电脑上已有同名目录」+ status 失败 + 远端数据完好。
  ⇒ 防护是**单边的**，与正向不一致。
- 建议：改成与反向一致的**阻断**（同名不同类型语义应对称；事后告知时文件已经没了）。
- 台架：`scripts/tests/e2e/remote-conflict-probe.ps1`
  - 默认口径 = "告知即通过"（exit 0）
  - `-RequireNoLoss` = "远端必须保住文件"（exit 1）

---

## 二、风险项（逐条实测后的状态）

| # | 风险项 | 状态 | 结论 |
|---|---|---|---|
| 1 | 认证后 askpass 脚本被删 | **实测，风险不成立** | 见下 §3 |
| 2 | 本地符号链接 / junction | **实测，不是缺陷** | 见下 §4 |
| 3 | 构建指纹闸门可被空构建骗过 | **仍成立** | 见下 §5 |
| 4 | 3 处失败出口只说「原因未识别」 | **仍成立** | 见下 §6 |
| 5 | 无 probe report path 时不发 `allFinished` | **仍成立** | 见下 §7 |
| 6 | 远端同名不同类型时远端文件被替换 | **成立（P1）** | 见 §1 |

---

## 三、#1 askpass 中途消失 —— 实测结论（2026-11-11）

台架：`scripts/tests/e2e/askpass-vanish-probe.ps1`（`-NoDelete` 是对照组）

| | 对照组（不删 helper） | 删除组 |
|---|---|---|
| 数据是否到达 | 是（31457280 B） | 是（31457280 B） |
| 状态 | 已完成 | 已完成 |
| helper 是否被重建 | **全程从未重建** | **全程从未重建** |
| 远端残留 `.srt-verify-*` | **0** | **1**（39 字节） |

机理（已定位到源码）：

- helper 整轮只创建一次（`RsyncRunner.cpp:3281` 测试连接、`:4088` 准备阶段），
  **从不重建**（对照组全程观察到 0 次重建）。
- 轮次里真正决定成败的两次 ssh —— 建远端目录（`:4317`）与 rsync 传输（`:4885`）——
  在删除动作发生前就已完成认证；rsync 那条连接在删除后仍保持着已认证状态。
- 删除之后只剩一次 ssh：轮次末尾的标记清理
  （`cleanupRemoteMarkers()`，`RsyncRunner.cpp:5968`）。它需要同一个 askpass
  helper，此时文件已经不在 ⇒ 认证失败 ⇒ **退出码没有被检查**
  （`:6006` 只 `waitForFinished(2000)`）⇒ 失败被静默忽略
  ⇒ 远端留下一个 39 字节的 `.srt-verify-*` 标记文件。

**结论**：不丢数据、不假成功、不影响下一轮（下一轮开头的"清旧标记"会把它清掉）。
唯一后果是**远端残留一个 39 字节的隐藏标记文件，产品一个字都不提**。

附带发现：那次清理 ssh 的退出码根本没被检查 —— 与本项目"失败必须留痕"的原则
不一致（建议：检查退出码，失败就记一行日志）。

---

## 四、#2 本地 junction —— 实测结论（2026-11-11）

台架：`scripts/tests/e2e/junction-probe.ps1`（新加）

Windows 没有 Developer Mode 时建不了真符号链接（要管理员），但 **junction 可以建**，
覆盖的正是会泄漏、会卡死的目录形态。三种形态：

- `extlink` → 指向数据根**外部**的目录（泄漏形态）
- `looplink` → 指回**祖先**目录（成环形态）
- `deadlink` → 指向**不存在**的路径（断链形态）

实测（真实容器 + 真 rsync + 密钥认证）：

```
files=1, ok=1, 已完成
LOG WARN 有 3 个符号链接没有同步（链接不是数据，远端拿到的是链接本身）
LOG SKIP 因是链接目录跳过：…/root/deadlink（链接不是数据）
LOG SKIP 因是链接目录跳过：…/root/extlink（链接不是数据）
LOG SKIP 因是链接目录跳过：…/root/looplink（链接不是数据）
远端：只有 j1/plain.dat
```

- 扫描**没有卡死**（1.3 秒完成）⇒ 成环形态被挡住了
- 清单里只有 `plain.dat`，外部文件**没进清单**
- 远端**只收到** `plain.dat` ⇒ 没有数据泄漏
- 三个链接**都计数、都有逐条说明** ⇒ 不是静默丢弃
- 对照组：同一个文件作为普通文件放在根里 → 会被列出并上传（`files=2`），
  证明上一条的"没到达"是因为 junction，不是因为后缀规则不匹配

**结论：Windows junction 的三种形态都不是缺陷。**
（文件级软链仍属"未验证"：本机建不出来。）

---

## 五、#3 构建指纹闸门（仍成立）

- `scripts/package-delivery.ps1:768-786` 的闸门只比 **提交号**。
- `build-win.ps1` **空构建也会重写** `build-stamp.txt`，所以
  `git-head=38dbc40 / git-tree=clean / built-at=15:39` 这套指纹可以在
  产品源码已经变了（12:53）的情况下照样写成 12:48 的 exe 的样子。
- `:414-420` 已经算出"最新源文件时间"，但**只打印不拦**。
- 建议：`newest src/ mtime > exe mtime ⇒ 拒绝出包`，
  并且 `exe sha256` 必须等于 stamp 里记录的 `exe-sha256`。

---

## 六、#4 三处「原因未识别」（仍成立）

`src/RsyncRunner.cpp`：

- `:3614` ssh-keygen 预检超时
- `:4872` rsync 起不来
- `:5410` 远端命令起不来

三处都只输出「原因未识别」，没有归因层、没有下一步建议。
另外 `:412` 附近 15 条预检判据目前**零归因**。

---

## 七、#5 无 probe report path 时不发 `allFinished`（仍成立）

该轮既不发 `allFinished` 也不报错，老探针会一直挂到 10 秒超时。

---

## 八、台架清单

全部在 `scripts/tests/e2e/`，用产品自己的测试钩子驱动真实同步路径（无窗口、无鼠标）。

| 台架 | 覆盖什么 |
|---|---|
| `remote-conflict-probe.ps1` | D3：远端文件被本地目录替换 |
| `junction-probe.ps1` | 本地 junction 三形态（外链 / 成环 / 断链） |
| `askpass-vanish-probe.ps1` | 认证后 askpass 被删 |
| `askpass-vanish-probe.ps1 -NoDelete` | 同上，对照组 |
| `cancel-probe.ps1` | 传输中取消 |
| `concurrent-probe.ps1` | 并发同步 |
| `diskfull-probe.ps1` | 磁盘满 |
| `edge-probe.ps1` | 边界文件名 |
| `lock-probe.ps1` | 文件被占用 |
| `mutate2-probe.ps1` | 传输中源文件被改 |
| `netcut-probe.ps1` | 网络中断 |
| `okmeans-arrived-probe.ps1` | 数据确实到达 |
| `onlynew-probe.ps1` | 只传新增 |
| `scale-probe.ps1` | 规模 / 私钥 |
| `special-probe.ps1` | 特殊文件名（tar.gz、全角点、`*`、无后缀） |
| `test-d1-probe.ps1` | D1 |
| `run-edge-cases.ps1` | 汇总跑法（台架缺失时判 FAIL 而不是 SKIP） |

> 注意：`scripts/tests/` 目录历史上被别的会话删过两次（`38dbc40`、`f0050ba`）。
> 若出现"台架缺失"，用 `git checkout a98958c -- scripts/tests` 取回。

---

## 九、验证结果（本台账每一条的来源）

- D3：2026-11-10 实跑 `remote-conflict-probe.ps1`，两种口径都跑过
- askpass：2026-11-11 实跑 `askpass-vanish-probe.ps1`（删除组 + `-NoDelete` 对照组）
- junction：2026-11-11 实跑 `junction-probe.ps1`（含对照组），7 项判据全 PASS
- §5~§7：2026-11-10 之前的复核轮次结论，未在本轮复测
