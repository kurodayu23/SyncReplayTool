#!/bin/sh
# 交付自检。客户机上一条命令就能知道"能不能跑、缺什么"。
#
# 为什么需要它：Linux 缺库时的默认表现是**双击闪一下就没了** ——
# 动态链接器把 "GLIBC_2.34 not found" 写到 stderr，而 GUI 程序没人看 stderr。
# 客户看到的是"软件打不开"，但拿不到任何原因，只能打电话问开发。
# 这个脚本把那些静默失败翻译成人话。
#
# 本脚本的第一原则：**只说真话**。
#   - ssh / rsync 报的是**实际在用的那一份**（系统自带 / 包内自带），带路径和版本；
#   - 任何一项没查过，就不写"环境满足"；
#   - --quiet 只留错误行，警告行不刷屏（双击启动时最烦的就是一屏重复路径）。

QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

say()  { [ "$QUIET" = "1" ] || echo "$@"; }
# warn 只提示、不置 FAIL。用于"这一项坏了但软件仍然能用"的情况。
# **受 --quiet 控制** —— 以前不受，于是双击启动先喷一屏重复路径。
warn() { [ "$QUIET" = "1" ] || echo "△ $1"; }
bad()  { echo "✗ $1"; FAIL=1; }
ok()   { [ "$QUIET" = "1" ] || echo "✓ $1"; }
FAIL=0

HERE=$(cd "$(dirname "$0")" && pwd)

# 文件名编码兜底（非 UTF-8 会话下中文名会全变成乱码方块）
#
# Qt 用**当前 locale 的字符集**解码文件名与插件路径。
# 会话 locale 是 POSIX/C 时，本包目录名「数据同步工具-麒麟-x86_64-v2.4」里的每一个非 ASCII 字节
# 都会变成 U+FFFD，Qt 于是找不到 platforms/libqoffscreen.so，直接 abort(134)。
#
# 实测（同一份包，只改 locale 这一个变量）：
#   env -u LANG -u LC_ALL sh 自检.sh          -> exit=1「环境不满足，软件无法在这台机器上运行」
#   LANG=C.UTF-8 LC_ALL=C.UTF-8 sh 自检.sh    -> exit=0「环境满足，可以启动」
# systemd / cron / SSH 这类会话**没有** locale，正好落进前一种。
#
# 调用方的 locale 已经是 UTF-8 就原样保留，不是才挑一个系统里真实存在的。
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *UTF-8*|*utf8*|*UTF8*|*Utf8*) ;;
    *)
        for _srt_loc in C.UTF-8 C.utf8 en_US.UTF-8 en_US.utf8 zh_CN.UTF-8 zh_CN.utf8; do
            if locale -a 2>/dev/null | grep -qx "$_srt_loc"; then
                LANG="$_srt_loc"; LC_ALL="$_srt_loc"
                export LANG LC_ALL
                break
            fi
        done
        unset _srt_loc
        ;;
esac

# ---- 临时文件目录 ----
# 原来硬写 /tmp：/tmp 不可写（精简系统、配额满、只读挂载）时，
# 重定向建不出来，脚本直接喷 "cannot create /tmp/...: Permission denied"，
# 而且「程序本体启动失败」那一项后面跟的日志是**空的** —— 运维拿不到任何信息。
# 这里按 XDG_RUNTIME_DIR → TMPDIR → /tmp → 包内 .srt-tmp 的顺序挑一个真能写的。
# 「包内 .srt-tmp」挪到最后一位：它一旦被建出来，卸载时就会作为「不属于包清单的残留」留下来。
_srt_tmpdir=""
for _srt_d in "${XDG_RUNTIME_DIR:-}" "${TMPDIR:-}" /tmp "$HERE/.srt-tmp"; do
    [ -n "$_srt_d" ] || continue
    if mkdir -p "$_srt_d" 2>/dev/null && [ -w "$_srt_d" ]; then
        _srt_tmpdir="$_srt_d"
        break
    fi
done
[ -n "$_srt_tmpdir" ] || _srt_tmpdir="/tmp"
say "=== 数据同步工具 · 运行环境自检 ==="
say ""

# 把一串以空白分隔的库名整理成"去重 + 去掉尾冒号 + 最多 3 个"的短句。
# 以前是把 ldd 的输出原样拼起来：同一个库重复十几次，一行就刷满整屏
#（客户看到的是"一屏报错"，其实只有一两个库真的缺）。
fmt_libs() {
    _fl_seen=""
    _fl_out=""
    _fl_n=0
    for _fl_one in $1; do
        _fl_one=${_fl_one%:}
        [ -n "$_fl_one" ] || continue
        case " $_fl_seen " in
            *" $_fl_one "*) continue ;;
        esac
        _fl_seen="$_fl_seen $_fl_one"
        _fl_n=$((_fl_n + 1))
        if [ "$_fl_n" -le 3 ]; then
            _fl_out="$_fl_out $_fl_one"
        fi
    done
    if [ "$_fl_n" -gt 3 ]; then
        _fl_out="$_fl_out 等 $_fl_n 个"
    fi
    printf '%s' "${_fl_out# }"
}

# 去掉 PATH 里本包 bin/ 的绝对路径，返回剩下的 PATH。
# 必须剔掉自己：run.sh 把 $HERE/bin 放在 PATH 最前面，
# 直接 command -v rsync 找到的是 wrapper 自己，不是系统那一份。
_path_without_self() {
    _pw_self=$(cd "$HERE/bin" 2>/dev/null && pwd)
    _pw_out=""
    _pw_sep=""
    _pw_ifs=$IFS
    IFS=:
    set -f
    for _pw_d in $PATH; do
        [ -n "$_pw_d" ] || continue
        _pw_real=$(cd "$_pw_d" 2>/dev/null && pwd)
        [ -n "$_pw_real" ] || _pw_real="$_pw_d"
        [ "$_pw_real" = "$_pw_self" ] && continue
        _pw_out="$_pw_out$_pw_sep$_pw_d"
        _pw_sep=:
    done
    set +f
    IFS=$_pw_ifs
    printf '%s' "$_pw_out"
}

# 找系统那一份的绝对路径；找不到返回非 0。
_find_system() {
    _fs_name=$1
    _fs_p=$(_path_without_self)
    if [ -n "$_fs_p" ]; then
        _fs_hit=$(PATH="$_fs_p" command -v "$_fs_name" 2>/dev/null)
        if [ -n "$_fs_hit" ]; then
            printf '%s' "$_fs_hit"
            return 0
        fi
    fi
    for _fs_d in /usr/local/bin /usr/bin /bin /usr/sbin /sbin; do
        if [ -x "$_fs_d/$_fs_name" ] && [ ! -d "$_fs_d/$_fs_name" ]; then
            printf '%s' "$_fs_d/$_fs_name"
            return 0
        fi
    done
    return 1
}

# 某个系统库在不在（不依赖 ldconfig，精简系统上也能用）。
_lib_exists() {
    for _le_d in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /lib64 /usr/lib /lib; do
        [ -e "$_le_d/$1" ] && return 0
    done
    return 1
}

# ---- 配置文件目录在哪 ----
# 产品用 QSettings(IniFormat, UserScope, "AP", "SyncReplayTool")，
# Linux 上落在 $XDG_CONFIG_HOME/AP/SyncReplayTool.ini（即 ~/.config/AP/）；
# 另有环境变量 SRT_CONFIG_DIR 可整体改目录。
# 交付手册里写的默认目录是 ~/.config/SyncReplayTool，
# 两个都算上：哪个真实存在就用哪个，免得判据和实际落盘位置对不上。
_srt_cfgdirs() {
    if [ -n "${SRT_CONFIG_DIR:-}" ] && [ "${SRT_CONFIG_DIR#/}" != "$SRT_CONFIG_DIR" ]; then
        printf '%s\n' "$SRT_CONFIG_DIR"
    else
        if [ -n "${XDG_CONFIG_HOME:-}" ]; then
            printf '%s\n' "$XDG_CONFIG_HOME/AP"
        else
            printf '%s\n' "${HOME:-/root}/.config/AP"
        fi
        printf '%s\n' "${HOME:-/root}/.config/SyncReplayTool"
    fi
}

