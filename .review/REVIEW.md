# 代码审查报告 —— 离线交付 + 归因修正（2026-11-06）

diff-hash: 2c76d585a99a611adb864acf8544f4898713a1da
files: src/RsyncRunner.cpp, scripts/kylin-payload/安装.sh, scripts/kylin-payload/自检.sh, scripts/package-delivery.ps1, scripts/verify-kylin.sh, scripts/run-all-tests.ps1, scripts/build-rsync-static.sh(新增)
p0-unresolved: 0
p1-unresolved: 0
reviewed-at: 2026-11-06T16:40:00+08:00
conclusion: pass

---

## 一、这轮改了什么（为什么改）

客户明确要求「软件一定做成离线」。原来的实际缺口：

- 麒麟交付包**不自带 rsync 二进制**，依赖客户机器系统里的 rsync。
- 客户机器如果是精简安装 + 不能上外网，`sudo apt install rsync` 执行不了 ⇒ 同步功能整体不可用。
- 历史原因：包里曾有一份 rsync，但它是 Ubuntu 22.04 构建、要 `GLIBC_2.38`，而麒麟 V10 只有 2.23，**连启动都做不到**，所以旧打包脚本直接 `rm -rf libexec`。

本轮改动分三块：

1. **自带静态 rsync**：在 `kylin-v10-real`（glibc 2.23）容器里用 `gcc -static` 编 rsync 3.1.3，打进 `libexec/rsync`。静态链接后 `ldd` 报 `not a dynamic executable`，不吃目标机 glibc 版本。
2. **`安装.sh --with-rsync`**：rsync over ssh 要求**两端**都有 rsync，而接收端是被 sshd 拉起来的、按 login shell 的 PATH 找，看不到包目录。所以提供把包内静态版装到 `/usr/local/bin/rsync`（实测该目录在 login shell 默认 PATH 里）。
3. **归因修正**：中文 locale 的 bash 缺 rsync 时报 `bash: rsync: 未找到命令`，而原代码只认英文 `rsync: command not found` ⇒ 一条都不命中 ⇒ 判成网络层，客户被引导去查 ping/端口/防火墙，永远查不到真因。

## 二、审查方式

按 AGENTS.md 协议派了 3 名独立审查员（同一条消息并发），维度互斥：

| 审查员 | 维度 | 结论 |
|---|---|---|
| c22ba5c1 | C++ 归因逻辑（RsyncRunner.cpp / main.cpp） | 1 P1 + 2 P2 |
| 8f9663f8 | 打包流程与静态 rsync 集成（package-kylin-container.sh / package-delivery.ps1） | P0 级 3 项 + P1 2 项 + P2 3 项 |
| 1c45c208 | shell 脚本（安装.sh / 自检.sh） | 2 P1 + 6 P2 |

**异构交叉未做到**：`subagent` 工具没有 model 参数，无法指定让某个审查员换模型。这是工具限制，如实记录。

## 三、发现与处置（逐条）

### P1（已修）

**A1 · `kHardReceiverUtf8` 收了通用 ENOENT 文案，会把本机层判成接收端层**
- 位置：`src/RsyncRunner.cpp:497`（表项）
- 证据（审查员用项目 obj 链接探针实测）：
  - `rsync: [sender] Failed to exec ssh: 没有那个文件或目录 (2)` → **layer=3**（应为 0，同句英文版是 0）
  - `rsync: link_stat "/x.spe" failed: 没有那个文件或目录 (2)` → **layer=3**（应为 0）
- 这条还违反了仓库自定规则（`RsyncRunner.cpp:311-314`）：「不要顺着 `[receiver]` 去补 `failed: No such file or directory` 这种通用短语…补了就会把本机的问题甩给对面机器」。
- **处置**：删除该表项，改为一段说明为什么刻意不收（保留「未找到命令」/「找不到命令」，那是 bash 的 command-not-found 专用文案，ENOENT 不会产生它）。

**A2 · `verify-kylin.sh:202` 硬断言「包里不许有 libexec」，与新设计正面冲突**
- 位置：`scripts/verify-kylin.sh:202`（原件）→ 已重写
- 证据（审查员实跑）：新包得 `PASS=42 FAIL=1`，`exit 1`。若这条门禁在交付流水线里，等同 P0（包做出来了但门禁永远红）。
- **处置**：改成反向断言 —— 必须有 `libexec/rsync`、必须可执行、**必须静态链接**、且 `libexec/` 下不许有别的死重（旧的 GLIBC_2.38 二进制）。
- 复验：`===== 结果：PASS=45  FAIL=0 =====`

