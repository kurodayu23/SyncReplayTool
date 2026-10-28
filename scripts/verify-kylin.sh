#!/bin/bash
# =============================================================================
# 在容器里对**交付包**做回归验证。
#
# 用法：
#   bash /verify.sh <包.tar.gz | 包根目录> [--old-tar <旧包.tar.gz>]
#
# 关键判据（对应 P0-1 / P0-2 / P1-*）：
#   1. PATH 里有 bin/ 时，bin/rsync、bin/ssh、bin/scp、bin/ssh-keygen、
#      bin/ssh-keyscan 都必须在 5 秒内返回（旧包在这里挂死）；
#   2. strace 数 10 秒内的 execve 次数：旧包是几百次（无限自我调用），
#      新包应当只有个位数；
#   3. 自检脚本退出码 0，且 ssh/rsync 标注的是"系统自带"；
#   4. 自检 --quiet 的输出行数是个位数（旧包 60+ 行）；
#   5. run.sh 从任意工作目录都能起来；
#   6. 程序本体 --selfcheck 退出码 0。
# =============================================================================
set -uo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C.UTF-8

TARGET="$1"; shift
OLD_TAR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --old-tar) OLD_TAR=$2; shift 2 ;;
        *) shift ;;
    esac
done

WORK=/tmp/srt-verify
rm -rf "$WORK"; mkdir -p "$WORK"

if [ -f "$TARGET" ]; then
    echo "===== 验证目标（tar）：$TARGET ====="
    tar -xzf "$TARGET" -C "$WORK"
    PKG=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d | head -1)
else
    PKG="$TARGET"
    echo "===== 验证目标（目录）：$PKG ====="
fi
[ -n "$PKG" ] && [ -d "$PKG" ] || { echo "!!! 找不到包"; exit 1; }
echo "  包根目录：$(basename "$PKG")"
echo "  条目数  ：$(find "$PKG" | wc -l)"
echo

PASS=0
FAILN=0
res() { # res <0|1> <说明>
    if [ "$1" = 0 ]; then
        printf '  \033[32mPASS\033[0m %s\n' "$2"; PASS=$((PASS+1))
    else
        printf '  \033[31mFAIL\033[0m %s\n' "$2"; FAILN=$((FAILN+1))
    fi
}

# ------------------------------------------------------------ 1. wrapper 不挂死
echo "--- 1. bin/ 下的 wrapper 在 PATH 优先时是否挂死（P0-1 / P0-2）---"
check_tool() { # check_tool <名字> <参数...>
    _name=$1; shift
    _t0=$(date +%s)
    _out=$(PATH="$PKG/bin:$PATH" timeout 5 "$PKG/bin/$_name" "$@" 2>&1)
    _rc=$?
    _t1=$(date +%s)
    _dt=$((_t1 - _t0))
    printf '    %-14s rc=%-3s %ss  %s\n' "$_name" "$_rc" "$_dt" "$(printf '%s' "$_out" | head -1 | cut -c1-70)"
    if [ "$_rc" = 124 ]; then
        res 1 "$_name 超时（5 秒未返回）—— 无限 exec 循环"
    elif [ "$_rc" = 127 ]; then
        res 1 "$_name 找不到（回落失败）"
    else
        res 0 "$_name ${_dt}s 内返回（rc=$_rc）"
    fi
}
check_tool rsync --version
check_tool ssh -V
check_tool scp -?
check_tool ssh-keygen -?
check_tool ssh-keyscan -?

# 程序真正起进程的方式：**裸命令名** + PATH 里 bin/ 排最前
# （RsyncRunner 里还会再前置一次 appDir/appBin）。
# 这一条比上面那条更接近真实调用路径，必须单独验。
echo "    -- 按程序的真实调用方式（裸命令名 + PATH 优先）--"
_bare() { # _bare <命令> <参数...>
    _b_name=$1; shift
    _b_out=$(cd "$PKG" && PATH="$PKG/bin:$PATH" timeout 5 "$_b_name" "$@" 2>&1)
    _b_rc=$?
    printf '    %-14s rc=%-3s %s\n' "$_b_name" "$_b_rc" "$(printf '%s' "$_b_out" | head -1 | cut -c1-70)"
    if [ "$_b_rc" = 124 ]; then
        res 1 "裸命令名 $_b_name 超时（无限 exec 循环）"
    elif [ "$_b_rc" = 127 ]; then
        res 1 "裸命令名 $_b_name 找不到"
    else
        res 0 "裸命令名 $_b_name 正常返回（rc=$_b_rc）"
    fi
}
_bare rsync --version
_bare ssh -V
# 模拟 RsyncRunner 把 appDir/bin 再前置一次的情况（同一个目录，不同拼法）
_bare2_out=$(cd "$PKG" && PATH="$PKG/bin/.:$PATH" timeout 5 rsync --version 2>&1); _bare2_rc=$?
if [ "$_bare2_rc" = 124 ]; then
    res 1 "PATH 里 bin/ 用相对路径拼写时挂死"