# 找到配置文件就打印它的路径；找不到返回非 0。
_srt_inifile() {
    _sif_list="$_srt_tmpdir/.srt-cfgdirs.$$"
    _srt_cfgdirs > "$_sif_list" 2>/dev/null
    _sif_hit=""
    while IFS= read -r _sif_d; do
        [ -n "$_sif_d" ] || continue
        if [ -f "$_sif_d/SyncReplayTool.ini" ]; then
            _sif_hit="$_sif_d/SyncReplayTool.ini"
            break
        fi
    done < "$_sif_list"
    rm -f "$_sif_list" 2>/dev/null
    [ -n "$_sif_hit" ] || return 1
    printf '%s' "$_sif_hit"
}

# 从 ini 里读一个键（只认行首 key=，取第一个）。
_srt_ini() {
    _si_f="$1"; _si_k="$2"
    [ -f "$_si_f" ] || return 1
    sed -n "s/^${_si_k}=//p" "$_si_f" 2>/dev/null | head -1
}

# ---- 1. CPU 架构 ----
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)  ok "CPU 架构：$ARCH（x86_64）" ;;
    aarch64) bad "CPU 架构：$ARCH —— 本包是 x86_64 版，装错机器了" ;;
    *)       bad "CPU 架构：$ARCH —— 本包是 x86_64 版" ;;
esac

# ---- 2. glibc 版本 ----
# 本包的 **GUI** 要求 >= 2.17。ssh/rsync 用**系统自带**的那一份
#（随包那份要求 glibc 2.38，麒麟 V10 是 2.23，带上也跑不起来，已从包里删掉）。
# **musl 不是"太老"，是另一套 libc**（Alpine、某些精简系统）。
# 实测（alpine:3.20 容器）：`ldd --version` 不是 GNU 格式，取版本号得到空串，
# 旧写法会报成「glibc： —— 本包要求 >= 2.17，这台机器的系统太老」，
# 而真实原因是"这台机器根本不是 glibc"——两者的修复动作完全不同，不能混。
#
# 门槛的来源（readelf -V 实测，不是估的）：
#   SyncReplayTool 本体最高要 GLIBC_2.14；libQt5Core.so.5.9.6 要 **GLIBC_2.17**。
G=$(ldd --version 2>/dev/null | head -1 | grep -o '[0-9][0-9.]*$')
if [ -z "$G" ]; then
    if ldd --version 2>&1 | grep -qi musl; then
        bad "libc：musl —— 本包是 glibc 版，musl 系统（Alpine 等）跑不了"
        echo "    请换用 glibc 系统（麒麟 / 统信 / Ubuntu / Debian），或联系供应商索取 musl 版。"
    else
        warn "读不出 glibc 版本号，无法判断系统版本"
        echo "    请让系统管理员执行：ldd --version   （把输出发给技术支持）"
    fi
else
GM=$(echo "$G" | cut -d. -f1); Gn=$(echo "$G" | cut -d. -f2); Gn=${Gn:-0}
if [ "${GM:-0}" -gt 2 ] || { [ "$GM" = "2" ] && [ "$Gn" -ge 17 ]; }; then
    ok "glibc：$G（界面部分要求 >= 2.17）"
else
    bad "glibc：$G —— 本包要求 >= 2.17，这台机器的系统太老"
fi
fi

