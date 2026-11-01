#!/bin/sh
# 启动脚本。**必须**用它启动，不要直接双击二进制 —— 直接跑 ./SyncReplayTool
# 会缺两个环境变量，Qt 找不到平台插件会 abort(134)，一个字符都不输出。
#
# 这里额外做四件让"双击能开"成立的事：
#   1. 自动定位自身目录（不依赖当前工作目录）
#   2. 没有图形界面时**说人话**并回落到离屏，而不是让 Qt 吐一句
#      "Could not connect to any X display" 然后退出
#   3. 自检失败时给出明确说明，但不擅自替客户决定放弃启动
#   4. 文件名编码兜底（非 UTF-8 会话下中文名会全变成乱码方块）

HERE=$(cd "$(dirname "$0")" && pwd)

# 文件名编码。Qt 用**当前 locale 的字符集**解码文件名，所以会话是 POSIX/C 时，
# 每一个非 ASCII 字节都会变成 U+FFFD。
#
# 实测（麒麟真 xcb + 真 sshd 端到端，同一批文件只改 locale）：
#   LANG 未设（POSIX） → 清单里写成 c_<7 个 U+FFFD>.spe，rsync link_stat 失败，
#                        中文文件名的文件**一个都传不过去**，界面只报"部分文件失败"
#   LANG=C.UTF-8      → c_中文.spe 正常到位
#   LANG=zh_CN.utf8   → c_中文.spe 正常到位
#
# 本脚本原先**完全不设置 locale**，于是"双击启动"由桌面会话带着 zh_CN.UTF-8 没事，
# 而 SSH、systemd、cron 这类没有 locale 的会话启动就必中。
# 因此：调用方的 locale 已经是 UTF-8 就原样保留，不是才挑一个系统里真实存在的。
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *UTF-8*|*utf8*|*UTF8*|*Utf8*) ;;
    *)
        _srt_loc_ok=0
        for _srt_loc in C.UTF-8 C.utf8 en_US.UTF-8 en_US.utf8 zh_CN.UTF-8 zh_CN.utf8; do
            if locale -a 2>/dev/null | grep -qx "$_srt_loc"; then
                LANG="$_srt_loc"; LC_ALL="$_srt_loc"
                export LANG LC_ALL
                _srt_loc_ok=1
                break
            fi
        done
        unset _srt_loc
        # 一个 UTF-8 locale 都找不到 —— **必须说出来**（2026-11-02 补，原 N6）。
        #
        # 原来这里是静默落回原 locale（POSIX/C）继续跑：
        # 程序照常起来、界面照常有中文，但**每一个中文文件名都会变成 U+FFFD**，
        # rsync link_stat 失败 → 这些文件一个都传不过去，
        # 界面只报"部分文件传输失败"，用户根本想不到是 locale 的锅。
        # 静默落回 = 静默丢数据，比直接报错坏得多。
        if [ "$_srt_loc_ok" -eq 0 ]; then
            echo "✗ 警告：这台机器上找不到任何 UTF-8 locale（当前 LANG=${LANG:-未设置}）"
            echo "  中文文件名在传输时会变成乱码方块，这些文件将**传不过去**，"
            echo "  界面只会显示「部分文件传输失败」，很容易被误判成网络或磁盘问题。"
            echo "  请管理员安装其一："
            echo "    麒麟 / openEuler：sudo yum install glibc-langpack-zh   （或 glibc-common）"
            echo "    Ubuntu / Debian：sudo apt install locales && locale-gen C.UTF-8"
            echo "  装完之后重新启动本程序即可。"
            echo
        fi
        unset _srt_loc_ok
        ;;
esac


# 架构前置检查：x86_64 的运行库在 ARM 机器上根本加载不了，
# 硬启动只会 abort(134)，客户看到的是"闪一下就没了 + 没有任何原因"。
# 所以在这里直接拦下，并把话说清楚（与 自检.sh 同一句口径）。
_srt_arch=$(uname -m 2>/dev/null || echo unknown)
case "$_srt_arch" in
    x86_64|amd64) ;;
    *)
        echo "✗ CPU 架构：$_srt_arch —— 本包是 x86_64 版，装错机器了"
        echo "  请换一台 x86_64（amd64）机器，或联系供应商索取对应架构的版本。"
        exit 1
        ;;
esac