else
    res 0 "PATH 里 bin/ 用相对路径拼写时也正常（rc=$_bare2_rc）"
fi

# ------------------------------------------------------------ 2. strace execve 计数
echo
echo "--- 2. strace 数 execve 次数（10 秒窗口）---"
if command -v strace >/dev/null 2>&1; then
    _st=/tmp/srt-execve-$$.log
    _st2=""   # --old-tar not given -> stays empty (set -u safe)
    PATH="$PKG/bin:$PATH" strace -f -e trace=execve -o "$_st" \
        timeout 10 "$PKG/bin/rsync" --version >/dev/null 2>&1
    _n=$(grep -c 'execve(' "$_st" 2>/dev/null || echo 0)
    echo "    新包 bin/rsync：execve 次数 = $_n"
    grep 'execve(' "$_st" 2>/dev/null | head -8 | sed 's/^/      /'
    if [ "$_n" -le 20 ]; then
        res 0 "新包无 exec 循环（$_n 次 execve）"
    else
        res 1 "新包疑似仍有 exec 循环（$_n 次 execve）"
    fi

    # 旧包对照（如果给了旧包）
    if [ -n "$OLD_TAR" ] && [ -f "$OLD_TAR" ]; then
        _od=/tmp/srt-old-$$
        rm -rf "$_od"; mkdir -p "$_od"
        tar -xzf "$OLD_TAR" -C "$_od" 2>/dev/null
        _or=$(find "$_od" -mindepth 1 -maxdepth 1 -type d | head -1)
        if [ -n "$_or" ] && [ -x "$_or/bin/rsync" ]; then
            _st2=/tmp/srt-execve-old-$$.log
            PATH="$_or/bin:$PATH" strace -f -e trace=execve -o "$_st2" \
                timeout 10 "$_or/bin/rsync" --version >/dev/null 2>&1
            _n2=$(grep -c 'execve(' "$_st2" 2>/dev/null || echo 0)
            echo "    旧包 bin/rsync：execve 次数 = $_n2 （10 秒内）"
            grep 'execve(' "$_st2" 2>/dev/null | head -4 | sed 's/^/      /'
            if [ "$_n2" -gt 100 ]; then
                res 0 "旧包对照组复现了 exec 循环（$_n2 次）—— 证明本修复针对的确实是这个缺陷"
            else
                echo "    （旧包对照组未复现，数量偏小）"
            fi
        fi
    fi
    rm -f "$_st" ${_st2:+"$_st2"} 2>/dev/null
else
    echo "    容器里没有 strace，跳过"
fi

# ------------------------------------------------------------ 3. 自检
echo
echo "--- 3. 自检脚本 ---"
_sc_out=$(cd "$PKG" && sh ./自检.sh 2>&1); _sc_rc=$?
echo "$_sc_out" | sed 's/^/    | /'
if [ "$_sc_rc" = 0 ]; then res 0 "自检退出码 0"; else res 1 "自检退出码 $_sc_rc"; fi
if echo "$_sc_out" | grep -q '系统自带'; then
    res 0 "自检把 ssh/rsync 标注为「系统自带」（不是谎报包内自带）"
else
    res 1 "自检没有标注「系统自带」"
fi
if echo "$_sc_out" | grep -q '包内自带'; then
    res 1 "自检仍然出现「包内自带」字样"
else
    res 0 "自检里没有「包内自带」的谎报"
fi
_q=$(cd "$PKG" && sh ./自检.sh --quiet 2>&1); _qn=$(printf '%s\n' "$_q" | grep -c .)
echo "    --quiet 输出行数 = $_qn"
echo "$_q" | sed 's/^/    | /'
if [ "$_qn" -le 12 ]; then res 0 "--quiet 输出只有 $_qn 行（改前 60+ 行）"; else res 1 "--quiet 输出 $_qn 行，仍偏多"; fi

# ------------------------------------------------------------ 4. run.sh
echo
echo "--- 4. run.sh 从任意工作目录启动 ---"
mkdir -p /tmp/srt-verify-cwd
( cd /tmp/srt-verify-cwd && sh "$PKG/run.sh" --selfcheck ) >/tmp/srt-run.out 2>&1
_run_rc=$?
tail -3 /tmp/srt-run.out | sed 's/^/    | /'
if [ "$_run_rc" = 0 ]; then res 0 "run.sh 从 /tmp 启动成功（rc=0）"; else res 1 "run.sh 从 /tmp 启动返回 $_run_rc"; fi

# ------------------------------------------------------------ 5. 程序本体
echo
echo "--- 5. 程序本体离屏自检 ---"
export XDG_RUNTIME_DIR=/tmp/srt-rt-$$
mkdir -p "$XDG_RUNTIME_DIR"
LD_LIBRARY_PATH="$PKG/lib" QT_PLUGIN_PATH="$PKG/plugins" \
QT_QPA_PLATFORM_PLUGIN_PATH="$PKG/plugins/platforms" \
QT_QPA_PLATFORM=offscreen \
    "$PKG/SyncReplayTool" --selfcheck >/tmp/srt-sc.out 2>/tmp/srt-sc.err