# ---- 3. 图形界面依赖的库（决定界面能不能打开）----
# 只查 lib/：离屏自检（第 7 项）也只用 $HERE/lib，范围必须一致。
# 以前这里把 lib/sshv2/ 一起查，于是麒麟 V10 上明明界面能正常渲染，
# 却因为自带的 ssh 用不了 2.38 的符号而报"环境不满足"，
# run.sh 跟着 exit —— 客户连界面都开不了。
MISS=""
LDD_ODD=0
for so in "$HERE"/lib/*.so*; do
    [ -e "$so" ] || continue
    real=$(readlink -f "$so" 2>/dev/null || echo "$so")
    _ldd_out=$(LD_LIBRARY_PATH="$HERE/lib" ldd "$real" 2>&1)
    # 只认**长得像库名**的缺失项（lib 开头、含 .so）。
    # 原来这里是 `awk '{print $1}'`，在 musl 系统上 ldd 整段是错误信息
    # （"Error loading shared library ... not found"），
    # 取 $1 就得到一个叫 "Error" 的**假库名**，界面上会出现
    # 「缺少以下系统库：Error」—— 部署人员根本不知道该装什么。
    out=$(printf '%s\n' "$_ldd_out" | grep 'not found' | awk '$1 ~ /^lib[^ ]*\.so/ {print $1}')
    if [ -z "$out" ] && printf '%s\n' "$_ldd_out" | grep -q 'not found'; then
        LDD_ODD=1
    fi
    [ -n "$out" ] && MISS="$MISS $out"
done
if [ "$LDD_ODD" = 1 ]; then
    warn "有系统库解析不了，但 ldd 没给出库名（多见于 musl / 精简系统）"
    echo "    请让系统管理员执行：LD_LIBRARY_PATH=\"$HERE/lib\" ldd \"$HERE/SyncReplayTool\""
fi
if [ -n "$MISS" ]; then
    bad "缺少以下系统库：$(fmt_libs "$MISS")"
    echo "    这些是**麒麟/操作系统自带**的库，不在本包内。"
    echo "    请把这句话发给系统管理员，他们需要知道装哪些包。"
else
    ok "界面依赖库全部可解析"
fi

# ---- 4. ssh / rsync：真实来源 + 运行时路径 ----
# 同步功能的全部工作都由 ssh + rsync 完成，而它们**不在**本包（二进制本体不含）。
#
# 【旧版两个错】
#  (a) 把**系统那一份**的版本当成"包内自带"报出来（7.2p2 / 3.1.1 其实都是系统的，
#      包内自带的那份是 OpenSSH_9.6p1，需要 glibc 2.38，在麒麟 V10 上根本跑不起来）。
#      运维据此判断"包自带、能独立运行"，判断基础是错的。
#  (b) 只测了 wrapper 能不能跑，没测**真实运行条件**下会不会挂死。
#      旧版 wrapper 的回落逻辑是 "command -v rsync" -> "exec rsync"，
#      而 PATH 里排在前面的就是 wrapper 自己 —— 无限 exec 循环，
#      麒麟 V10 上点"开始同步" 100% 挂死、日志一行不出。
#
# 现在分开报三件事：系统那一份的**路径和版本**、包内 wrapper 的**回落是否正常**、
# 系统 rsync 的**版本门槛**。
ssh_ok=0
rsync_ok=0

SSH_SYS=$(_find_system ssh)
RSYNC_SYS=$(_find_system rsync)

# 模拟真实运行条件跑一次 wrapper：PATH 里有 bin/，带 5 秒超时。
# 挂死（timeout 124）= 回落逻辑有缺陷 = 同步必然挂死，必须报出来。
RT_RC=0
RT_OUT=""
RT_HANG=0
runtime_probe() {
    _rp_name=$1; shift
    _rp_tmp="$_srt_tmpdir/.srt-probe.$$"
    if command -v timeout >/dev/null 2>&1; then
        PATH="$HERE/bin:$PATH" timeout 5 "$HERE/bin/$_rp_name" "$@" >"$_rp_tmp" 2>&1
        _rp_rc=$?
    else
        PATH="$HERE/bin:$PATH" "$HERE/bin/$_rp_name" "$@" >"$_rp_tmp" 2>&1
        _rp_rc=$?
    fi
    RT_RC=$_rp_rc
    RT_OUT=$(head -1 "$_rp_tmp" 2>/dev/null)
    rm -f "$_rp_tmp" 2>/dev/null
    if [ "$_rp_rc" = 124 ]; then
        RT_HANG=1
        return 1
    fi
    RT_HANG=0
    # 非 0 退出码一律算失败。
    # 原来只把 124（挂死）当失败：rc=127（wrapper 自己说"跑不起来"）也走成成功，
    # 于是输出成「✓ 回落正常（…在这台机器上都跑不起来。）」——
    # 一个自相矛盾的绿色勾，运维看了会以为包是好的。
    [ "$_rp_rc" = 0 ] || return 1
    return 0
}

if [ -n "$SSH_SYS" ]; then
    SSH_SYSVER=$("$SSH_SYS" -V 2>&1 | head -1)
    ok "ssh 客户端：系统自带 ${SSH_SYS}（${SSH_SYSVER:-版本未知}）"
    ssh_ok=1
else
    bad "系统里找不到 ssh 客户端 —— 同步功能不可用"
    echo "    请让系统管理员安装：sudo apt install openssh-client"
fi

if [ -n "$RSYNC_SYS" ]; then
    RSYNC_SYSVER=$("$RSYNC_SYS" --version 2>&1 | head -1)
    ok "rsync：系统自带 ${RSYNC_SYS}（${RSYNC_SYSVER:-版本未知}）"
    rsync_ok=1
    elif [ -x "$HERE/libexec/rsync" ] && "$HERE/libexec/rsync" --version >/dev/null 2>&1; then
    ok "rsync：系统里没有，但**包内自带静态版可用**（$("$HERE/libexec/rsync" --version 2>/dev/null | head -1)）"
    echo "    离线机器就是靠它：rsync 走 ssh 要求**两端**都有，作为接收端时请先执行："
    echo "        sudo sh \"$HERE/安装.sh\" --with-rsync"
    echo "    （会装到 /usr/local/bin/rsync，sshd 也能找到；不需要联网）"
    rsync_ok=1
    
else
    bad "系统里没有 rsync，包内自带的那份也跑不起来 —— 同步功能不可用"
    echo "    请让系统管理员安装：sudo apt install rsync"
fi

if [ -f "$HERE/libexec/rsync" ]; then
    # 2026-11-06：libexec/rsync 是**离线交付用的静态 rsync**，有意保留。
    # 原文案无条件 warn「它在麒麟 V10 上跑不起来，实际用的是系统那一份」——
    # 与 70 行之后那段新说明自相矛盾，也与事实相反：实测 bin/rsync 正是靠它跑起来的
    # （它是 LDFLAGS=-static 编的，ldd 报 not a dynamic executable，不吃 glibc 版本）。
    # 只有真的不可用时才该 warn。
    if [ -x "$HERE/libexec/rsync" ] && "$HERE/libexec/rsync" --version >/dev/null 2>&1; then
        ok "包内自带静态 rsync：可用（$("$HERE/libexec/rsync" --version 2>/dev/null | head -1)）"
    else
        warn "包内 libexec/rsync 存在但跑不起来（丢执行位或架构不符），会用系统 rsync。"
    fi
fi

# 包内 wrapper 的回落探测（真实运行条件 + 5 秒超时）
if [ -f "$HERE/bin/ssh" ]; then
    if runtime_probe ssh -V; then
        ok "包内启动脚本 bin/ssh：回落正常（5 秒内返回，${RT_OUT:-无输出}）"
    elif [ "$RT_HANG" = 1 ]; then
        bad "包内 bin/ssh 回落逻辑有缺陷：5 秒未返回（无限自我调用），开始同步会卡死"
        echo "    这是**包**的缺陷，不是这台机器的问题。请联系技术支持换包。"
    else
        bad "包内 bin/ssh 不可用（返回码 $RT_RC）"
        [ -n "$RT_OUT" ] && echo "    原因：$RT_OUT"
        echo "    这一条是**这台机器**的问题（系统里也没有可用的 ssh），"
        echo "    请让系统管理员执行：sudo apt install openssh-client"
    fi
fi
if [ -f "$HERE/bin/rsync" ]; then
    if runtime_probe rsync --version; then
        ok "包内启动脚本 bin/rsync：回落正常（5 秒内返回）"
    elif [ "$RT_HANG" = 1 ]; then
        bad "包内 bin/rsync 回落逻辑有缺陷：5 秒未返回（无限自我调用），开始同步会卡死"
        echo "    这是**包**的缺陷，不是这台机器的问题。请联系技术支持换包。"
    else
        bad "包内 bin/rsync 不可用（返回码 $RT_RC）"
        [ -n "$RT_OUT" ] && echo "    原因：$RT_OUT"
        echo "    这一条是**这台机器**的问题（系统里也没有可用的 rsync），"
        echo "    请让系统管理员执行：sudo apt install rsync"
    fi
fi

# 系统 rsync 的版本门槛
# 注意：rsync --version 的第一行里有两个 "version"：
#   "rsync  version 3.1.1  protocol version 31"
# 用 sed 的贪婪匹配会抓到后面那个 "protocol version 31"（当成 31），
# 所以这里按**字段**取：第一个 "version" 后面那个数字才是真版本号。
RSYNC_VER=$(printf '%s' "$RSYNC_SYSVER" | awk '{for(i=1;i<=NF;i++) if($i=="version" && $(i+1) ~ /^[0-9]/) {print $(i+1); exit}}')
if [ -n "$RSYNC_VER" ]; then
    _rv_maj=$(printf '%s' "$RSYNC_VER" | cut -d. -f1)
    _rv_min=$(printf '%s' "$RSYNC_VER" | cut -d. -f2)
    [ -n "$_rv_maj" ] || _rv_maj=0
    [ -n "$_rv_min" ] || _rv_min=0
    if [ "$_rv_maj" -gt 3 ] || { [ "$_rv_maj" = 3 ] && [ "$_rv_min" -ge 1 ]; }; then
        ok "rsync 版本 $RSYNC_VER：支持进度显示（--info=progress2）"
    elif [ "$_rv_maj" = 3 ]; then
        warn "rsync 版本 $RSYNC_VER：没有 --info=progress2，界面不显示实时进度（同步本身可用）"
        echo "    如需进度条，请让管理员升级 rsync 到 3.1 以上。"
    else
        bad "rsync 版本 $RSYNC_VER 太老：缺少 --from0 / --protect-args / --partial-dir，同步会失败"
        echo "    请让系统管理员升级：sudo apt install rsync"
    fi
else
    warn "读不出系统 rsync 的版本号，无法判断功能是否齐全"
fi

# 协议版本：两端必须对得上。rsync 3.x 用 protocol 31，2.6.x 用 30。
# 实测：两端协议对不上时 rsync 直接报
#   "rsync: protocol version 30 is not supported by this rsync"，退出码 12，
#   界面只显示"部分文件传输失败"，看不出是版本问题。
RSYNC_PROTO=$(printf '%s' "$RSYNC_SYSVER" | awk '{for(i=1;i<=NF;i++) if($i=="protocol" && $(i+1) ~ /^[0-9]+$/) {print $(i+1); exit}}')
if [ -n "$RSYNC_PROTO" ]; then
    if [ "$RSYNC_PROTO" -ge 31 ]; then
        ok "rsync 协议版本 $RSYNC_PROTO（与 rsync 3.x 对接）"
    else
        bad "rsync 协议版本 $RSYNC_PROTO —— 与接收端对不上，同步会直接失败"
        echo "    请让系统管理员升级 rsync 到 3.1 以上（协议 31）。"
    fi
fi

[ "$ssh_ok" = 1 ] && [ "$rsync_ok" = 1 ] || {
    # 2026-11-06 起本包**附带**了一份静态 rsync（libexec/rsync，在麒麟 V10 里
    # LDFLAGS=-static 编的，ldd 无动态依赖），所以不能再照旧说"本包不附带"。
    if [ -x "$HERE/libexec/rsync" ]; then
        echo "    说明：本包**附带**一份静态 rsync（libexec/rsync），但对端和 sshd 看不到它。"
        echo "         rsync 走 ssh 时**两端**都要有 rsync：接收端是被 sshd 拉起来的，"
        echo "         它按 login shell 的 PATH 找，看不到包目录。"
        echo "         离线机器上执行这一条即可让接收端也能用："
        echo "             sudo sh \"$HERE/安装.sh\" --with-rsync"
        echo "         （会把包内静态版装到 /usr/local/bin/rsync；不需要联网）"
    else
        echo "    说明：本包未附带 rsync（裁剪过）。镜像里 rsync 用**系统自带**的那一份；"
        echo "         都不可用时请让系统管理员执行：sudo apt install rsync"
    fi
}

# ---- 5. 口令加密后端 ----
# 程序是用 **dlopen 按名字找系统里的 libcrypto**（依次试 libcrypto.so.3 /
# libcrypto.so.1.1 / libcrypto.so.1.0.0 / libcrypto），找到就用 AES-256-CBC
# 加密登录口令；一个都找不到时**优雅降级**（登录口令不保存），不会崩溃。
#
# 这里必须按**同一个口径**去查。
# 原来这一节只检查 "$HERE/lib/libcrypto.so*"（包里的 lib 目录）——
# 而包里**本来就不带** OpenSSL（那正是当初改成 dlopen 的原因：
# 带一个固定 soname 的 libcrypto 会让系统直接起不来）。
# 于是这个判据恒为假，客户每次都看到一句
# 「保存的登录口令不会被加密」—— 而实际上系统的 libcrypto 找得到、口令是加密的。
# 一句与事实相反的话，比不说更糟。
#
# 程序自己的 --selfcheck 里会打印一行
#   INFO  crypto-backend  available=1  backend=...
# 那才是权威说法；下面这段只是给还没跑到第 7 项的人一个即时判断。
crypto_found=""
for cand in libcrypto.so.3 libcrypto.so.1.1 libcrypto.so.1.0.0 libcrypto.so; do
    # 先问动态链接器的缓存 —— dlopen 走的就是这份名单，口径一致
    if command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep -qE "[[:space:]]${cand}[[:space:]]"; then
        crypto_found="$cand"
        break
    fi
    # 再扫一遍常见库目录（没有 ldconfig 的精简系统上走这条）
    if _lib_exists "$cand"; then
        crypto_found="$cand"
        break
    fi
done
if [ -n "$crypto_found" ]; then
    ok "口令加密：可用（系统 $crypto_found）"
else
    say "○ 口令加密：系统里没有找到可用的 libcrypto，程序会自动降级"
    say "    降级后的行为：**登录口令不会被保存**（等于没有加密），其余功能不受影响，不会崩溃。"
    say "    如需启用：管理员执行  sudo apt install libssl1.1  （或 libssl3）"
fi

# ---- 6. 图形插件（plugins/）需要的系统库 ----
# 以前只查 $HERE/lib/，plugins/ 下的平台插件要用的系统库**一个都没查**。
# 结果是：缺 libX11-xcb.so.1 / libXi.so.6 / libXext.so.6 之类时，
# 离屏自检照样是绿的（离屏插件不需要 X），可图形界面就是起不来。
PLUGMISS=""
# 插件需要的 libQt5* 必须在**包内**。
# 缺了它，动态链接器会回落到**系统自带的 Qt**（版本多半对不上），
# 结果就是 xcb 平台插件加载失败、界面永远起不来。
# 真实案例（2026-10-23 麒麟 V10 真机）：
#   libqxcb.so: libQt5Core.so.5: version `Qt_5.15' not found
#              (required by /lib/x86_64-linux-gnu/libQt5DBus.so.5)
# 上一版自检把 libQt5* 一律当"包内自带"跳过，正好放过了这个缺陷。
PLUGQT_MISS=""
for so in "$HERE"/plugins/*/*.so "$HERE"/plugins/*.so; do
    [ -e "$so" ] || continue
    _pl_real=$(readlink -f "$so" 2>/dev/null || echo "$so")
    _pl_ldd=$(LD_LIBRARY_PATH="$HERE/lib" ldd "$_pl_real" 2>&1)
    # 形态 A："libX11-xcb.so.1 => not found" —— $1 就是库名
    for _pl_one in $(echo "$_pl_ldd" | grep 'not found' | awk '$1 ~ /^lib/ {print $1}'); do
        PLUGMISS="$PLUGMISS $_pl_one"
    done
    # 形态 B："xxx.so: /path/libQt5Core.so.5: version `Qt_5.15' not found (...)"
    #        这一行的 $1 是**对象路径**（插件自己），不是库名，
    #        所以 awk '{print $1}' 取到的是插件路径，会把"缺库"报成"插件有问题"。
    for _pl_one in $(echo "$_pl_ldd" | grep 'not found' | sed -n 's/.*: \([^:]*\): version .* not found.*/\1/p'); do
        PLUGMISS="$PLUGMISS $_pl_one"
    done
    # 插件需要的 libQt5* 必须在包内（即使 ldd 解析到了系统那一份）
    for _pl_dep in $(echo "$_pl_ldd" | awk '$1 ~ /^libQt5/ {print $1}' | sort -u); do
        if [ ! -e "$HERE/lib/$_pl_dep" ]; then
            PLUGQT_MISS="$PLUGQT_MISS $_pl_dep"
        fi
    done