# 自检：环境不对就立刻说清楚，别让用户对着"闪一下就没了"猜。
#
# 自检失败**不再直接退出**。实测（麒麟 V10，glibc 2.23）：界面本身能正常渲染，
# 但自带的 ssh/rsync 因要求 glibc 2.38 起不来。老版本在这里 exit，客户的
# 表现是"软件完全打不开"，而真实情况只是少了个可选的自带组件。
#
# 现在的做法：把自检结论原样打出来，再继续启动。真的跑不起来的话，
# 图形界面自己会失败，而那时屏幕上已经有自检的结论可看了，
# 不再是"闪一下就没了 + 没有任何原因"。
#
# 用 sh 调而不是直接执行：有的拷贝方式会丢掉可执行位，
# 那样连自检都跑不了，更谈不上"说清楚"。

# XDG_RUNTIME_DIR 必须在**调用 自检.sh 之前**准备好。
#
# 自检.sh 按「XDG_RUNTIME_DIR → TMPDIR → /tmp → 包内 .srt-tmp」挑一个可写的临时目录。
# 原来这个变量是在自检.sh **之后**才导出的 ⇒ 自检.sh 跑的时候它还是空的
# ⇒ 自检.sh 就在**包目录里**建了 `.srt-tmp` ⇒ 那个目录不属于 dpkg/rpm 的文件清单
# ⇒ 卸载时被留下（dpkg 会报 "directory '/opt/syncreplaytool' not empty so not removed"，
#    客户卸载完还能看到 /opt/syncreplaytool 残留）。
# 提前导出这一行，残留问题直接消失；顺带普通用户没权限在 /opt 下建目录时的行为差异也没了。
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null

if [ -f "$HERE/自检.sh" ]; then
    if ! sh "$HERE/自检.sh" --quiet; then
        echo
        echo "上面带 ✗ 的项目表示这台机器的环境有缺失。"
        echo "现在仍然尝试启动 —— 界面能打开就说明主要部分可用；"
        echo "如果窗口没出现，请把上面这段结论发给系统管理员。"
        echo
    fi
fi

# 运行库路径。**不要**再带 $HERE/lib/rsync —— 包里根本没有这个目录，
# 那是一处历史遗留（写上去不报错，但没有任何作用，只会让人以为还有第二套库）。
export LD_LIBRARY_PATH="$HERE/lib:${LD_LIBRARY_PATH:-}"

# 自带工具必须先进 PATH，否则 rsync 起不来。
#
# Qt 5.9 用 execvpe() 起子进程，而 glibc 的 execvpe 是拿**调用方自己**的 PATH
# 去解析裸命令名的，不是拿环境变量里传进来的那个。于是 RsyncRunner 里注入到
# 子进程环境的那段 appDir/bin 对"第一次 exec"完全无效 —— 而第一次 exec 就是
# rsync 自己。实测：系统没装 rsync 的机器上，run.sh 不导 PATH 就一个文件都传不出去，
# 而自检却报"环境满足"。
#
# 放在这里导出，桌面图标（Exec=sh "<包目录>/run.sh"）和终端里 sh run.sh
# 两条客户路径就都覆盖到了。放到子进程环境里那条路继续留着 ——
# rsync 自己的子进程（也就是 ssh）确实要走它，那部分是对的。
#
# bin/rsync 和 bin/ssh 自己带回落：自带的跑不起来时会改用系统那份，
# 所以老系统（麒麟 V10）上不会因为 PATH 里排在前面的那个坏了就彻底不能用。
#
# 【配套】bin/ 里的 wrapper 回落时会把 PATH 里的自己剔掉再找系统那一份，
# 并用绝对路径 exec —— 否则 "command -v rsync" 找到的就是 wrapper 自己，
# exec 出去又是自己，无限 exec 循环（实测 5 秒内 400+ 次 execve，
# 界面永远停在"同步中"）。**这两处必须配套改，只改一处不管用。**
export PATH="$HERE/bin:$PATH"

# 第二行是必须的：只给 LD_LIBRARY_PATH 时，Qt 找不到 platforms/libqoffscreen.so
export QT_PLUGIN_PATH="$HERE/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms"

# 没有 X 也不直接放弃：先给提示，再落到离屏，保证至少能看到界面/日志
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    if [ -z "${QT_QPA_PLATFORM:-}" ]; then
        echo "提示：没有检测到图形界面（DISPLAY 为空）。"
        echo "     这通常意味着通过 SSH 远程启动，或这台机器没装桌面环境。"
        echo "     本工具是图形界面程序，需要在**带桌面的机器上**双击运行。"
        echo "     现在先用离屏模式启动，日志会输出在这里。"
        echo
        export QT_QPA_PLATFORM=offscreen
    fi
fi

cd "$HERE" || exit 1
exec ./SyncReplayTool "$@"