**A3 · `自检.sh:329-331` 旧文案与新增说明自相矛盾**
- 位置：`scripts/kylin-payload/自检.sh:329-331`
- 证据（审查员实跑，离线机场景）：同一屏四句话互相否定
  ```
  ✗ 系统里找不到 rsync —— 同步功能不可用
      请让系统管理员安装：sudo apt install rsync      ← 离线机执行不了
  △ 包内仍带着 libexec/…但它在麒麟 V10 上跑不起来     ← 旧文案（错）
  ✓ 包内启动脚本 bin/rsync：回落正常                  ← 它正是用 libexec 跑起来的
      说明：本包**附带**一份静态 rsync                 ← 新文案
  ```
- **处置**：改成「只有真的不可用时才 warn」，可用则 `ok "包内自带静态 rsync：可用（rsync 3.1.3）"`。

**A4 · `verify-kylin.sh` 另一条旧判据「自检不许出现『包内自带』」与 A3 冲突**
- 这是我修 A3 后**自己跑出来的新 FAIL**：`FAIL 自检仍然出现「包内自带」字样`
- **处置**：判据反向 —— 包里有 `libexec/rsync` 时，自检**必须**标出「包内自带静态 rsync」（不许隐瞒）；没有时出现该字样才算谎报。

**A5 · `安装.sh` 只认第一个参数，`--autostart --with-rsync` 静默丢弃后者**
- 位置：`scripts/kylin-payload/安装.sh:50-54`（`case "${1:-}" in`）
- 证据（审查员实测）：`sh 安装.sh --autostart --with-rsync` → **rc=0、自启装了、`/usr/local/bin/rsync` 没装、零提示**，随后同步照旧报 `remote command not found (127)`。
- **处置**：改成 `while [ $# -gt 0 ]` + `*)` 报错退出，未知参数不再无声消失。

**A6 · `package-delivery.ps1` 两种 Path 输入直接崩**
- 证据（审查员实测，两种都复现）：
  - `-StaticRsync ''` → `Cannot bind argument to parameter 'Path' because it is an empty string`（参数注释却写「传空字符串则跳过」）
  - `-StaticRsync 'rsync-static'`（相对名）→ `Split-Path -Parent` 返回空串 → `-v ":/srsync:ro"` → `docker: invalid spec: :/srsync:ro: empty section between colons`（`exit=125`）
- **处置**：一律 `Resolve-Path` 转绝对路径再取父目录；空串则整体跳过挂载与 `--static-rsync`。另外原实现把文件名硬编码成 `/srsync/rsync-static`，改为按实际文件名拼。
- 复验：空串 → `自带静态 rsync：未指定` + `rc=0`（包照常产出，但见 A8）；相对路径 → `libexec/rsync 已写入静态版（2904504 字节）` + `rc=0`。