done
# 明确点名的那几个：平台插件直接依赖，缺了界面必起不来
for l in libX11-xcb.so.1 libXi.so.6 libX11.so.6 libxcb.so.1 libfontconfig.so.1 \
         libfreetype.so.6 libglib-2.0.so.0 libGL.so.1 libXext.so.6; do
    if ! _lib_exists "$l" && ! (command -v ldconfig >/dev/null 2>&1 && ldconfig -p 2>/dev/null | grep -q "$l"); then
        PLUGMISS="$PLUGMISS $l"
    fi
done
if [ -n "$PLUGQT_MISS" ]; then
    bad "包内缺少插件依赖的 Qt 库：$(fmt_libs "$PLUGQT_MISS")"
    echo "    缺这些库时，动态链接器会回落到**系统自带的 Qt**（版本多半对不上），"
    echo "    表现为：图形界面起不来，报 could not find or load the Qt platform plugin "xcb"。"
    echo "    这是**交付包不完整**，请联系技术支持换一份完整的包，不要自己装系统库。"
fi
if [ -n "$PLUGMISS" ]; then
    bad "图形插件缺少系统库：$(fmt_libs "$PLUGMISS")"
    echo "    这些是**麒麟/操作系统自带**的库。缺了它们界面会起不来，"
    echo "    但离屏自检看不出来。请把这句话发给系统管理员。"
else
    ok "图形插件需要的系统库全部可解析"
fi

