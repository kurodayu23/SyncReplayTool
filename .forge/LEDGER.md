# 状态台账（LEDGER）

> 每轮收工前更新。上下文被压缩后，这里是唯一可信的记忆。

## 当前阶段

P4 测试矩阵 / P5 审查 —— 矩阵 21 项：通过 20 / 失败 0 / 未执行 1；p0=0 p1=0

## 计数

- p0: 0
- p1: 0

## 运行约束（当前）

- **用户在打游戏**：所有测试必须 headless、不开窗口、不吃 CPU
  - 产品一律加 `QT_QPA_PLATFORM=offscreen`
  - 虚拟机（VMware Kylin-V10）已停机 —— 它在 Emulation 模式下满占一个核
  - 重编译/打包这类重活推迟到用户结束游戏后再做

## 本轮已修的 P0/P1（都有修复前后实测对比）

| 编号 | 问题 | 提交 | 实测证据 |
|---|---|---|---|
| P0-1 | 目标机不可达后自动同步永久卡死 + 每秒起 3 个 rsync（`verifyRemoteDir` 三条跳过分支漏 `++m_groupIndex`） | a1e23d2 | 修复前 3 次/秒持续 5.5 分钟且 lastRunAt 冻结；修复后恢复即归零、lastRunAt 持续推进 |
| P0-2 | rsync 启动失败时 `onProcError` 不推进组号（同类死循环） | cf71aea | 用同一把尺子扫全部 `launchNext()` 调用点后补齐 |
| P1-1 | 自启动自愈失败原来静默（界面显示"已开启"但开机不启动） | 63c28b4 | 本机实测产品写注册表拿回错误码 5，而 pwsh/reg.exe/python 同参数成功 ⇒ 按进程的注册表防护；已改为进日志 |

## 任务

| 任务 | owner | 状态 | 判据 | 证据 | 下一检查点 |
|---|---|---|---|---|---|
| 纯逻辑自检 | 主代理 | 完成 | 79/0（Windows 与麒麟均 0 FAIL） | `--selfcheck` | — |
| GUI 离屏自检 | 主代理 | 完成 | 40/0 | `--selfcheck-gui` offscreen | — |
| 麒麟交付包 18 项 | 主代理 | 完成 | P=18 F=0 | `work\stage-k\kylin-full.sh` | — |
| 麒麟→麒麟真实同步 | 主代理 | 完成 | `SECRETS_ALL_CLEAN` + `round ok=1` | `work\stage-k\kylin-sync2.sh` | — |
| 远端目录矩阵 | 主代理 | 完成 | 10 场景落点正确、无越界 | `probes\dir_matrix.py` | — |
| 文件选择矩阵 | 主代理 | 完成 | 只传指定后缀、不多传 | `probes\pick_matrix.py` | — |
| 怪文件名矩阵 | 主代理 | 完成 | 26/27 一致（1 个是文件系统长度差异） | `probes\weird_names.py` | — |
| 带宽精度矩阵 | 主代理 | 完成 | 5 档均落在 ±25% 内 | `probes\deep_a_bw_modes.py` | — |
| 组合场景（限速+强杀+续传+多规则） | 主代理 | 完成 | 14 项断言全过 | `probes\deep_e_combo.py` | — |
| 自动同步暴力 10 项 | 子代理 4aae5d58 | 完成 | 报回完整报告并暴露 P0-1 | 子代理报告 | — |
| **无 GUI 端到端台架 6 项** | 后台作业 | **进行中** | 每项有机器可判定结论 | `work\logs\night-run.log` | 收作业后逐项判定 |
| 真实麒麟桌面 GUI 点击 | — | **未执行** | 需真实 X11 会话 | 见下方阻塞 | 用户结束游戏且虚拟机可启动时 |
| 设置项逐字段往返 | — | 待定 | 每字段写入→重启→读回 | 无（子代理两次都中断） | 可用无 GUI 探针 `--dump-rows` 补 |
| 最终打包 + 三门禁 | — | 待定 | 三门禁 PASS | 无 | 用户结束游戏后（打包占 CPU） |

## 阻塞

1. **真实麒麟桌面 GUI 无法测试** —— 矩阵第 17 行「未执行」的唯一原因：
   - VMware `Kylin-V10` 跑在 `CPU reset: soft (mode Emulation)`（纯软件模拟），
     单核持续满载、画面停在 720x400 纯黑，等 25 分钟仍无桌面
   - 容器内麒麟定制 Xorg 报 `xorg-server: Kylin kernel check failed`，直接拒绝启动
   - 容器不能联网（`1-archive-proxy.internal` 解析失败），装不了 Xvfb
   - **替代手段**：`QT_QPA_PLATFORM=offscreen` 下 `--selfcheck-gui`（真控件、真事件、40/0），
     能验控件逻辑与状态机，验不了真实 X11 渲染与窗口管理器交互
2. **本机注册表被按进程拦截**（不影响交付判定，已进日志）：
   产品写 `HKCU\...\Run\SyncReplayTool` 拿回错误码 5，而 pwsh/reg.exe/python/裸 Win32 同参数均成功。

## 已知限制（已写入 `已知限制与未验证项.md` 第五节 19–22 条）

- 长中文名：Windows 255 字符 vs 麒麟 255 字节 ⇒ 96 字中文名（280 字节）麒麟存不下，
  rsync 报 code 24，产品判失败但日志只有英文原文 → 建议客户文件名控制在 80 个中文字内
- 空目录不同步（rsync 语义，已写使用文档）
- 210 万文件峰值内存 1446.7 MB（距 1.5 GB 上限 53 MB）
- 远端可能残留 `.srt-verify-<pid>-<时间戳>` / `.<name>.XXXXXX`（仅下次完整同步时清）
- `QFile::setPermissions` 改不了 NTFS ACL（口令文件安全性仅靠目录私有性）