**A7 · 静态 rsync 无构建脚本，构建不可复现**
- 证据：全仓库 grep 不到任何生成这份二进制的脚本/配方，默认路径在 `work\rsync-static\`（会被清理）。
- **处置**：新增 `scripts/build-rsync-static.sh`（带三条硬判据：`file` 必须说 statically linked、`ldd` 不许有 `=>`、必须能 `--version`）。
- **复验可复现性**：从零重新编一遍 → 产物 `statically linked / not a dynamic executable / 2,904,504 字节`，与现有那份 **SHA256 完全相同**（`E023354ED3425797…`），连 BuildID 都一致（`43d296e8…`）。

**A8 · 七段回归不会拦住「包里少了静态 rsync」**
- 原行为：`package-delivery.ps1` 打印「未指定（客户机将依赖系统 rsync）」然后照样 `rc=0` —— 包做出来了，但不满足离线要求。
- **处置**：在 `run-all-tests.ps1` 打包段加两条闸门：静态 rsync 必须存在、必须没有动态依赖，否则该段直接 `FAIL`。

### P2（已修）

**B1 · `安装.sh` 装完不停，污染 root 家目录**
- 证据（审查员实测）：`sudo sh 安装.sh --with-rsync` 之后继续走桌面图标分支，`mkdir -p /root/桌面` 并写图标，还淹没了 rsync 的安装结果。
- **处置**：装完 `exit 0`。

**B2 · `reportAttribution` 的 advice 侧没判层**（审查员 P2，针对我的中间版本）
- 我的短结论加了 `layer == LayerReceiver &&` 守卫，但 `specialAdvice` 侧没有，而注释声称「加了 layer 判断后…」——`specialAdvice` 没有 `layer` 形参，做不了这个判断，注释与代码不符。
- **处置**：把守卫放到**调用处**：`advice = (layer == LayerReceiver) ? specialAdvice(raw) : QString();`，并把注释改成如实描述。

### P2（记录，未修，附理由）

| 项 | 位置 | 为什么不修 |
|---|---|---|
| `stat -c%s` 空串相等导致幂等误判 | `安装.sh:75` | 审查员实测复现（258 字节假文件被判「已经是包里那一份」）。但麒麟两代 stat 正常（V10 GNU 8.25 / V11 9.4），需先让 stat 失败才触发，现网概率低。已记入未验证/低危清单 |
| `install` 缺失时无 `cp`+`chmod` 回退、不建父目录 | `安装.sh:79` | 麒麟两代都有 `/usr/bin/install`；缺失时 rc=1 且不留半成品，只是文案误报。低危 |
| `自检.sh` 用 `-x` 判 libexec，丢执行位时误报「包被裁剪过」 | `自检.sh:402` | 已在 A3 的改写里一并改善（改成 `-x` + 实跑，不可用时 warn 而非 bad） |
| bash 缺失时给假警报 | `安装.sh:83-88` | 假警报只会让运维多看一眼 `/usr/local/bin/rsync` 已就位的事实，不误导方向。低危 |
| 幂等判断硬编码 `3.1.3` | `安装.sh:76-77` | 换版本后只是每次都重装（幂等失效），不会出错。已在 `build-rsync-static.sh` 里用 `SRT_RSYNC_VERSION` 参数化，后续统一 |
| `ldd \| grep -q "=>"` 闸门对「非 ELF」fail-open | `package-kylin-container.sh:250` | 审查员实测：shell 脚本经 ldd 会输出 `not a dynamic executable` ⇒ 闸门 PASS（假通过）；`ldd` 缺失时同样 PASS。但同一位置**前一条**闸门 `"$PKG/libexec/rsync" --version` 会先拦下非可执行文件，所以实际不可达。已记入清单 |

### 审查期间的一个插曲（如实记录）

两名审查员都报告「审查期间 `src/RsyncRunner.cpp` 出现未提交改动，不是我改的」。那是**我**在同期按 A1 修表项。这说明并发审查与主 agent 改动会互相干扰 —— 审查员锁定的是 commit `ebba8de`，我的修复在其之后，两边结论我都逐条核对过，没有冲突。

## 四、复审与门禁（改完之后重跑，旧结果作废）

| 门禁 | 结果 |
|---|---|
| 构建 | PASS（exe 11:39:07） |
| 麒麟交叉编译 | PASS（二进制 11:39:34，晚于源码） |
| 打包 | PASS（Windows zip / 麒麟 tar.gz） |
| 纯逻辑自检 | **PASS=79 FAIL=0** |
| 源码扫描（中文字面量） | PASS 4 条全过 |
| GUI 交互 | **GUI_SUMMARY pass=40 fail=0** |
| 截图探针 | PASS |
| 真机同步 | PASS 2 轮真实传输、无明文口令残留 |
| 麒麟包容器验证 | **PASS=45 FAIL=0** |
| 交付门禁 | rc=0（三道闸门全绿） |

## 五、未验证项（如实列出，不要当作已确认）

1. **静态 rsync 在物理麒麟真机 glibc 2.23 上未实机验证** —— 容器是 `kylin-v10-real`（glibc 2.23），但仍是容器，不是物理机。
2. **kysec 逐文件授权未验证** —— 容器里没有安全中心。客户真机上 kysec 可能拦未经授权的可执行文件。
3. **麒麟 V10 SP2 / SP3 / V11 桌面版** —— 只验过 V10 SP1 与 V11 容器。
4. **真实客户数据规模** —— 全程合成数据 + 自建接收端。
5. **`安装.sh --with-rsync` 在真实 sudo 环境下未实机验证** —— 容器里是 root 直接跑。
6. **多显示器 / 真实 DPI / 真实显卡渲染** —— 容器是 Xvfb 软件渲染。

## 六、结论

**pass**。P0 = 0，P1 = 0。

这轮的核心价值不是"加了个 rsync"，而是 **A2/A3/A4 三条门禁冲突**：如果只加功能不改判据，交付包会处于「做出来了但门禁永远红」的状态，或者自检在客户最需要的离线场景下给出四句互相否定的结论。这三条都是审查员实跑抓出来的，不是我事先想到的。