# ---- 7. 程序本体能不能真的起起来（离屏自检）----
#
# 为什么必须单独查：上面几项全过，程序照样可能一个窗口都开不出来。
#
# 银河麒麟的安全中心（kysec / 应用执行控制）会**按文件**给动态库打执行标记。
# 被标记成"未放行"时，dlopen 会失败，而失败信息是
#     failed to map segment from shared object
# 加上 Qt 那句
#     could not find or load the Qt platform plugin "offscreen"
# 这两句话里没有一个字提到"安全中心"，客户看到只会以为软件坏了。
#
# 实测（麒麟 V11 2603，同一份包同一台机器）：
#   /home/Lei/srttest/     -> 起不来（那批文件被执行控制标记过）
#   复制到 /tmp/pkgtest/   -> 同一个二进制、同一批库，全部正常
# 因为新复制的文件会被重新评估。
#
# 这一项就是把这个静默失败翻译成人话。
if [ -x "$HERE/SyncReplayTool" ]; then
    _rc_log="$_srt_tmpdir/srt-jiancha-$$"
    LD_LIBRARY_PATH="$HERE/lib:${LD_LIBRARY_PATH:-}" \
    QT_PLUGIN_PATH="$HERE/plugins" \
    QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms" \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -u)}" \
    QT_QPA_PLATFORM=offscreen \
        "$HERE/SyncReplayTool" --selfcheck >/dev/null 2>"$_rc_log"
    _rc=$?
    if [ "$_rc" = 0 ]; then
        ok "程序本体可启动（离屏自检通过）"
    elif grep -qi 'failed to map segment' "$_rc_log" 2>/dev/null; then
        # 这一句是 mmap(PROT_EXEC) 被拒，只有安全中心/安全软件会这么干。
        # 所以到这一支就可以确定是**被拦**，不是文件缺失。
        bad "程序本体被系统拦截，无法加载图形组件"
        echo "    这是**安全中心**（控制面板 → 安全中心 → 应用执行控制）拦下来的。"
        echo "    处理办法（任选一种，从简单到麻烦）："
        echo "      1) 把整个解压出来的文件夹复制到别处再运行，例如复制到桌面或 /tmp 下试一次；"
        echo "      2) 在安全中心里把本程序、以及同目录下的 plugins/ 和 lib/ 设为「始终允许」；"
        echo "      3) 仍然打不开的话，把这一屏发给技术支持。"
        echo ""
        echo "    （这一条是银河麒麟特有的。其它国产系统也可能有类似组件，现象一样。）"
    elif grep -qi 'could not find or load the Qt platform plugin' "$_rc_log" 2>/dev/null; then
        # 走到这一支只能说明"文件不在或加载不了"，**还没有**证据说它是被拦的。
        # 一口咬定是安全中心就会在包解压不完整的时候误导人，所以先查文件在不在。
        bad "图形组件（Qt 平台插件）加载不了"
        # 数目录里还剩几个 .so，而不是只查某一个文件：
        # 少哪一个都不对，但"一个都不剩"和"少了几个"要分开说。
        _nplug=$(ls -1 "$HERE/plugins/platforms/"*.so 2>/dev/null | wc -l)
        # **期望数量从包里读，不再硬编码**（2026-11-10 审查 P2-1）。
        # 打包脚本 package-kylin-container.sh 在裁完插件后把最终数量写进
        # plugins/platforms/.expected-count；这里只读那份事实。
        # 原来写死"应有 2 个"，而真正决定数量的是打包脚本里那几行 rm ——
        # 两边一旦不同步，就会对着一个完整的包说"解压不完整"，把客户引去重拷。
        _expect=$(cat "$HERE/plugins/platforms/.expected-count" 2>/dev/null)
        case "$_expect" in
            ''|*[!0-9]*) _expect=2 ;;   # 老包没有这个文件，退回历史值 2
        esac
        if [ "$_nplug" -eq 0 ]; then
            echo "    plugins/platforms/ 里一个插件都没有 —— 这个包**解压不完整**。"
        elif [ "$_nplug" -lt "$_expect" ]; then
            echo "    plugins/platforms/ 里只剩 $_nplug 个插件（本包应有 $_expect 个）—— 解压不完整。"
            echo "    请重新完整拷贝一次，或重新解压一次压缩包。"
        else
            echo "    文件在，但加载失败。可能是**安全中心**把它拦了，也可能是它损坏。"
            echo "    先试：把整个文件夹复制到别处再运行一次。"
            echo "    还不行：在安全中心 → 应用执行控制 里把本程序和 plugins/、lib/ 设为「始终允许」。"
        fi
        echo "    仍不行的话，把这一屏发给技术支持。"
    elif grep -qEi 'shared librar|cannot open shared object' "$_rc_log" 2>/dev/null; then
        bad "运行库缺失：这台电脑的系统库版本对不上"
        echo "    缺哪个库见上面第 3 项的输出。把这一屏发给技术支持。"
    else
        bad "程序本体启动失败（自检返回码 $_rc）"
        if [ -s "$_rc_log" ]; then
            echo "    请把下面这几行发给技术支持："
            sed -n '1,8p' "$_rc_log" 2>/dev/null | sed 's/^/      /'
        else
            echo "    （没能抓到程序的输出：临时文件建不出来，"
            echo "      多半是 $_srt_tmpdir 不可写。请检查这个目录的权限或磁盘空间，"
            echo "      然后把这一屏发给技术支持。）"
        fi
    fi
    rm -f "$_rc_log" 2>/dev/null
elif [ -f "$HERE/SyncReplayTool" ]; then
    # 文件在、却没有执行位。
    #
    # 原来这里只 say 一句"跳过启动自检"就往下走，最后照样打印"环境满足，可以启动"
    # 并以 0 退出 —— 也就是说**唯一**会导致双击没反应的原因，被判成了通过。
    # 从 U 盘拷贝最容易踩：有些拷贝方式会丢掉可执行位。
    bad "SyncReplayTool 没有执行权限，双击不会运行"
    echo "    终端里执行一次即可修复：  chmod +x \"$HERE/SyncReplayTool\""
    echo "    （或者重新用『安装.sh』装一遍，它会自动处理权限。）"
else
    bad "包里找不到 SyncReplayTool，无法启动"
fi

# ---- 8. 桌面图标 ----
if [ -f "$HERE/数据同步工具.desktop" ]; then
    say "  （桌面图标文件在包里；要在桌面放图标请执行一次 sh 安装.sh）"
else
    say "  （可选）双击解压出来的 run.sh 即可启动"
fi

# ---- 9. 图形界面 ----
if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
    # DISPLAY 有值**不等于**图形会话真的连着：X server 没起来、远程桌面没连上，
    # 都会留下一个看着正常的 DISPLAY 值，而真启动时只有 Qt 原话
    # "Could not connect to any X display." —— 客户完全看不懂。
    # 这里主动探一下，探不到就**别报绿**。
    _srt_xok=1
    if command -v xdpyinfo >/dev/null 2>&1; then
        xdpyinfo >/dev/null 2>&1 || _srt_xok=0
    elif command -v xset >/dev/null 2>&1; then
        xset q >/dev/null 2>&1 || _srt_xok=0
    else
        say "  （这台机器上没有 xdpyinfo / xset，无法确认图形会话是否真的连着。）"
    fi
    if [ "$_srt_xok" = 1 ]; then
        ok "检测到图形界面"
    else
        warn "DISPLAY=${DISPLAY:-} 连不上图形会话（X server 没起来，或远程桌面没连上）"
        echo "    本工具需要**真实桌面会话**才能开窗口。请先确认远程桌面/VNC 已连上再启动。"
    fi
else
    say "⚠ 没有图形界面（DISPLAY 为空）：程序会**在离屏模式下继续跑，但不会有窗口**。"
    say "    实测：无 DISPLAY 启动后同步**照常进行**（接收端确实收到文件），只是屏幕上什么都没有。"
    say "    请在**带桌面的机器上**双击 run.sh 启动；通过 SSH 启动看不到界面是正常的。"
fi

# ======================================================================
# 下面几项：把"到了客户新机器上跑不起来"的原因逐条变成可执行的检查。
# 每项 ≤3 行；失败时给一行**能直接抄**的修复命令。
# ======================================================================

# ---- 10. 启动入口：必须用 run.sh，不能直接跑二进制 ----
# 实测（麒麟 V10 容器，退出码都是实跑出来的）：
#   直接 ./SyncReplayTool
#       -> "./SyncReplayTool: /usr/lib/x86_64-linux-gnu/libQt5Core.so.5: version `Qt_5.9' not found"
#          退出码 1（先在动态链接器上就挂了，撞上的是系统自带的 Qt 5.x）
#   只给 LD_LIBRARY_PATH、不给 QT_PLUGIN_PATH
#       -> "could not find or load the Qt platform plugin \"xcb\"" + "Aborted (core dumped)"
#          退出码 134
#   这两种把 stderr 丢掉就**一个字都不剩**，部署人员只看到"闪一下就没了"。
if [ -f "$HERE/run.sh" ] && [ -r "$HERE/run.sh" ]; then
    ok "启动入口：run.sh 在（务必用它启动，不要直接双击 SyncReplayTool）"
else
    bad "包里找不到 run.sh，没有可用的启动入口"
    echo "    请重新完整拷贝一次压缩包；实在没有就手动补两个变量再启动："
    echo "    cd \"$HERE\" && LD_LIBRARY_PATH=\"$HERE/lib\" QT_PLUGIN_PATH=\"$HERE/plugins\" QT_QPA_PLATFORM_PLUGIN_PATH=\"$HERE/plugins/platforms\" ./SyncReplayTool"
fi