_sc2=$?
tail -4 /tmp/srt-sc.out | sed 's/^/    | /'
if [ "$_sc2" = 0 ]; then res 0 "SyncReplayTool --selfcheck 退出码 0"; else res 1 "SyncReplayTool --selfcheck 退出码 $_sc2"; fi

# ------------------------------------------------------------ 6. 包内容
echo
echo "--- 6. 包内容体检 ---"
for f in run.sh 自检.sh 安装.sh 数据同步工具.desktop qt.conf 使用步骤与注意事项.md SyncReplayTool; do
    if [ -e "$PKG/$f" ]; then res 0 "含 $f"; else res 1 "缺 $f"; fi
done
for f in rsync ssh scp ssh-keygen ssh-keyscan; do
    if [ -x "$PKG/bin/$f" ]; then res 0 "含 bin/$f（可执行）"; else res 1 "缺/不可执行 bin/$f"; fi
done
[ -e "$PKG/libexec" ] && res 1 "libexec/ 还在包里（永不可用的死重）" || res 0 "libexec/ 已清掉"
[ -e "$PKG/lib/sshv2" ] && res 1 "lib/sshv2/ 还在包里（永不可用的死重）" || res 0 "lib/sshv2/ 已清掉"
for d in plugins/imageformats plugins/egldeviceintegrations; do
    [ -e "$PKG/$d" ] && res 1 "$d/ 还在包里（用不上）" || res 0 "$d/ 已清掉"
done
for f in plugins/platforms/libqlinuxfb.so plugins/platforms/libqvnc.so plugins/platforms/libqeglfs.so plugins/platformthemes/libqgtk3.so; do
    [ -e "$PKG/$f" ] && res 1 "$f 还在包里（用不上）" || res 0 "$f 已清掉"
done
_nsl=$(find "$PKG/lib" "$PKG/plugins" -type l 2>/dev/null | wc -l)
if [ "$_nsl" -ge 20 ]; then res 0 "lib/plugins 的符号链接保住了（$_nsl 个）"; else res 1 "符号链接只剩 $_nsl 个（打包时被展开了？）"; fi
if grep -rlq $'\r' "$PKG/run.sh" "$PKG/自检.sh" "$PKG/安装.sh" "$PKG/bin/"* 2>/dev/null; then
    res 1 "脚本里还有 CRLF 换行"
else
    res 0 "脚本都是 LF 换行"
fi
if grep -q 'srt-client' "$PKG/使用步骤与注意事项.md" 2>/dev/null; then
    res 1 "文档里还残留旧目录名 srt-client"
else
    res 0 "文档里的目录名与包一致"
fi
if grep -q 'libexec' "$PKG/使用步骤与注意事项.md" 2>/dev/null; then
    res 1 "文档里还在说程序在 libexec/（已删除）"
else
    res 0 "文档不再提 libexec/"
fi

# ------------------------------------------------------------ 7. 安装.sh
echo
echo "--- 7. 安装.sh（桌面图标）---"
_fakehome=/tmp/srt-fakehome
rm -rf "$_fakehome"; mkdir -p "$_fakehome"
_install_out=$(HOME="$_fakehome" XDG_DESKTOP_DIR="$_fakehome/桌面" sh "$PKG/安装.sh" 2>&1)
_ins_rc=$?
echo "$_install_out" | sed 's/^/    | /'
if [ "$_ins_rc" = 0 ]; then res 0 "安装.sh 退出码 0"; else res 1 "安装.sh 退出码 $_ins_rc"; fi
_desk=$(find "$_fakehome" -name '*.desktop' 2>/dev/null | head -1)
if [ -n "$_desk" ]; then
    res 0 "桌面图标已生成：${_desk##*/}"
    if grep -q "^Exec=sh \"$PKG/run.sh\"" "$_desk"; then
        res 0 "图标里的 Exec 已被改写成绝对路径"
    else
        res 1 "图标里的 Exec 没被改写成绝对路径（拷到桌面会失效）"
    fi
else
    res 1 "安装.sh 没有生成桌面图标"
fi
if echo "$_install_out" | grep -q '不要手工把包里的 .desktop 拷到桌面'; then
    res 0 "安装.sh 明确提示不要手工拷 .desktop"
else
    res 1 "安装.sh 没有那句提醒"
fi
# 桌面目录不存在时应当先建再建（原来的 mkdir 是死代码）
_fakehome2=/tmp/srt-fakehome2
rm -rf "$_fakehome2"; mkdir -p "$_fakehome2"
HOME="$_fakehome2" XDG_DESKTOP_DIR="$_fakehome2/新桌面" sh "$PKG/安装.sh" >/dev/null 2>&1
if [ -d "$_fakehome2/新桌面" ]; then
    res 0 "桌面目录不存在时会自动创建"
else
    res 1 "桌面目录不存在时没有创建"
fi

echo
echo "===== 结果：PASS=$PASS  FAIL=$FAILN ====="
[ "$FAILN" = 0 ] || exit 1
exit 0