# ---- 读目标配置（第 11~14 项要用）----
CFG_INI=$(_srt_inifile 2>/dev/null || true)
T_HOST=""; T_PORT=""; T_USER=""; T_KEY=""; T_REMOTE=""; T_LOCAL=""
if [ -n "$CFG_INI" ]; then
    T_HOST=$(_srt_ini "$CFG_INI" host 2>/dev/null || true)
    T_PORT=$(_srt_ini "$CFG_INI" port 2>/dev/null || true)
    T_USER=$(_srt_ini "$CFG_INI" user 2>/dev/null || true)
    T_KEY=$(_srt_ini "$CFG_INI" keyPath 2>/dev/null || true)
    T_REMOTE=$(_srt_ini "$CFG_INI" remoteRoot 2>/dev/null || true)
    T_LOCAL=$(_srt_ini "$CFG_INI" localRoot 2>/dev/null || true)
fi
# 端口只要数字（ini 可能被手改坏）
case "$T_PORT" in ''|*[!0-9]*) T_PORT="" ;; esac

# 用配置好的目标做一次**只读**的 ssh 探测；结果在 PROBE_OUT / 返回码里。
_srt_sshprobe() {
    _sp_cmd="$1"
    _sp_out="$_srt_tmpdir/.srt-sshprobe.$$"
    # 用 set -- 拼参数，私钥路径带空格/中文时才不会被拆开。
    set -- -p "${T_PORT:-22}" -o BatchMode=yes -o ConnectTimeout=8 \
           -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
    if [ -n "$T_KEY" ] && [ -f "$T_KEY" ]; then
        set -- "$@" -i "$T_KEY"
    fi
    set -- "$@" "$T_USER@$T_HOST" "$_sp_cmd"
    if command -v timeout >/dev/null 2>&1; then
        timeout 12 ssh "$@" >"$_sp_out" 2>&1
    else
        ssh "$@" >"$_sp_out" 2>&1
    fi
    _sp_rc=$?
    PROBE_OUT=$(cat "$_sp_out" 2>/dev/null)
    rm -f "$_sp_out" 2>/dev/null
    return $_sp_rc
}

# ---- 11. 私钥权限：OpenSSH 在用私钥**之前**先看权限 ----
# 实测（麒麟 V10 / OpenSSH 7.2p2）：
#   chmod 0644 -> "Permissions 0644 for '/root/keys/id_envc' are too open."
#                 + "Load key \"/root/keys/id_envc\": bad permissions"
#                 + "Permission denied (publickey,password,keyboard-interactive)."  退出码 255
#   chmod 0666 / 0640 同样被拒。
# **最后一行只有 "Permission denied"，和"密钥填错了"完全一样**，极易误判。
# Windows 侧 OpenSSH 是同一套逻辑，措辞是 "Permissions for '...' are too open."。
KEYFILE="$T_KEY"
if [ -z "$KEYFILE" ] || [ ! -f "$KEYFILE" ]; then
    for _kf_d in "${HOME:-/root}/.ssh"; do
        for _kf_f in "$_kf_d"/id_rsa "$_kf_d"/id_ecdsa "$_kf_d"/id_ed25519; do
            [ -f "$_kf_f" ] && { KEYFILE="$_kf_f"; break; }
        done
        [ -n "$KEYFILE" ] && break
    done
fi
if [ -n "$KEYFILE" ] && [ -f "$KEYFILE" ]; then
    KPERM=$(stat -c '%a' "$KEYFILE" 2>/dev/null || ls -l "$KEYFILE" 2>/dev/null | cut -c1-10 | sed 's/.*rwx//; s/[rwx-]//g')
    KPERM=$(printf '%s' "$KPERM" | tr -cd '0-7')
    K3=$(printf '%s' "$KPERM" | tail -c 3)
    KGRP=$(printf '%s' "$K3" | cut -c2); KOTH=$(printf '%s' "$K3" | cut -c3)
    if [ "$KGRP" = "0" ] && [ "$KOTH" = "0" ]; then
        ok "私钥权限：$KEYFILE（$KPERM，只有自己能读写）"
    else
        bad "私钥权限：$KEYFILE 是 $KPERM —— 别人也能读，ssh 会拒绝用这把钥匙"
        echo "    chmod 600 \"$KEYFILE\""
    fi
else
    say "○ 没找到私钥文件（界面上还没填私钥路径，或密钥不在本机）"
fi

# ---- 12. 目标主机与账户：连通性 / 账户被锁 / 主机密钥 ----
# 实测（接收端 Kylin V11 / OpenSSH 9.6p1）：
#   账户被锁（/etc/shadow 的密码字段被加了 '!'）时，sshd 在**公钥认证之前**就拒绝：
#     sshd 日志: "User apuser not allowed because account is locked"
#     客户端只看到: "Permission denied (publickey,password,keyboard-interactive)."  退出码 255
#   客户端那一句和"密钥错了"一模一样，看不出区别 —— 必须去接收端查。
#   首次连接 / 主机密钥变化：产品默认 autoAcceptHostKey=true，
#   用的是 StrictHostKeyChecking=no + UserKnownHostsFile=/dev/null，
#   所以**主机密钥变了也不会有任何提示**（实测：变了照样连上，一个字都不说）。
if [ -z "$T_HOST" ] || [ -z "$T_USER" ]; then
    say "○ 还没配置目标主机/账号（第 12~14 项跳过；配好后再跑一次本脚本）"
elif ! command -v ssh >/dev/null 2>&1 && [ ! -x "$HERE/bin/ssh" ]; then
    warn "这台机器上没有 ssh 客户端，第 12~14 项没法自动检查"
    echo "    请让系统管理员安装：sudo apt install openssh-client"
else
    if _srt_sshprobe 'echo SRT_PROBE_OK'; then
        ok "目标主机连通并可登录：$T_USER@$T_HOST:$T_PORT"
        _ahk=$(_srt_ini "$CFG_INI" autoAcceptHostKey 2>/dev/null || true)
        case "$_ahk" in
            false|0|no)
                ok "主机密钥：严格校验（StrictHostKeyChecking=yes）" ;;
            *)
                warn "主机密钥：自动接受 —— 接收端换了机器或被中间人劫持时**不会有任何提示**"
                echo "    要严格校验：界面里取消「自动接受主机公钥」后保存设置。" ;;
        esac
    else
        case "$PROBE_OUT" in
            *"not allowed because account is locked"*)
                bad "目标账户被锁：接收端 sshd 拒绝了 $T_USER"
                echo "    在**接收端**执行：sudo grep ^$T_USER: /etc/shadow   （字段以 ! 开头就是被锁了）"
                ;;
            *"Host key verification failed"*|*"REMOTE HOST IDENTIFICATION HAS CHANGED"*)
                bad "主机密钥校验失败：$T_HOST 的主机密钥变了或还没接受"
                echo "    确认确实换了机器后：ssh-keygen -R \"[$T_HOST]:${T_PORT:-22}\" && ssh $T_USER@$T_HOST"
                ;;
            *"Permission denied"*|*"bad permissions"*|*"too open"*)
                bad "认证被拒：私钥、账号或账户状态有问题（客户端只说 Permission denied）"
                echo "    私钥权限：chmod 600 <私钥路径>；再到接收端查：sudo grep ^$T_USER: /etc/shadow"
                ;;
            *"Connection refused"*|*"Connection timed out"*|*"No route to host"*|*"Could not resolve"*)
                bad "连不上目标主机：$T_HOST:${T_PORT:-22}"
                echo "    在本机执行：nc -vz $T_HOST ${T_PORT:-22}   （不通就让网络/防火墙管理员放行）"
                ;;
            *)
                bad "连接 $T_USER@$T_HOST:${T_PORT:-22} 失败"
                echo "    详情：${PROBE_OUT:-（没抓到输出）}"
                ;;
        esac
    fi
fi

# ---- 13. 接收端是不是 UTF-8（中文文件名）----
# 实测：rsync 在**文件清单**里对非 ASCII 文件名做八进制转义，
# 所以任何一次失败，stderr 里的中文文件名都是 "\#344\#270\#255..." 这种形态。
# 这跟 locale **无关**（实测发送端 C.UTF-8 / C / zh_CN.utf8、接收端 POSIX / UTF-8，
# 有无 --protect-args，六种组合结果逐字节相同）。
# 落地文件名本身是对的（实测 od 是正确 UTF-8），**没有**静默改名。
# 另有一条实测：接收端 sshd 给的会话**永远没有 locale**
# （sshd 重建子进程环境，不继承 LANG；~/.profile、~/.bashrc 在非交互会话里也不读），
# 所以接收端默认就是 POSIX。
if [ -z "$T_HOST" ] || [ -z "$T_USER" ]; then
    say "○ 还没配置目标主机，第 13 项跳过"
elif _srt_sshprobe 'locale charmap 2>/dev/null || echo NO_LOCALE_CMD'; then
    case "$PROBE_OUT" in
        *UTF-8*|*utf8*|*UTF8*)
            ok "接收端字符集：$PROBE_OUT（中文文件名能正常显示）" ;;
        *)
            warn "接收端字符集：${PROBE_OUT:-未知} —— 不是 UTF-8，中文名在报错里会变成八进制转义"
            echo "    在接收端装 UTF-8 locale：sudo locale-gen zh_CN.UTF-8" ;;
    esac
else
    say "○ 连不上接收端，跳过第 13 项"
fi

# ---- 14. 配置目录 / 单实例锁目录能不能写 ----
# 单实例锁文件放在配置目录里。目录写不进去时：
#   - 设置存不下去（实测：目录只读时配置文件一个字也写不进去）；
#   - 锁文件建不出来时，程序会认为"已经有另一个实例在跑"而拒绝启动。
CFG_DIR_HIT=""
CFG_EXISTS=0
_srt_cfgdirs > "$_srt_tmpdir/.srt-cfgd.$$" 2>/dev/null
while IFS= read -r _cd_d; do
    [ -n "$_cd_d" ] || continue
    if [ -d "$_cd_d" ]; then CFG_DIR_HIT="$_cd_d"; CFG_EXISTS=1; break; fi
    if [ -z "$CFG_DIR_HIT" ]; then CFG_DIR_HIT="$_cd_d"; fi
done < "$_srt_tmpdir/.srt-cfgd.$$"
rm -f "$_srt_tmpdir/.srt-cfgd.$$" 2>/dev/null
if [ -z "$CFG_DIR_HIT" ]; then
    warn "找不到配置目录（$HOME/.config/AP 或 $HOME/.config/SyncReplayTool）"
    echo "    mkdir -p \"${HOME:-/root}/.config/AP\" && chmod 700 \"${HOME:-/root}/.config/AP\""
elif [ "$CFG_EXISTS" = 1 ]; then
    if [ -w "$CFG_DIR_HIT" ]; then
        ok "配置/锁目录可写：$CFG_DIR_HIT"
    else
        bad "配置/锁目录不可写：$CFG_DIR_HIT —— 设置存不下去、单实例锁也建不出来"
        echo "    chmod u+rwx \"$CFG_DIR_HIT\""
    fi
else
    warn "配置目录还不存在：$CFG_DIR_HIT（程序第一次保存设置时会自动建）"
    echo "    mkdir -p \"$CFG_DIR_HIT\" && chmod 700 \"$CFG_DIR_HIT\""
fi

# ---- 15. 开机自启装了没有 ----
# 麒麟侧的开机自启是 ~/.config/autostart/ 下的 .desktop 文件。
# 判据三件套：文件在 + Exec= 是**绝对路径** + 文件可读。
# 绝对路径这条很重要：.desktop 里写相对路径时，开机时的工作目录不确定，
# 自启会**静默不启动**（用户以为装上了，其实每次开机都要手动点）。
AD_DIR="${XDG_CONFIG_HOME:-${HOME:-/root}/.config}/autostart"
AD_FOUND=""
if [ -d "$AD_DIR" ]; then
    for _ad_f in "$AD_DIR"/*.desktop; do
        [ -f "$_ad_f" ] || continue
        if grep -qi "SyncReplayTool\|数据同步工具" "$_ad_f" 2>/dev/null; then
            AD_FOUND="$_ad_f"
            break
        fi
    done
fi
if [ -z "$AD_FOUND" ]; then
    say "○ 开机自启：没装（$AD_DIR 下没有本程序的 .desktop）"
    say "    要开机自动同步：sh \"$HERE/安装自启.sh\"   （或界面里勾选后保存设置）"
else
    _ad_exec=$(grep -m1 '^Exec=' "$AD_FOUND" 2>/dev/null | sed 's/^Exec=//')
    # Exec 有两种形态：
    #   Exec=/绝对路径/run.sh         —— 脚本自带 shebang 且可执行
    #   Exec=sh "/绝对路径/run.sh"    —— **产品自己写的就是这一种**
    #                                    （ConfigStore::autoStartCommand()）
    # 判"是不是绝对路径"时必须先把 sh 前缀和引号剥掉，
    # 否则 `sh "/opt/srt/run.sh"` 会被判成"不是绝对路径" → 报 ✗ →
    # 自检结论变成「环境不满足，软件无法在这台机器上运行」。
    # 而实际上自启是好的 —— 客户在界面里勾了「开机自动启动」就会踩到。
    # （本轮实测发现，2026-10-23。）
    _ad_path=$(printf '%s' "$_ad_exec" | sed -e 's/^[sS][hH][[:space:]]*//' -e 's/^"//' -e 's/"$//')
    case "$_ad_path" in
        /*)
            if [ ! -f "$_ad_path" ]; then
                bad "开机自启指向的程序不存在：$_ad_path"
                echo "    多半是包被移动/改名了。重新生成：sh \"$HERE/安装.sh\" --autostart"
            elif [ -r "$AD_FOUND" ]; then
                ok "开机自启已安装：$AD_FOUND"
            else
                bad "开机自启文件读不了：$AD_FOUND"
                echo "    chmod 644 \"$AD_FOUND\""
            fi ;;
        '')
            bad "开机自启文件里没有 Exec=：$AD_FOUND"
            echo "    重新生成：sh \"$HERE/安装.sh\" --autostart" ;;
        *)
            bad "开机自启的 Exec 不是绝对路径：$_ad_exec"
            echo "    改成绝对路径后重装：sh \"$HERE/安装.sh\" --autostart" ;;
    esac
fi

# ---- 16. 远端目录能不能写（只读挂载 / 权限不足）----
# 实测：目标目录是**只读挂载**时，rsync 报
#   "mkstemp "..." failed: Read-only file system (30)"，退出码 23，
#   界面只显示"部分文件传输失败"，看不出是只读。
# 目录权限不足时报 "Permission denied (13)"，同样看不出该改哪里。
# 这里在同步**之前**先探一次，免得传完才发现。
if [ -z "$T_HOST" ] || [ -z "$T_USER" ]; then
    say "○ 还没配置目标主机，第 16 项跳过"
elif [ -z "$T_REMOTE" ]; then
    say "○ 还没配置远端目录，第 16 项跳过"
elif _srt_sshprobe "test -w '$T_REMOTE' && echo SRT_RW_YES || echo SRT_RW_NO"; then
    case "$PROBE_OUT" in
        *SRT_RW_YES*) ok "远端目录可写：$T_REMOTE" ;;
        *SRT_RW_NO*)
            bad "远端目录不可写：$T_REMOTE —— 同步会以 Read-only file system / Permission denied 失败"
            echo "    在接收端执行：chmod u+rwx '$T_REMOTE'" ;;
        *)
            warn "探不到远端目录的写权限：$T_REMOTE（目录可能还不存在）"
            echo "    在接收端执行：ls -ld '$T_REMOTE'" ;;
    esac
else
    say "○ 连不上接收端，跳过第 16 项"
fi

# ---- 17. HOME 在不在 ----
# 实测：HOME 未设 + **直接跑二进制** -> Qt 找不到平台插件，abort(134)，
# 而且包目录里的中文变成 "??????????????-??????"（locale 退化成 POSIX，路径解码不了）。
# 走 run.sh 则没事（run.sh 自己兜一个 UTF-8 locale）。
if [ -n "${HOME:-}" ] && [ -d "${HOME:-/nonexistent}" ]; then
    ok "HOME 指向可用目录：$HOME"
else
    warn "HOME 没设或不可用 —— 直接跑二进制会 abort(134)（走 run.sh 才没事）"
    echo "    桌面图标 / systemd 自启请改成：export HOME=<该用户的家目录>"
fi

# ---- 18. 本地根目录路径长度 ----
# 实测：路径超过 PATH_MAX(4096) 时，rsync 报
#   "link_stat "<整条路径>" failed: No such file or directory (2)"，退出码 23。
# 单个文件名超过 NAME_MAX(255) 时，本地根本建不出这个文件（"File name too long"）。
if [ -n "$T_LOCAL" ]; then
    _plen=$(printf '%s' "$T_LOCAL" | wc -c)
    _pmax=$(getconf PATH_MAX / 2>/dev/null || echo 4096)
    if [ "$_plen" -gt $((_pmax - 260)) ]; then
        bad "本地根目录路径过长（$_plen 字节，PATH_MAX=$_pmax）—— 同步会以 link_stat 失败结束"
        echo "    请把本地根目录换到短路径下（例如 /data）"
    else
        ok "本地根目录路径长度 $_plen 字节（上限 PATH_MAX=$_pmax）"
    fi
else
    say "○ 还没配置本地根目录，第 18 项跳过"
fi

# ---- 19. coreutils 版本（给将来要用 --zero 的人踩坑）----
# 实测（麒麟 V10）：coreutils 8.25 的 md5sum **不支持** --zero：
#   "md5sum: unrecognized option '--zero'"
# 本包与产品目前**一处都没用** md5sum --zero，所以这一项只作提示、不会让自检失败。
CU_VER=$(md5sum --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+$')
if [ -z "$CU_VER" ]; then
    warn "读不出 coreutils 版本"
else
    if printf 'x' | md5sum --zero >/dev/null 2>&1; then
        ok "coreutils $CU_VER（支持 md5sum --zero）"
    else
        say "○ coreutils $CU_VER —— 它的 md5sum 不支持 --zero（本包未用到，仅提示）"
        say "    以后要写 NUL 分隔的校验和时，用 md5sum 再把换行换成 NUL 字节，别用 --zero。"
    fi
fi

echo
# ---- 20. 开机自动启动（XDG autostart）：本程序那一份 + 配置与实际是否一致 ----
#
# 第 15 项已经查过「目录里有没有 .desktop、Exec 是不是绝对路径」。
# 这一项查得更具体、也更要紧的两件事：
#   ① **本程序**那一份（~/.config/autostart/syncreplaytool.desktop）在不在，
#      它的 Exec 指向的 run.sh **真的存在**（包被挪走/改名后最常见的坑）；
#   ② 配置里的 app/autoStartEnabled（用户意图）与系统里的实际落地（事实）
#      **是不是一致** —— 不一致时程序**不会报错**，只是开机不启动，
#      用户只会觉得"自启是坏的"。所以这里把差明确指出来并给修法。
_AUTOSTART_DIR="$HOME/.config/autostart"
_AUTOSTART_FILE="$_AUTOSTART_DIR/syncreplaytool.desktop"
_AUTOSTART_EXISTS=0
_AUTOSTART_EXEC=""
if [ -f "$_AUTOSTART_FILE" ]; then
    _AUTOSTART_EXISTS=1
    _AUTOSTART_EXEC=$(sed -n 's/^Exec=//p' "$_AUTOSTART_FILE" 2>/dev/null | head -1)
    # Exec 形如：sh "<目录>/run.sh"  → 剥掉 sh 前缀和引号，取出真实路径
    _AUTOSTART_TARGET=$(printf '%s' "$_AUTOSTART_EXEC" | sed -e 's/^[sS][hH][[:space:]]*//' -e 's/^"//' -e 's/"$//')
    if [ -z "$_AUTOSTART_TARGET" ]; then
        bad "开机自启文件在，但没有 Exec 行：$_AUTOSTART_FILE"
        echo "    修法：sh \"$HERE/安装.sh\" --autostart"
    elif [ ! -f "$_AUTOSTART_TARGET" ]; then
        bad "开机自启指向的程序不存在：$_AUTOSTART_TARGET"
        echo "    多半是包被移动/改名了。修法：sh \"$HERE/安装.sh\" --autostart"
    else
        ok "开机自启指向的程序存在：$_AUTOSTART_TARGET"
    fi
else
    say "○ 没装本程序的开机自启（$_AUTOSTART_FILE 不存在）"
fi

# 配置里的用户意图
_AUTOSTART_KEY=""
[ -n "$CFG_INI" ] && _AUTOSTART_KEY=$(_srt_ini "$CFG_INI" autoStartEnabled 2>/dev/null || true)
case "$_AUTOSTART_KEY" in
    true|1|yes|on) _AUTOSTART_WANT=1 ;;
    *)              _AUTOSTART_WANT=0 ;;
esac
if [ -z "$CFG_INI" ]; then
    say "○ 还没生成配置文件，第 21 项跳过（先启动一次软件就会生成）"
elif [ "$_AUTOSTART_WANT" = "1" ] && [ "$_AUTOSTART_EXISTS" = "0" ]; then
    warn "配置里「开机自动启动」是**开**，但系统里**没有**自启文件 —— 开机不会自动启动"
    echo "    修法一（推荐）：sh \"$HERE/安装.sh\" --autostart"
    echo "    修法二：软件「设置」里取消勾选「开机自动启动」再勾上"
elif [ "$_AUTOSTART_WANT" = "0" ] && [ "$_AUTOSTART_EXISTS" = "1" ]; then
    warn "系统里有开机自启文件，但配置里「开机自动启动」是**关** —— 两者不一致"
    echo "    修法一：sh \"$HERE/安装.sh\" --no-autostart"
    echo "    修法二：软件「设置」里勾上「开机自动启动」再取消"
else
    ok "开机自启状态一致（配置=$_AUTOSTART_KEY，系统=$([ "$_AUTOSTART_EXISTS" = "1" ] && echo 已装 || echo 未装)）"
fi

# ---- 21. 每天定点的时刻 ----
# 配置里 sync/autoSyncDailyTime 是 "HH:mm"，sync/autoSyncMode 是 daily/interval。
# 这一项只做**格式**检查和提示：时刻本身合不合法，用户在设置界面里点一下就有提示，
# 自检在这里重复一遍只能多一条噪音。真要报，得报"格式坏了"而不是"时刻不对"。
_AUTOSYNC_MODE=""
[ -n "$CFG_INI" ] && _AUTOSYNC_MODE=$(_srt_ini "$CFG_INI" autoSyncMode 2>/dev/null || true)
_AUTOSYNC_TIME=""
[ -n "$CFG_INI" ] && _AUTOSYNC_TIME=$(_srt_ini "$CFG_INI" autoSyncDailyTime 2>/dev/null || true)
if [ -z "$CFG_INI" ]; then
    say "○ 还没生成配置文件，第 21 项跳过"
elif [ "$_AUTOSYNC_MODE" = "daily" ]; then
    case "$_AUTOSYNC_TIME" in
        [0-2][0-9]:[0-5][0-9]) ok "每天定时：$_AUTOSYNC_TIME（本地时间）" ;;
        *) bad "每天定点的时刻格式不对：$_AUTOSYNC_TIME（应为 HH:mm）"
           echo "    修法：软件「设置」里重新选一次「每天定时」的时刻并保存" ;;
    esac
else
    say "○ 自动同步不是「每天定时」模式（当前：${_AUTOSYNC_MODE:-interval}），第 21 项跳过"
fi

echo
if [ "$FAIL" != "0" ]; then
    echo "结论：环境不满足，软件**无法在这台机器上运行**。请把上面的 ✗ 交给系统管理员。"
    exit 1
fi
say "结论：环境满足，可以启动。"
exit 0
